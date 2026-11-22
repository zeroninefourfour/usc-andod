#pragma once
#include <Graphics/ResourceTypes.hpp>

namespace Graphics
{
#ifndef EMBEDDED
	extern const uint32 shaderStageMap[];
#endif
	/* Enum of supported shader types */
	enum class ShaderType
	{
		Vertex,
		Fragment,
		Geometry
	};

	/*
		Default value for a uniform declared with an initializer (e.g. "uniform int x = 1;").
		Desktop GLSL supports initializers, GLSL ES 100 does not: the ES shader
		translator strips them and records the values here so they can be applied
		with glUniform calls after the program links.
	*/
	struct ShaderUniformDefault
	{
		String name;
		uint8 numComponents = 0; // 1-4, or 0 if the initializer could not be evaluated
		double values[4] = { 0.0, 0.0, 0.0, 0.0 };

		// Value for component index i, splatting single-component defaults and
		// padding missing components with 0
		double GetComponent(size_t i) const
		{
			if (numComponents == 0)
				return 0.0;
			if (numComponents == 1)
				return values[0];
			return i < numComponents ? values[i] : 0.0;
		}
	};

	/*
		A single unlinked OpenGL shader
	*/
	class ShaderRes
	{
	public:
		virtual ~ShaderRes() = default;
		static Ref<ShaderRes> Create(class OpenGL* gl, ShaderType type, const String& assetPath);
		static Ref<ShaderRes> Create(class OpenGL* gl, ShaderType type, const Resource& resource);
		static void Unbind(class OpenGL* gl, ShaderType type);
		friend class OpenGL;
	public:
		// Tries to hot-reload the shader program, only works if _DEBUG is defined
		// returns true if the program was changed and thus the handle value also changed
		virtual bool UpdateHotReload() = 0;
		#ifndef EMBEDDED
		virtual void Bind() = 0;
		[[nodiscard]]
		virtual bool IsBound() const = 0;
		[[nodiscard]]
		virtual uint32 GetLocation(const String& name) const = 0;
		[[nodiscard]]
		uint32 operator[](const char* name) const
		{
			return GetLocation(name);
		}
		#endif
		[[nodiscard]]
		virtual uint32 Handle() = 0;

		[[nodiscard]]
		virtual String GetOriginalName() const = 0;

		// Uniform default values from GLSL initializers, stripped by the ES shader translator
		[[nodiscard]]
		virtual const Vector<ShaderUniformDefault>& GetUniformDefaults() const = 0;
	};

	typedef Ref<ShaderRes> Shader;

	DEFINE_RESOURCE_TYPE(Shader, ShaderRes)
}