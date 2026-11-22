#include "stdafx.h"
#include "Shader.hpp"
#include <Graphics/ResourceManagers.hpp>
#include "OpenGL.hpp"

namespace Graphics
{
#ifdef EMBEDDED
	// GL_GEOMETRY_SHADER only exists on GLES 3.2 contexts; the ES 3.0 headers
	// don't declare it.
	#ifndef GL_GEOMETRY_SHADER
	#define GL_GEOMETRY_SHADER 0x8DD9
	#endif

	// Shader stage -> GL shader type; returns 0 when the stage can't be used
	// on this context (geometry needs GLES 3.2+)
	static uint32 EmbeddedShaderType(ShaderType type, bool gles32)
	{
		switch (type)
		{
		case ShaderType::Vertex:
			return GL_VERTEX_SHADER;
		case ShaderType::Fragment:
			return GL_FRAGMENT_SHADER;
		case ShaderType::Geometry:
			return gles32 ? (uint32)GL_GEOMETRY_SHADER : 0;
		}
		return 0;
	}
#else
	const uint32 typeMap[] =
	{
		GL_VERTEX_SHADER,
		GL_FRAGMENT_SHADER,
		GL_GEOMETRY_SHADER,
	};
	const uint32 shaderStageMap[] =
	{
		GL_VERTEX_SHADER_BIT,
		GL_FRAGMENT_SHADER_BIT,
		GL_GEOMETRY_SHADER_BIT,
	};
#endif
	class Shader_Impl : public ShaderRes
	{
		ShaderType m_type;
		uint32 m_prog = 0;
		OpenGL* m_gl;

		String m_sourcePath;
		Ref<Buffer> m_sourceBytes;

		// Hot Reload detection on windows
#ifdef _WIN32
		HANDLE m_changeNotification = INVALID_HANDLE_VALUE;
		uint64 m_lwt = -1;
#endif

		// Uniform default values (from GLSL initializers, stripped by the ES translator)
		Vector<ShaderUniformDefault> m_uniformDefaults;

	public:
		Shader_Impl(OpenGL* gl) : m_gl(gl)
		{
		}
		~Shader_Impl()
		{
			// Cleanup OpenGL resource
			if(glIsProgram(m_prog))
			{
				glDeleteProgram(m_prog);
			}

#ifdef _WIN32
			// Close change notification handle
			if(m_changeNotification != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_changeNotification);
			}
#endif
		}
		void SetupChangeHandler()
		{
#ifdef _WIN32
			if(m_changeNotification != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_changeNotification);
				m_changeNotification = INVALID_HANDLE_VALUE;
			}

			WString rootFolder = Utility::ConvertToWString(Path::RemoveLast(m_sourcePath));
			m_changeNotification = FindFirstChangeNotificationW(*rootFolder, false, FILE_NOTIFY_CHANGE_LAST_WRITE);
#endif
		}

#ifdef EMBEDDED
		// GLSL ES 100 has no implicit int->float conversion, but desktop skin
		// shaders freely mix them (e.g. "x / 2", "vec4(0)"). Convert integer
		// literals used as arithmetic operands / call arguments to float
		// literals, while keeping true int contexts (int declarations, ivec
		// constructors, array indices, comparison operators) untouched.
		static String ConvertIntLiteralsToFloat(const String& line)
		{
			String out;
			size_t n = line.size();
			size_t i = 0;
			while (i < n)
			{
				char c = line[i];
				if (isdigit((unsigned char)c) && (i == 0 || !(isalnum((unsigned char)line[i - 1]) || line[i - 1] == '_' || line[i - 1] == '.')))
				{
					size_t j = i;
					while (j < n && isdigit((unsigned char)line[j])) j++;
					// float literal (1.0, 1e-3, 1.0e-10) -> copy verbatim, including exponent
					if (j < n && (line[j] == '.' || line[j] == 'e' || line[j] == 'E'))
					{
						size_t j2 = j;
						while (j2 < n && (isdigit((unsigned char)line[j2]) || line[j2] == '.')) j2++;
						if (j2 < n && (line[j2] == 'e' || line[j2] == 'E'))
						{
							j2++;
							if (j2 < n && (line[j2] == '+' || line[j2] == '-')) j2++;
							while (j2 < n && isdigit((unsigned char)line[j2])) j2++;
						}
						out += line.substr(i, j2 - i);
						i = j2;
						continue;
					}
					String lit = line.substr(i, j - i);
					// previous significant token in `out`
					size_t k = out.size();
					while (k > 0 && isspace((unsigned char)out[k - 1])) k--;
					String prevOp;
					if (k >= 2 && (out.compare(k - 2, 2, "==") == 0 || out.compare(k - 2, 2, "!=") == 0 ||
						out.compare(k - 2, 2, "<=") == 0 || out.compare(k - 2, 2, ">=") == 0 ||
						out.compare(k - 2, 2, "&&") == 0 || out.compare(k - 2, 2, "||") == 0))
						prevOp = out.substr(k - 2, 2);
					else if (k >= 1)
						prevOp = out.substr(k - 1, 1);
					bool convert = false;
					if (prevOp == "+" || prevOp == "-" || prevOp == "*" || prevOp == "/" || prevOp == "%" || prevOp == "=")
						convert = true; // arithmetic operand or assignment ("*=", "+=" end with these chars)
					else if (prevOp == "(" || prevOp == ",")
					{
						// enclosing function name (to skip ivec constructors)
						size_t p = out.rfind('(');
						if (p != std::string::npos)
						{
							size_t e = p;
							while (e > 0 && isspace((unsigned char)out[e - 1])) e--;
							size_t s2 = e;
							while (s2 > 0 && (isalnum((unsigned char)out[s2 - 1]) || out[s2 - 1] == '_')) s2--;
							String fname = out.substr(s2, e - s2);
							convert = !(fname.size() >= 4 && fname.compare(0, 4, "ivec") == 0);
						}
						else
							convert = true;
					}
					// '[', comparisons, ';', start-of-line -> keep as int
					if (convert)
						out += lit + ".0";
					else
						out += lit;
					i = j;
					continue;
				}
				out += c;
				i++;
			}
			// strip invalid GLSL ES 'f' float suffixes (0.3f -> 0.3)
			String res;
			for (size_t i = 0; i < out.size(); i++)
			{
				if (out[i] == 'f' && i > 0 && isdigit((unsigned char)out[i - 1]) &&
					(i + 1 >= out.size() || !(isalnum((unsigned char)out[i + 1]) || out[i + 1] == '_')))
					continue; // drop the 'f'
				res += out[i];
			}
			return res;
		}

		// GLSL ES 100 does not support uniform initializers ("uniform int x = 1;"),
		// which desktop skins use to declare default parameter values.
		// Strips them from the shader source and records the values so
		// Material_Impl can apply them with glUniform calls after linking.
		static String TrimString(String s)
		{
			size_t a = s.find_first_not_of(" \t\r");
			if (a == std::string::npos)
				return String();
			size_t b = s.find_last_not_of(" \t\r");
			return s.substr(a, b - a + 1);
		}

		// Desktop GLSL allows global variables initialized from non-const
		// expressions, including uniforms ("vec2 t = vec2(uX, uY);"). GLSL ES
		// (every version) rejects that: "Only consts can be used in a global
		// initializer". Split such declarations into a bare declaration plus an
		// assignment that the caller hoists into main(); this preserves the
		// desktop semantics (globals are initialized at program start in
		// declaration order, and main() runs after that).
		static bool SplitGlobalInitializer(const String& line, String& declOut, String& initOut)
		{
			static const char* const types[] = {
				"float", "vec2", "vec3", "vec4", "int", "ivec2", "ivec3", "ivec4",
				"bool", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4"
			};
			size_t sp = line.find(' ');
			if (sp == String::npos || sp == 0)
				return false;
			String type = line.substr(0, sp);
			bool validType = false;
			for (const char* t : types)
			{
				if (type == t)
				{
					validType = true;
					break;
				}
			}
			if (!validType)
				return false;

			size_t eq = line.find('=');
			if (eq == String::npos || eq < sp)
				return false;
			size_t semi = line.find(';', eq);
			if (semi == String::npos)
				return false;

			size_t nameStart = sp + 1;
			while (nameStart < line.size() && line[nameStart] == ' ')
				nameStart++;
			size_t nameEnd = eq;
			while (nameEnd > nameStart && isspace((unsigned char)line[nameEnd - 1]))
				nameEnd--;
			String name = line.substr(nameStart, nameEnd - nameStart);
			if (name.empty())
				return false;
			for (char c : name)
			{
				if (!(isalnum((unsigned char)c) || c == '_'))
					return false; // arrays / complex declarations not supported
			}

			declOut = type + " " + name + ";";
			initOut = name + " = " + TrimString(line.substr(eq + 1, semi - eq - 1)) + ";";
			return true;
		}

		// Inserts a set of hoisted global-initializer statements at the top of
		// main(). No-op when the shader has no main() (they are dropped, which
		// just fails compilation like the untranslated shader would).
		static void HoistIntoMain(String& out, const Vector<String>& hoistedInits)
		{
			if (hoistedInits.empty())
				return;
			size_t mainPos = out.find("void main(");
			if (mainPos == String::npos)
				return;
			size_t bracePos = out.find('{', mainPos);
			if (bracePos == String::npos)
				return;
			String ins = "\n";
			for (const String& h : hoistedInits)
				ins += "\t" + h + "\n";
			out.insert(bracePos + 1, ins);
		}

		// Evaluates a GLSL constant expression used as a uniform initializer.
		// Supports scalars, bools, float(n)/int(n) casts and vecN/ivecN/bvecN constructors.
		static bool ParseInitializerValue(String value, uint8& numComponents, double values[4])
		{
			value = TrimString(value);
			if (value.empty())
				return false;

			size_t open = value.find('(');
			if (open != String::npos)
			{
				String fn = TrimString(value.substr(0, open));
				size_t close = value.rfind(')');
				if (close == String::npos || close != value.size() - 1 || close < open)
					return false;
				uint8 want = 0;
				if (fn == "float" || fn == "int" || fn == "bool")
					want = 1;
				else if (fn == "vec2" || fn == "ivec2" || fn == "bvec2")
					want = 2;
				else if (fn == "vec3" || fn == "ivec3" || fn == "bvec3")
					want = 3;
				else if (fn == "vec4" || fn == "ivec4" || fn == "bvec4")
					want = 4;
				else
					return false;

				String args = value.substr(open + 1, close - open - 1);
				// split top-level commas
				Vector<String> parts;
				{
					int depth = 0;
					size_t start = 0;
					for (size_t i = 0; i <= args.size(); i++)
					{
						if (i == args.size() || (args[i] == ',' && depth == 0))
						{
							parts.Add(args.substr(start, i - start));
							start = i + 1;
						}
						else if (args[i] == '(') depth++;
						else if (args[i] == ')') depth--;
					}
				}
				// single-component constructors also splat: vec4(x)
				if (!(parts.size() == want || (parts.size() == 1 && want > 1)))
					return false;
				for (size_t i = 0; i < want; i++)
				{
					uint8 n = 0;
					double v[4] = {};
					if (!ParseInitializerValue(parts[i < parts.size() ? i : 0], n, v) || n != 1)
						return false;
					values[i] = v[0];
				}
				numComponents = want;
				return true;
			}

			if (value == "true" || value == "false")
			{
				values[0] = (value == "true") ? 1.0 : 0.0;
				numComponents = 1;
				return true;
			}

			char* end = nullptr;
			double v = strtod(value.c_str(), &end);
			if (end == value.c_str() || !TrimString(String(end)).empty())
				return false;
			values[0] = v;
			numComponents = 1;
			return true;
		}

		/*
			Rewrites a uniform declaration line, stripping any initializers
			("uniform int a = 1, b = 2;" -> "uniform int a, b;") and recording
			the values in 'defaults'.
			'line' must already be left-trimmed.
			Returns false (leaving 'line' untouched) if the line is not a
			uniform declaration with an initializer.
		*/
		static bool StripUniformInitializers(const String& line, String& rewritten, Vector<ShaderUniformDefault>& defaults)
		{
			if (line.compare(0, 7, "uniform") != 0)
				return false;
			size_t p = 7;
			if (p < line.size() && line[p] != ' ' && line[p] != '\t')
				return false;
			while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;

			static const char* const validTypes[] = { "int", "float", "bool",
				"vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "bvec2", "bvec3", "bvec4" };
			size_t typeStart = p;
			while (p < line.size() && (isalnum((unsigned char)line[p]) || line[p] == '_')) p++;
			String type = line.substr(typeStart, p - typeStart);
			bool validType = false;
			for (const char* t : validTypes)
			{
				if (type == t)
				{
					validType = true;
					break;
				}
			}
			if (!validType)
				return false;

			size_t semi = line.find(';', p);
			if (semi == String::npos)
				return false;
			String declList = line.substr(p, semi - p);

			// split top-level commas ','
			Vector<String> segments;
			{
				int depth = 0;
				size_t start = 0;
				for (size_t i = 0; i <= declList.size(); i++)
				{
					if (i == declList.size() || (declList[i] == ',' && depth == 0))
					{
						segments.Add(declList.substr(start, i - start));
						start = i + 1;
					}
					else if (declList[i] == '(') depth++;
					else if (declList[i] == ')') depth--;
				}
			}

			bool anyInitializer = false;
			String rewrittenBody;
			for (const String& segRaw : segments)
			{
				String seg = TrimString(segRaw);
				if (seg.empty())
					return false;
				size_t eq = seg.find('=');
				String name = TrimString(eq == String::npos ? seg : seg.substr(0, eq));
				if (name.empty() || !(isalpha((unsigned char)name[0]) || name[0] == '_'))
					return false;
				for (char c : name)
				{
					if (!(isalnum((unsigned char)c) || c == '_'))
						return false;
				}
				if (!rewrittenBody.empty())
					rewrittenBody += ", ";
				rewrittenBody += name;

				if (eq != String::npos)
				{
					anyInitializer = true;
					ShaderUniformDefault def;
					def.name = name;
					uint8 n = 0;
					double v[4] = {};
					String init = TrimString(seg.substr(eq + 1));
					if (ParseInitializerValue(init, n, v))
					{
						def.numComponents = n > 4 ? 4 : n;
						for (size_t k = 0; k < 4; k++)
							def.values[k] = k < n ? v[k] : 0.0;
					}
					else
					{
						Logf("Unsupported uniform initializer '%s = %s', using 0 instead",
							Logger::Severity::Warning, name, init);
					}
					defaults.Add(def);
				}
			}
			if (!anyInitializer)
				return false;

			rewritten = "uniform " + type + " " + rewrittenBody + line.substr(semi);
			return true;
		}

		// Translate desktop GLSL 330 skin shaders to GLSL ES 100 (GLES2 has no
		// layout qualifiers, in/out keywords, gl_PerVertex blocks or texelFetch)
		static String TranslateDesktopGLSLToES100(const String& src, bool isVertex, Vector<ShaderUniformDefault>& uniformDefaults)
		{
			String out;
			size_t pos = 0;
			bool skipPerVertex = false;
			bool needMapSize = false;
			int braceDepth = 0; // 0 == global scope
			Vector<String> hoistedInits; // global initializers moved into main()
			while (pos < src.size())
			{
				size_t eol = src.find('\n', pos);
				if (eol == std::string::npos) eol = src.size();
				String line = src.substr(pos, eol - pos);
				pos = eol + 1;

				size_t first = line.find_first_not_of(" \t\r");
				String lead = (first == std::string::npos) ? String() : line.substr(0, first);
				String rest = (first == std::string::npos) ? String() : line.substr(first);

				// Track brace depth (comment-safe) so global-scope detection
				// below can ignore declarations inside function bodies
				{
					size_t cm = rest.find("//");
					String code = (cm == String::npos) ? rest : rest.substr(0, cm);
					for (char c : code)
					{
						if (c == '{') braceDepth++;
						else if (c == '}') braceDepth--;
					}
				}

				if (skipPerVertex)
				{
					if (rest.find("};") != std::string::npos || (!rest.empty() && rest[0] == '}'))
						skipPerVertex = false;
					continue;
				}
				if (rest.compare(0, 8, "#version") == 0)
					continue; // replaced by the prepended "#version 100"
				if (rest.compare(0, 10, "#extension") == 0)
					continue; // desktop extensions don't exist in GLES2
				if (rest.compare(0, 15, "in gl_PerVertex") == 0 || rest.find("out gl_PerVertex") == 0)
				{
					skipPerVertex = true; // gl_Position is built-in in GLES2
					continue;
				}
				if (rest.compare(0, 6, "layout") == 0)
				{
					size_t inPos = rest.find(" in ");
					size_t outPos = rest.find(" out ");
					if (outPos == std::string::npos && rest.find(") in ") != std::string::npos)
						inPos = rest.find(") in ") + 1; // "layout(location=1) in vec2 x;"
					else if (inPos == std::string::npos && rest.find(") out ") != std::string::npos)
						outPos = rest.find(") out ") + 1; // "layout(location=0) out vec4 x;"
					if (inPos != std::string::npos)
					{
						out += lead + (isVertex ? "attribute " : "varying ") + rest.substr(inPos + 4) + "\n";
						continue;
					}
					if (outPos != std::string::npos)
					{
						// fragment shader output is dropped: 'target' is #defined to gl_FragColor
						if (isVertex)
							out += lead + "varying " + rest.substr(outPos + 5) + "\n";
						continue;
					}
				}
				// Desktop declarations without explicit layout, e.g. "out vec4 position;"
				// (arbitrary varyings used by skins) - 'in'/'out' don't exist in GLSL ES 100
				if (isVertex && rest.compare(0, 4, "out ") == 0)
				{
					out += lead + "varying " + rest.substr(4) + "\n";
					continue;
				}
				if (isVertex && rest.compare(0, 3, "in ") == 0)
				{
					out += lead + "attribute " + rest.substr(3) + "\n";
					continue;
				}
				if (!isVertex && rest.compare(0, 3, "in ") == 0)
				{
					out += lead + "varying " + rest.substr(3) + "\n";
					continue;
				}
				// texelFetch(tex, ivec2(uv), 0) -> texture2D(tex, uv / vec2(mapSize))
				size_t tf = rest.find("texelFetch(");
				if (tf != std::string::npos)
				{
					size_t open = rest.find('(', tf);
					size_t close = std::string::npos;
					if (open != std::string::npos)
					{
						int depth = 0;
						for (size_t i = open; i < rest.size(); i++)
						{
							if (rest[i] == '(') depth++;
							else if (rest[i] == ')')
							{
								depth--;
								if (depth == 0) { close = i; break; }
							}
						}
					}
					if (close != std::string::npos)
					{
						String args = rest.substr(open + 1, close - open - 1);
						// split top-level commas
						Vector<String> parts;
						int depth = 0; size_t start = 0;
						for (size_t i = 0; i <= args.size(); i++)
						{
							if (i == args.size() || (args[i] == ',' && depth == 0))
							{
								String p = args.substr(start, i - start);
								size_t f = p.find_first_not_of(" \t");
								if (f != std::string::npos) parts.Add(p.substr(f));
								start = i + 1;
							}
							else if (args[i] == '(') depth++;
							else if (args[i] == ')') depth--;
						}
						if (parts.size() >= 2)
						{
							String uv = parts[1];
							if (uv.compare(0, 6, "ivec2(") == 0 && uv.size() >= 7)
								uv = uv.substr(6, uv.size() - 7); // strip ivec2( ... )
							rest = rest.substr(0, tf) + "texture2D(" + parts[0] + ", " + uv + " / vec2(mapSize))" + rest.substr(close + 1);
							needMapSize = true;
						}
					}
				}
				// Strip desktop-only uniform initializers ("uniform int x = 0;")
				// and record them as program-link defaults
				{
					String rewritten;
					if (StripUniformInitializers(rest, rewritten, uniformDefaults))
						rest = rewritten;
				}
				// Global variables initialized from uniforms/non-const globals
				// are illegal on ES: split into declaration + main() assignment
				{
					String decl, init;
					if (braceDepth == 0 && SplitGlobalInitializer(rest, decl, init))
					{
						hoistedInits.Add(ConvertIntLiteralsToFloat(init));
						out += lead + decl + "\n";
						continue;
					}
				}
				// int literal -> float literal in expressions (skip int declarations)
				bool intDecl = (rest.compare(0, 4, "int ") == 0) || (rest.compare(0, 12, "uniform int ") == 0);
				if (!intDecl)
					rest = ConvertIntLiteralsToFloat(rest);
				out += lead + rest + "\n";
			}
			HoistIntoMain(out, hoistedInits);
			if (needMapSize && out.find("uniform ivec2 mapSize") == std::string::npos)
				out = "uniform ivec2 mapSize;\n" + out;
			return out;
		}

		// Parsed GL context version ("OpenGL ES 3.2 ..." -> 3, 2), cached
		static void GetGLESContextVersion(int& major, int& minor)
		{
			static int sMajor = -1;
			static int sMinor = 0;
			if (sMajor < 0)
			{
				sMajor = 0;
				const GLubyte* ver = glGetString(GL_VERSION);
				if (ver && strncmp((const char*)ver, "OpenGL ES ", 10) == 0)
				{
					const char* p = (const char*)ver + 10;
					sMajor = atoi(p);
					while (*p && *p != '.') p++;
					if (*p == '.')
						sMinor = atoi(p + 1);
				}
			}
			major = sMajor;
			minor = sMinor;
		}
		static bool HasGLES3Context()
		{
			int major = 0, minor = 0;
			GetGLESContextVersion(major, minor);
			return major >= 3;
		}
		static bool HasGLES32Context()
		{
			int major = 0, minor = 0;
			GetGLESContextVersion(major, minor);
			return major > 3 || (major == 3 && minor >= 2);
		}

		// Translate desktop GLSL (130..4xx) skin shaders to GLSL ES 300. Almost
		// 1:1 with desktop GLSL 330: layout qualifiers, in/out, texelFetch,
		// integer ops and real loops are all native; only the version line,
		// desktop-only #extension lines and uniform initializers need work.
		// Callers fall back to the ES 100 path if the result fails to compile.
		static String TranslateDesktopGLSLToES300(const String& src, Vector<ShaderUniformDefault>& uniformDefaults)
		{
			String out;
			size_t pos = 0;
			bool skipPerVertex = false;
			int braceDepth = 0; // 0 == global scope
			Vector<String> hoistedInits; // global initializers moved into main()
			while (pos < src.size())
			{
				size_t eol = src.find('\n', pos);
				if (eol == std::string::npos) eol = src.size();
				String line = src.substr(pos, eol - pos);
				pos = eol + 1;

				size_t first = line.find_first_not_of(" \t\r");
				String lead = (first == std::string::npos) ? String() : line.substr(0, first);
				String rest = (first == std::string::npos) ? String() : line.substr(first);

				// Track brace depth (comment-safe) so global-scope detection
				// below can ignore declarations inside function bodies
				{
					size_t cm = rest.find("//");
					String code = (cm == String::npos) ? rest : rest.substr(0, cm);
					for (char c : code)
					{
						if (c == '{') braceDepth++;
						else if (c == '}') braceDepth--;
					}
				}

				if (skipPerVertex)
				{
					if (!rest.empty() && rest[0] == '}')
						skipPerVertex = false; // closes "};" and "} gl_in[1];" alike
					continue;
				}
				if (rest.compare(0, 8, "#version") == 0)
					continue; // replaced by the prepended "#version 300 es"
				if (rest.compare(0, 10, "#extension") == 0)
					continue; // desktop extensions don't exist in GLES
				if (rest.compare(0, 15, "in gl_PerVertex") == 0 || rest.find("out gl_PerVertex") == 0)
				{
					skipPerVertex = true; // the built-in block must not be subset-redeclared in ES
					continue;
				}
				// GLSL ES 300 still has no uniform initializers
				{
					String rewritten;
					if (StripUniformInitializers(rest, rewritten, uniformDefaults))
						rest = rewritten;
				}
				// Global variables initialized from uniforms/non-const globals
				// are illegal on ES: split into declaration + main() assignment
				{
					String decl, init;
					if (braceDepth == 0 && SplitGlobalInitializer(rest, decl, init))
					{
						hoistedInits.Add(ConvertIntLiteralsToFloat(init));
						out += lead + decl + "\n";
						continue;
					}
				}
				// Desktop GL (and thus desktop skins) implicitly converts int
				// literals inside float expressions ("1/bgAr", "x/2"); GLSL ES
				// rejects this even in ES 300/320, so run the same conversion
				// pass as the ES 100 route. True int contexts (declarations,
				// loop counters, array indices) are kept by the pass itself.
				// layout(location=N) qualifiers must stay integers though, so
				// never run the conversion on layout lines.
				{
					bool isLayoutLine = rest.compare(0, 6, "layout") == 0;
					bool intDecl = (rest.compare(0, 4, "int ") == 0) || (rest.compare(0, 12, "uniform int ") == 0);
					if (!intDecl && !isLayoutLine)
						rest = ConvertIntLiteralsToFloat(rest);
				}
				out += lead + rest + "\n";
			}
			HoistIntoMain(out, hoistedInits);
			return out;
		}

		bool LoadProgram(uint32& programOut)
		{
			String sourceStr;
			File in;
			if (m_sourceBytes)
				sourceStr.assign((const char*)m_sourceBytes->data(), m_sourceBytes->size());
			else if (in.OpenRead(m_sourcePath))
			{
				sourceStr.resize(in.GetSize());
				in.Read(&sourceStr.front(), sourceStr.size());
			}
			else
				return false;
			if(sourceStr.size() == 0)
				return false;

			auto compileWithHeader = [this, &programOut](const String& src, const String& header) -> bool
			{
				String full = header + src;
				const GLint programsize = full.size();
				const char* pChars = *full;
				glShaderSource(programOut, 1, &pChars, &programsize);
				glCompileShader(programOut);
				int nStatus = 0;
				glGetShaderiv(programOut, GL_COMPILE_STATUS, &nStatus);
				return nStatus != GL_FALSE;
			};

			// Geometry shaders only exist as of GLES 3.2 (GLSL ES 320) and have
			// no possible ES 100 downgrade, so this either compiles or the caller
			// gracefully degrades (Application::LoadMaterial logs and skips)
			if (m_type == ShaderType::Geometry)
			{
				if (!HasGLES32Context())
				{
					Logf("Shader %s: geometry shaders require a GLES 3.2 context, skipping",
						Logger::Severity::Warning, m_sourcePath);
					return false;
				}
				m_uniformDefaults.clear();
				String es320 = TranslateDesktopGLSLToES300(sourceStr, m_uniformDefaults);
				if (compileWithHeader(es320, "#version 320 es\nprecision highp float;\nprecision highp int;\n"))
				{
					Logf("Shader %s compiled as GLSL ES 320 (geometry)", Logger::Severity::Info, m_sourcePath);
					return true;
				}
				static char infoLogBuffer[2048];
				int s = 0;
				glGetShaderInfoLog(programOut, sizeof(infoLogBuffer), &s, infoLogBuffer);
				Logf("Shader program compile log for %s: %s", Logger::Severity::Error, m_sourcePath, infoLogBuffer);
				return false;
			}

			// On a GLES 3 context, desktop skin shaders (GLSL 130+/330)
			// compile almost natively as GLSL ES 300/320, which supports layout
			// qualifiers, in/out, texelFetch, dynamic loops, integer ops etc.
			// Shaders with an explicit "#ifdef EMBEDDED" fallback (USC's own
			// shaders and mobile-aware skins) keep the GLSL ES 100 path.
			if (HasGLES3Context() && sourceStr.find("EMBEDDED") == String::npos)
			{
				m_uniformDefaults.clear();
				String es300 = TranslateDesktopGLSLToES300(sourceStr, m_uniformDefaults);
				String esHeader = HasGLES32Context()
					? "#version 320 es\nprecision highp float;\nprecision highp int;\n"
					: "#version 300 es\nprecision highp float;\nprecision highp int;\n";
				if (compileWithHeader(es300, esHeader))
				{
					Logf("Shader %s compiled as %s", Logger::Severity::Info, m_sourcePath,
						HasGLES32Context() ? "GLSL ES 320" : "GLSL ES 300");
					return true;
				}
				// Log why the ES 300/320 probe failed before degrading to ES 100
				{
					static char probeLog[2048];
					int s = 0;
					glGetShaderInfoLog(programOut, sizeof(probeLog), &s, probeLog);
					Logf("Shader %s GLSL ES 300/320 compile log: %s", Logger::Severity::Warning, m_sourcePath, probeLog);
				}
				Logf("Shader %s retrying as GLSL ES 100", Logger::Severity::Warning, m_sourcePath);
			}

			// GLSL ES 100 path: lossy downgrade of desktop shaders (and the
			// native path for USC's own dual-mode shaders)
			m_uniformDefaults.clear();
			sourceStr = TranslateDesktopGLSLToES100(sourceStr, m_type == ShaderType::Vertex, m_uniformDefaults);
			sourceStr = "#version 100\n#define EMBEDDED\n#define target gl_FragColor\n#define texture texture2D\nprecision mediump float;\n" + sourceStr;
			if (!compileWithHeader(sourceStr, String()))
			{
				static char infoLogBuffer[2048];
				int s = 0;
				glGetShaderInfoLog(programOut, sizeof(infoLogBuffer), &s, infoLogBuffer);

				Logf("Shader program compile log for %s: %s", Logger::Severity::Error, m_sourcePath, infoLogBuffer);
				return false;
			}

			// Shader hot-reload in debug mode
#if defined(_DEBUG) && defined(_WIN32)
			// Store last write time
			if (!m_sourceBytes)
			{
				m_lwt = in.GetLastWriteTime();
				SetupChangeHandler();
			}
#endif
			return true;
		}
#else
		
		bool LoadProgram(uint32& programOut)
		{
			String sourceStr;
			File in;
			if (m_sourceBytes)
				sourceStr.assign((const char*)m_sourceBytes->data(), m_sourceBytes->size());
			else if (in.OpenRead(m_sourcePath))
			{
				sourceStr.resize(in.GetSize());
				in.Read(&sourceStr.front(), sourceStr.size());
			}
			else
				return false;
			if(sourceStr.size() == 0)
				return false;
			String firstLine;
			sourceStr.Split("\n", &firstLine, nullptr);
			firstLine.Trim('\r');
			firstLine.ToLower();
			if (firstLine.compare("#version 330") != 0)
			{
				sourceStr = "#version 330\n" + sourceStr;
			}
			const char* pChars = *sourceStr;
			programOut = glCreateShaderProgramv(typeMap[(size_t)m_type], 1, &pChars);
			if(programOut == 0)
				return false;

			int nStatus = 0;
			glGetProgramiv(programOut, GL_LINK_STATUS, &nStatus);
			if(nStatus == 0)
			{
				static char infoLogBuffer[2048];
				int s = 0;
				glGetProgramInfoLog(programOut, sizeof(infoLogBuffer), &s, infoLogBuffer);

				Logf("Shader program compile log for %s: %s", Logger::Severity::Error, m_sourcePath, infoLogBuffer);
				return false;
			}

			// Shader hot-reload in debug mode
#if defined(_DEBUG) && defined(_WIN32)
			// Store last write time
			if (!m_sourceBytes)
			{
				m_lwt = in.GetLastWriteTime();
				SetupChangeHandler();
			}
#endif
			return true;
		}
		
#endif

		bool UpdateHotReload() override
		{
#ifdef _WIN32
			if(m_changeNotification != INVALID_HANDLE_VALUE)
			{
				if(WaitForSingleObject(m_changeNotification, 0) == WAIT_OBJECT_0)
				{
					uint64 newLwt = File::GetLastWriteTime(m_sourcePath);
					if(newLwt != -1 && newLwt > m_lwt)
					{
						uint32 newProgram = 0;
						if(LoadProgram(newProgram))
						{
							// Successfully reloaded
							m_prog = newProgram;
							return true;
						}
					}

					// Watch for new change
					SetupChangeHandler();
				}
			}
#endif
			return false;
		}

		const Vector<ShaderUniformDefault>& GetUniformDefaults() const override
		{
			return m_uniformDefaults;
		}

		bool Init(ShaderType type, const String& name)
		{
			m_sourcePath = Path::Normalize(name);
			m_type = type;

			#ifdef EMBEDDED
			uint32 glType = EmbeddedShaderType(type, HasGLES32Context());
			if (glType == 0)
				return false; // e.g. geometry shader on a GLES < 3.2 context
			m_prog = glCreateShader(glType);
			#endif

			return LoadProgram(m_prog);
		}
		bool Init(ShaderType type, const Resource& resource)
		{
			if (resource.IsPath())
				return Init(type, resource.GetPath());
			m_sourcePath = resource.GetName();
			m_sourceBytes = resource.GetBytes();
			m_type = type;
			#ifdef EMBEDDED
			uint32 glType = EmbeddedShaderType(type, HasGLES32Context());
			if (glType == 0)
			{
				m_sourceBytes.reset();
				return false;
			}
			m_prog = glCreateShader(glType);
			#endif
			bool loaded = LoadProgram(m_prog);
			m_sourceBytes.reset();
			return loaded;
		}
#ifndef EMBEDDED
		void Bind() override
		{
			if(m_gl->m_activeShaders[(size_t)m_type] != this)
			{
				glUseProgramStages(m_gl->m_mainProgramPipeline, shaderStageMap[(size_t)m_type], m_prog);
				m_gl->m_activeShaders[(size_t)m_type] = this;
			}
		}
		bool IsBound() const override
		{
			return m_gl->m_activeShaders[(size_t)m_type] == this;
		}
		uint32 GetLocation(const String& name) const override
		{
			return glGetUniformLocation(m_prog, name.c_str());
		}
		virtual void BindUniform(uint32 loc, const Transform& mat)
		{
			glProgramUniformMatrix4fv(m_prog, loc, 1, false, mat.mat);
		}
		virtual void BindUniformVec2(uint32 loc, const Vector2& v)
		{
			glProgramUniform2fv(m_prog, loc, 1, &v.x);
		}
		virtual void BindUniformVec3(uint32 loc, const Vector3& v)
		{
			glProgramUniform3fv(m_prog, loc, 1, &v.x);
		}
		virtual void BindUniformVec4(uint32 loc, const Vector4& v)
		{
			glProgramUniform4fv(m_prog, loc, 1, &v.x);
		}
		virtual void BindUniform(uint32 loc, int i)
		{
			glProgramUniform1i(m_prog, loc, i);
		}
		virtual void BindUniform(uint32 loc, float i)
		{
			glProgramUniform1f(m_prog, loc, i);
		}
		virtual void BindUniformArray(uint32 loc, const Transform* mat, size_t count)
		{
			glProgramUniformMatrix4fv(m_prog, loc, (int)count, false, (float*)mat);
		}
		virtual void BindUniformArray(uint32 loc, const Vector2* v2, size_t count)
		{
			glProgramUniform2fv(m_prog, loc, (int)count, (float*)v2);
		}
		virtual void BindUniformArray(uint32 loc, const Vector3* v3, size_t count)
		{
			glProgramUniform3fv(m_prog, loc, (int)count, (float*)v3);
		}
		virtual void BindUniformArray(uint32 loc, const Vector4* v4, size_t count)
		{
			glProgramUniform4fv(m_prog, loc, (int)count, (float*)v4);
		}
		virtual void BindUniformArray(uint32 loc, const float* i, size_t count)
		{
			glProgramUniform1fv(m_prog, loc, (int)count, i);
		}
		virtual void BindUniformArray(uint32 loc, const int* i, size_t count)
		{
			glProgramUniform1iv(m_prog, loc, (int)count, i);
		}
#endif
		virtual uint32 Handle() override
		{
			return m_prog;
		}

		String GetOriginalName() const override
		{
			return m_sourcePath;
		}
	};

	Shader ShaderRes::Create(class OpenGL* gl, ShaderType type, const String& assetPath)
	{
		Shader_Impl* pImpl = new Shader_Impl(gl);
		if(!pImpl->Init(type, assetPath))
		{
			delete pImpl;
			return Shader();
		}
		else
		{
			return GetResourceManager<ResourceType::Shader>().Register(pImpl);
		}
	}
	Shader ShaderRes::Create(class OpenGL* gl, ShaderType type, const Resource& resource)
	{
		Shader_Impl* pImpl = new Shader_Impl(gl);
		if(!pImpl->Init(type, resource))
		{
			delete pImpl;
			return Shader();
		}
		return GetResourceManager<ResourceType::Shader>().Register(pImpl);
	}
	void ShaderRes::Unbind(class OpenGL* gl, ShaderType type)
	{
		#ifndef EMBEDDED
		if(gl->m_activeShaders[(size_t)type] != 0)
		{
			glUseProgramStages(gl->m_mainProgramPipeline, shaderStageMap[(size_t)type], 0);
			gl->m_activeShaders[(size_t)type] = 0;
		}
		#endif
	}
}
