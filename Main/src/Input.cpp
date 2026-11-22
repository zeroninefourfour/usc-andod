#include "stdafx.h"
#include "Input.hpp"
#include "GameConfig.hpp"

#ifdef ANDROID
#include <jni.h>
#include <SDL_system.h>

// Short haptic pulse for touch button feedback, routed through the activity
// (com.tutorial.game.HelloSDL2Activity.hapticVibrate) which owns the Vibrator
// service on Android. Called on the SDL event thread; on any failure (no
// vibrator, JNI trouble) it silently does nothing.
static void TouchButtonHaptic()
{
	if (!g_gameConfig.GetBool(GameConfigKeys::TouchButtonVibration))
		return;
	JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
	jobject activity = (jobject)SDL_AndroidGetActivity();
	if (!env || !activity)
		return;
	jclass clazz = env->GetObjectClass(activity);
	if (clazz)
	{
		jmethodID method = env->GetStaticMethodID(clazz, "hapticVibrate", "(I)V");
		if (method)
			env->CallStaticVoidMethod(clazz, method, (jint)15);
		env->DeleteLocalRef(clazz);
	}
	env->DeleteLocalRef(activity);
}
#endif

Input::~Input()
{
	// Shoud be set to null by Cleanup
	assert(!m_gamepad);
	assert(!m_window);
}

void Input::Init(Graphics::Window& wnd)
{
	Cleanup();
	m_window = &wnd;
	m_window->OnKeyPressed.Add(this, &Input::OnKeyPressed);
	m_window->OnKeyReleased.Add(this, &Input::OnKeyReleased);
	m_window->OnMouseMotion.Add(this, &Input::OnMouseMotion);
	m_window->OnTouchDown.Add(this, &Input::OnTouchDown);
	m_window->OnTouchMove.Add(this, &Input::OnTouchMove);
	m_window->OnTouchUp.Add(this, &Input::OnTouchUp);
	m_window->OnFocusChanged.Add(this, &Input::OnFocusChanged);

	memset(m_touchFingerIds, 0, sizeof(m_touchFingerIds));
	memset(m_touchActive, 0, sizeof(m_touchActive));
	memset(m_touchButton, 0xff, sizeof(m_touchButton));
	memset(m_touchButtonAlt, 0xff, sizeof(m_touchButtonAlt));
	memset(m_touchZone, 0xff, sizeof(m_touchZone));
	memset(m_touchLaser, 0xff, sizeof(m_touchLaser));
	memset(m_touchLastX, 0, sizeof(m_touchLastX));
	memset(m_touchLastY, 0, sizeof(m_touchLastY));
	m_touchLaserActive[0] = m_touchLaserActive[1] = false;
	m_touchLaserTarget[0] = m_touchLaserTarget[1] = 0.0f;
	m_touchLaserEst[0] = m_touchLaserEst[1] = 0.0f;

	m_lastMousePos[0] = m_window->GetMousePos().x;
	m_lastMousePos[1] = m_window->GetMousePos().y;

	m_laserDevice = g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::LaserInputDevice);
	m_buttonDevice = g_gameConfig.GetEnum<Enum_InputDevice>(GameConfigKeys::ButtonInputDevice);

	m_keySensitivity = g_gameConfig.GetFloat(GameConfigKeys::Key_Sensitivity);
	m_keyLaserReleaseTime = g_gameConfig.GetFloat(GameConfigKeys::Key_LaserReleaseTime);

	m_mouseAxisMapping[0] = g_gameConfig.GetInt(GameConfigKeys::Mouse_Laser0Axis);
	m_mouseAxisMapping[1] = g_gameConfig.GetInt(GameConfigKeys::Mouse_Laser1Axis);

	m_mouseSensitivity = CalculateRealMouseSens(g_gameConfig.GetFloat(GameConfigKeys::Mouse_Sensitivity));

	m_controllerAxisMapping[0] = g_gameConfig.GetInt(GameConfigKeys::Controller_Laser0Axis);
	m_controllerAxisMapping[1] = g_gameConfig.GetInt(GameConfigKeys::Controller_Laser1Axis);
	m_controllerSensitivity = g_gameConfig.GetFloat(GameConfigKeys::Controller_Sensitivity);
	m_controllerDeadzone = g_gameConfig.GetFloat(GameConfigKeys::Controller_Deadzone);
	m_controllerDirectMode = g_gameConfig.GetBool(GameConfigKeys::Controller_DirectMode);
	m_backComboHold = g_gameConfig.GetEnum<Enum_ButtonComboModeSettings>(GameConfigKeys::UseBackCombo) == ButtonComboModeSettings::Hold;
	m_backComboInstant = g_gameConfig.GetEnum<Enum_ButtonComboModeSettings>(GameConfigKeys::UseBackCombo) == ButtonComboModeSettings::Instant;

	switch (g_gameConfig.GetEnum<Enum_LaserAxisOption>(GameConfigKeys::InvertLaserInput)) {
	
	case LaserAxisOption::Left:
		m_laserDirections[0] = -1.0f;
		m_laserDirections[1] = 1.0f;
		break;
	case LaserAxisOption::Right:
		m_laserDirections[0] = 1.0f;
		m_laserDirections[1] = -1.0f;
		break;
	case LaserAxisOption::Both:
		m_laserDirections[0] = -1.0f;
		m_laserDirections[1] = -1.0f;
		break;
	default:
		m_laserDirections[0] = 1.0f;
		m_laserDirections[1] = 1.0f;
		break;
	}


	// Init controller mapping
	if(m_laserDevice == InputDevice::Controller || m_buttonDevice == InputDevice::Controller)
	{
		const auto deviceId = g_gameConfig.GetBlob<16>(GameConfigKeys::Controller_DeviceID);
		int deviceIndex = -1;

		for (int32 i = 0; i < m_window->GetNumGamepads(); i++)
		{
			auto id = SDL_JoystickGetDeviceGUID(i);
			if (memcmp(deviceId.data(), id.data, 16) == 0)
			{
				deviceIndex = i; //always set device index since it is a match, but check input count in case of multiple matches
				auto joystick = SDL_JoystickOpen(i);

				int numInputs = 0;
				numInputs += SDL_JoystickNumAxes(joystick);
				numInputs += SDL_JoystickNumButtons(joystick);
				numInputs += SDL_JoystickNumHats(joystick);

				SDL_JoystickClose(joystick);
				if (numInputs > 0)
					break;
			}
		}



		if(deviceIndex < 0)
		{
			Log("Could not find bound gamepad", Logger::Severity::Warning);
		}
		else
		{
			m_gamepad = m_window->OpenGamepad(deviceIndex);
			if(m_gamepad)
			{
				m_gamepad->OnButtonPressed.Add(this, &Input::m_OnGamepadButtonPressed);
				m_gamepad->OnButtonReleased.Add(this, &Input::m_OnGamepadButtonReleased);
			}
		}
		m_InitControllerMapping();
	}

	// Init keyboard mapping
	m_InitKeyboardMapping();
}
void Input::Cleanup()
{
	if(m_gamepad)
	{
		m_gamepad->OnButtonPressed.RemoveAll(this);
		m_gamepad->OnButtonReleased.RemoveAll(this);
		m_gamepad.reset();
	}
	if(m_window)
	{
		m_window->OnKeyPressed.RemoveAll(this);
		m_window->OnKeyReleased.RemoveAll(this);
		m_window->OnMouseMotion.RemoveAll(this);
		m_window->OnTouchDown.RemoveAll(this);
		m_window->OnTouchMove.RemoveAll(this);
		m_window->OnTouchUp.RemoveAll(this);
		m_window->OnFocusChanged.RemoveAll(this);
		m_window = nullptr;
	}
}

void Input::Update(float deltaTime)
{
	for(auto it = m_mouseLocks.begin(); it != m_mouseLocks.end();)
	{
		if(it->use_count() == 1)
		{
			it = m_mouseLocks.erase(it);
			continue;
		}
		it++;
	}

#ifdef ANDROID
	// Touch-driven: always keep absolute mouse mode so emulated touch clicks
	// land at the correct position (laser input comes from touch zones instead)
	if (m_window->GetRelativeMouseMode())
		m_window->SetRelativeMouseMode(false);
#else
	if(!m_mouseLocks.empty())
	{
		if (!m_window->GetRelativeMouseMode())
			m_window->SetRelativeMouseMode(true);
	}
	else if (m_window->GetRelativeMouseMode())
	{
		m_window->SetRelativeMouseMode(false);
	}
#endif

	// While real touches are active, ignore the mouse (touch->mouse emulation would
	// otherwise feed phantom laser movement when tapping buttons)
	if(m_laserDevice == InputDevice::Mouse && m_activeTouchCount == 0)
	{
		for(uint32 i = 0; i < 2; i++)
		{
			if(m_mouseAxisMapping[i] > 1)
			{
				// INVALID MAPPING
				m_laserStates[i] = 0.0f;
				m_rawKeyLaserStates[i] = 0.0f;
				continue;
			}
			
			m_laserStates[i] = m_mouseSensitivity * m_mousePos[m_mouseAxisMapping[i]];
			m_rawLaserStates[i] = m_mousePos[m_mouseAxisMapping[i]];
			m_mousePos[m_mouseAxisMapping[i]] = 0;
		}
	}

	if(m_laserDevice == InputDevice::Keyboard)
	{
		for(uint32 i = 0; i < 2; i++)
		{
			m_laserStates[i] = m_rawKeyLaserStates[i] * deltaTime;

			// if neither laser button is being held fade out the laser input
			if (!m_buttonStates[(int32)Button::LS_0Neg + i * 2 + 1] && !m_buttonStates[(int32)Button::LS_0Neg + i * 2])
			{
				if (m_keyLaserReleaseTime != 0.f)
				{
					float reduction = m_keySensitivity * deltaTime / m_keyLaserReleaseTime;
					if (reduction > fabs(m_rawKeyLaserStates[i]))
						m_rawKeyLaserStates[i] = 0.f;
					else
						m_rawKeyLaserStates[i] -= reduction * Math::Sign(m_rawKeyLaserStates[i]);
				}
				else
				{
					m_rawKeyLaserStates[i] = 0.f;
				}
			}
		}
	}


	if(m_gamepad)
	{
		// Poll controller laser input
		if(m_laserDevice == InputDevice::Controller)
		{
			for(uint32 i = 0; i < 2; i++)
			{
				float axisState = m_gamepad->GetAxis(m_controllerAxisMapping[i]);
				float delta = axisState;
				if (!m_controllerDirectMode)
					delta -= m_prevLaserStates[i];
				if (fabs(delta) > 1.5f)
					delta += 2 * (Math::Sign(delta) * -1);
				if (fabs(delta) < m_controllerDeadzone)
				{
					m_laserStates[i] = 0.0f;
					m_rawLaserStates[i] = 0.0f;
				}
				else
				{
					m_rawLaserStates[i] = delta;
					m_laserStates[i] = delta * m_controllerSensitivity;
				}
				m_prevLaserStates[i] = axisState;
			}
		}
	}

	// Touch laser input overrides other devices while a finger is held on a laser
	// zone. Swipe-follow logic (SDVX Ultimate Mobile style): the finger's x maps
	// absolutely to the laser position, the laser chases it at a capped speed.
	for (uint32 i = 0; i < 2; i++)
	{
		if (m_touchLaserActive[i])
		{
			float d = Math::Clamp((m_touchLaserTarget[i] - m_touchLaserEst[i]) * 10.0f, -0.3f, 0.3f);
			m_laserStates[i] = d;
			m_touchLaserEst[i] += d;
		}
	}

	for (size_t i = 0; i < 2; i++)
	{
		m_laserStates[i] *= m_laserDirections[i];
		m_absoluteLaserStates[i] = fmodf(m_absoluteLaserStates[i] + m_laserStates[i], Math::pi * 2);
	}


	//back combo checks
	if (m_backComboHold && m_buttonStates[(size_t)Button::BT_S] && Are3BTsHeld())
	{
		m_comboHoldTimer += deltaTime;
		if (m_comboHoldTimer >= 0.5 && !m_backSent)
		{
			OnButtonPressed.Call(Button::Back, 0);
			m_backSent = true;
		}
	}
	else
	{
		m_comboHoldTimer = 0.0f;
		m_backSent = false;
	}
}

bool Input::GetButton(Button button) const
{
	return m_buttonStates[(size_t)button];
}

float Input::GetAbsoluteLaser(int laser) const
{
	return m_absoluteLaserStates[laser];
}

bool Input::Are3BTsHeld() const
{
	bool bta = GetButton(Input::Button::BT_0);
	bool btb = GetButton(Input::Button::BT_1);
	bool btc = GetButton(Input::Button::BT_2);
	bool btd = GetButton(Input::Button::BT_3);

	return (bta && btb && btc) || (bta && btb && btd) || (bta && btc && btd) || (btb && btc && btd);
}

uint32 Input::GetButtonBits() const
{
	uint32 res = 0;
	for (size_t i = 0; i < 7; i++)
	{
		res = res | ((m_buttonStates[i] ? 1 : 0) << i);
	}
	return res;
}

String Input::GetControllerStateString() const
{
	if(m_gamepad)
	{
		String s = "Buttons\n";
		for(uint32 i = 0; i < m_gamepad->NumButtons(); i++)
		{
			s += Utility::Sprintf("  [%d]%d\n", i, m_gamepad->GetButton(i));
		}
		s += "\nAxes\n";
		for(uint32 i = 0; i < m_gamepad->NumAxes(); i++)
		{
			s += Utility::Sprintf("  [%d]%.2f\n", i, m_gamepad->GetAxis(i));
		}
		for (uint32 i = 0; i < 2; i++)
		{
			s += Utility::Sprintf("Delta for knob %d: %.2f\n", i, m_laserStates[i]);
		}
		return s;
	}
	return String();
}

Ref<int32> Input::LockMouse()
{
	return m_mouseLocks.Add(MouseLockHandle(new int32(m_mouseLockIndex++)));
}

float Input::GetInputLaserDir(uint32 laserIdx)
{
	return m_laserStates[laserIdx];
}
float Input::GetAbsoluteInputLaserDir(uint32 laserIdx)
{
	return m_rawLaserStates[laserIdx];
}

//To give the sensitivity slider a good feel and put reasonable sensitivities in a reasonable range.
const double Input::mouseSensVars[] = { 20.0, 0.1, 1.2 };

double Input::CalculateSensFromPpr(double ppr)
{
	double pprSign = Math::Sign(ppr);

	return pprSign * (mouseSensVars[0] / mouseSensVars[1]) / pow(fabs(ppr), 1.0 / mouseSensVars[2]);
}
double Input::CalculateRealMouseSens(double sensSetting)
{
	double sensSign = Math::Sign(sensSetting);
	double ppr = EstimatePprFromSens(abs(sensSetting));
	return sensSign * 6.0 / (ppr);
}
double Input::EstimatePprFromSens(double sens)
{
	return pow(mouseSensVars[0] / (fmax(sens, 0.001) * mouseSensVars[1]), mouseSensVars[2]);
}
void Input::m_InitKeyboardMapping()
{
	memset(m_buttonStates, 0, sizeof(m_buttonStates));
	m_buttonMap.clear();

	if(m_buttonDevice == InputDevice::Keyboard)
	{
		// Button mappings
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BTS), Button::BT_S);

		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BT0), Button::BT_0);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BT1), Button::BT_1);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BT2), Button::BT_2);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BT3), Button::BT_3);

		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_FX0), Button::FX_0);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_FX1), Button::FX_1);
		
		// Alternate button mappings
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BTSAlt), Button::BT_S);

		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BT0Alt), Button::BT_0);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BT1Alt), Button::BT_1);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BT2Alt), Button::BT_2);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BT3Alt), Button::BT_3);

		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_FX0Alt), Button::FX_0);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_FX1Alt), Button::FX_1);
	}

	//Always bind back button
	m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Back), Button::Back);
	m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_BackAlt), Button::Back);

	if(m_laserDevice == InputDevice::Keyboard)
	{
		// Laser button mappings
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Laser0Neg), Button::LS_0Neg);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Laser0Pos), Button::LS_0Pos);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Laser1Neg), Button::LS_1Neg);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Laser1Pos), Button::LS_1Pos);

		// Alternate laser button mappings
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Laser0NegAlt), Button::LS_0Neg);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Laser0PosAlt), Button::LS_0Pos);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Laser1NegAlt), Button::LS_1Neg);
		m_buttonMap.Add(g_gameConfig.GetInt(GameConfigKeys::Key_Laser1PosAlt), Button::LS_1Pos);
	}
}

void Input::m_InitControllerMapping()
{
	m_controllerMap.clear();
	if(m_buttonDevice == InputDevice::Controller)
	{
		m_controllerMap.Add(g_gameConfig.GetInt(GameConfigKeys::Controller_BTS), Button::BT_S);
		m_controllerMap.Add(g_gameConfig.GetInt(GameConfigKeys::Controller_BT0), Button::BT_0);
		m_controllerMap.Add(g_gameConfig.GetInt(GameConfigKeys::Controller_BT1), Button::BT_1);
		m_controllerMap.Add(g_gameConfig.GetInt(GameConfigKeys::Controller_BT2), Button::BT_2);
		m_controllerMap.Add(g_gameConfig.GetInt(GameConfigKeys::Controller_BT3), Button::BT_3);
		m_controllerMap.Add(g_gameConfig.GetInt(GameConfigKeys::Controller_FX0), Button::FX_0);
		m_controllerMap.Add(g_gameConfig.GetInt(GameConfigKeys::Controller_FX1), Button::FX_1);
		m_controllerMap.Add(g_gameConfig.GetInt(GameConfigKeys::Controller_Back), Button::Back);
	}
}

void Input::m_OnButtonInput(Button b, bool pressed, int32 delta)
{
	bool& state = m_buttonStates[(size_t)b];
	if(state != pressed)
	{
		state = pressed;
		if(state)
		{
			if (b == Button::BT_S && m_backComboInstant && Are3BTsHeld())
			{
				OnButtonPressed.Call(Button::Back, delta);
			}
			else if (b == Button::BT_S && m_backComboHold && Are3BTsHeld());
			else
			{
				OnButtonPressed.Call(b, delta);
			}
		}
		else
		{
			OnButtonReleased.Call(b, delta);
		}
	}

	static Timer t;
	if(b >= Button::LS_0Neg && b <= Button::LS_1Pos)
	{
		int32 btnIdx = (int32)b - (int32)Button::LS_0Neg;
		int32 laserIdx = btnIdx / 2;
		// Set laser state based uppon the button that was pressed last
		if(pressed)
			m_rawKeyLaserStates[laserIdx] = (btnIdx % 2) == 0 ? -m_keySensitivity : m_keySensitivity;
		else // If a button was released check if the other one is still held
		{
			if(m_buttonStates[(int32)Button::LS_0Neg + laserIdx * 2])
				m_rawKeyLaserStates[laserIdx] = -m_keySensitivity;
			else if(m_buttonStates[(int32)Button::LS_0Neg + laserIdx * 2 + 1])
				m_rawKeyLaserStates[laserIdx] = m_keySensitivity;

		}
	}
}

void Input::m_OnGamepadButtonPressed(uint8 button, int32 delta)
{
	// Handle button mappings
	auto it = m_controllerMap.equal_range(button);
	for(auto it1 = it.first; it1 != it.second; it1++)
		m_OnButtonInput(it1->second, true, delta);
}
void Input::m_OnGamepadButtonReleased(uint8 button, int32 delta)
{
	// Handle button mappings
	auto it = m_controllerMap.equal_range(button);
	for(auto it1 = it.first; it1 != it.second; it1++)
		m_OnButtonInput(it1->second, false, delta);
}

void Input::OnKeyPressed(SDL_Scancode code, int32 delta)
{
	// Handle button mappings
	auto it = m_buttonMap.equal_range(static_cast<int32>(code));
	for(auto it1 = it.first; it1 != it.second; it1++)
		m_OnButtonInput(it1->second, true, delta);
}
void Input::OnKeyReleased(SDL_Scancode code, int32 delta)
{
	// Handle button mappings
	auto it = m_buttonMap.equal_range(static_cast<int32>(code));
	for(auto it1 = it.first; it1 != it.second; it1++)
		m_OnButtonInput(it1->second, false, delta);
}

void Input::OnMouseMotion(int32 x, int32 y)
{
	m_mousePos[0] += x;
	m_mousePos[1] += y;
}

int32 Input::m_FindTouchSlot(int64 fingerId) const
{
	for (int32 i = 0; i < MAX_TOUCHES; i++)
	{
		if (m_touchActive[i] && m_touchFingerIds[i] == fingerId)
			return i;
	}
	return -1;
}

void Input::m_ReleaseAllTouches()
{
	for (int32 i = 0; i < MAX_TOUCHES; i++)
	{
		if (!m_touchActive[i])
			continue;
		if (m_touchButton[i] >= 0)
			m_OnButtonInput((Button)m_touchButton[i], false, 0);
		if (m_touchButtonAlt[i] >= 0)
			m_OnButtonInput((Button)m_touchButtonAlt[i], false, 0);
		m_touchActive[i] = false;
		m_touchButton[i] = -1;
		m_touchButtonAlt[i] = -1;
		m_touchZone[i] = -1;
		m_touchLaser[i] = -1;
	}
	m_touchLaserActive[0] = m_touchLaserActive[1] = false;
	m_activeTouchCount = 0;
}

void Input::OnFocusChanged(bool focused)
{
	m_focused = focused;
	// On focus loss SDL may never deliver FINGERUP for held fingers; without this
	// the stale slots would swallow the next touches (multi-touch "breaking")
	if (!focused)
	{
		Logf("Touch: focus lost, releasing %d held touch(es)", Logger::Severity::Info, m_activeTouchCount);
		m_ReleaseAllTouches();
	}
}

float Input::CritLineY(int32 wndW, int32 wndH)
{
	// Portrait: crit line at ~0.75 of height, buttons below it. Landscape: ~0.95.
	return (wndH > wndW) ? 0.76f : 0.86f;
}

int32 Input::ZoneAt(float x, float y, float critY)
{
	// Top corners: Start (right) / Back (left)
	if (y < 0.08f)
	{
		if (x > 0.8f)
			return TZ_Start;
		if (x < 0.2f)
			return TZ_Back;
	}
	// Button console below the crit line
	if (y > critY)
	{
		// StandardSDVX: 4 BT lanes on top, FX-L/FX-R strips below (FX-L spans
		// under A+B, FX-R under C+D)
		if (g_gameConfig.GetEnum<Enum_TouchControllerLayouts>(GameConfigKeys::TouchControllerLayout) == TouchControllerLayouts::StandardSDVX)
		{
			const float bandH = 1.0f - critY;
			if (y > critY + bandH * 0.7f)
				return (x < 0.5f) ? TZ_FX0 : TZ_FX1;
			float t = Math::Clamp((x - 0.1f) / 0.8f, 0.0f, 0.999999f);
			return (int32)(t * 4.0f); // TZ_Lane0..TZ_Lane3
		}
		// UltimateMobile: 4 big lanes across the middle 80%
		float t = Math::Clamp((x - 0.1f) / 0.8f, 0.0f, 0.999999f);
		return (int32)(t * 4.0f); // TZ_Lane0..TZ_Lane3
	}
	// Laser zone: left half controls laser 0, right half controls laser 1
	return (x < 0.5f) ? TZ_Laser0 : TZ_Laser1;
}

Input::Button Input::ZoneButton(int32 zoneId)
{
	// UltimateMobile screen order: [BT-A+FX-L], [BT-B+FX-L], [BT-C+FX-R], [BT-D+FX-R]
	static const Button lanes[4] = { Button::BT_0, Button::BT_1, Button::BT_2, Button::BT_3 };
	if (zoneId >= TZ_Lane0 && zoneId <= TZ_Lane3)
		return lanes[zoneId];
	if (zoneId == TZ_FX0)
		return Button::FX_0;
	if (zoneId == TZ_FX1)
		return Button::FX_1;
	if (zoneId == TZ_Start)
		return Button::BT_S;
	return Button::Back;
}
Input::Button Input::ZoneButtonAlt(int32 zoneId)
{
	// Merged FX button only exists in the UltimateMobile layout: A/B double as
	// FX-L, C/D as FX-R; StandardSDVX lanes are pure BT with dedicated FX zones
	if (zoneId >= TZ_Lane0 && zoneId <= TZ_Lane3 &&
		g_gameConfig.GetEnum<Enum_TouchControllerLayouts>(GameConfigKeys::TouchControllerLayout) == TouchControllerLayouts::UltimateMobile)
	{
		if (zoneId == TZ_Lane0 || zoneId == TZ_Lane1)
			return Button::FX_0;
		return Button::FX_1;
	}
	return Button::Length;
}

void Input::ZoneRect(int32 zoneId, float critY, float& x, float& y, float& w, float& h)
{
	switch (zoneId)
	{
		case TZ_Start: x = 0.8f; y = 0.0f; w = 0.2f; h = 0.08f; break;
		case TZ_Back:  x = 0.0f; y = 0.0f; w = 0.2f; h = 0.08f; break;
		case TZ_Laser0: x = 0.0f; y = 0.08f; w = 0.5f; h = critY - 0.08f; break;
		case TZ_Laser1: x = 0.5f; y = 0.08f; w = 0.5f; h = critY - 0.08f; break;
		default: // button console (full console height)
			if (g_gameConfig.GetEnum<Enum_TouchControllerLayouts>(GameConfigKeys::TouchControllerLayout) == TouchControllerLayouts::StandardSDVX)
			{
				// 4 BT lanes on top of the console band, FX strips below:
				// FX-L spans under A+B, FX-R under C+D
				const float bandH = 1.0f - critY;
				if (zoneId == TZ_FX0)
				{
					x = 0.1f; y = critY + bandH * 0.7f; w = 0.4f; h = bandH * 0.3f;
				}
				else if (zoneId == TZ_FX1)
				{
					x = 0.5f; y = critY + bandH * 0.7f; w = 0.4f; h = bandH * 0.3f;
				}
				else
				{
					// lanes share the UltimateMobile x positions
					x = 0.1f + zoneId * (0.8f / 4.0f); w = 0.8f / 4.0f;
					y = critY; h = bandH * 0.7f;
				}
			}
			else
			{
				// 4 lanes across the middle 80%
				x = 0.1f + zoneId * (0.8f / 4.0f); w = 0.8f / 4.0f;
				y = critY; h = 1.0f - critY;
			}
			break;
	}
}

void Input::SetTouchZonesEnabled(bool enabled)
{
	if (m_touchZonesEnabled == enabled)
		return;
	m_touchZonesEnabled = enabled;
	if (!enabled)
		m_ReleaseAllTouches(); // release everything any held finger was pressing
}

void Input::SetTouchButtonFireFilter(std::function<bool(Button)> filter)
{
	m_touchFireFilter = filter;
}

int32 Input::GetTouchZoneInfos(TouchZoneInfo* out, int32 maxZones) const
{
	if (!m_window)
		return 0;
	Vector2i wndSize = m_window->GetWindowSize();
	float critY = CritLineY(wndSize.x, wndSize.y);
	int32 n = 0;
	const bool standardLayout = g_gameConfig.GetEnum<Enum_TouchControllerLayouts>(GameConfigKeys::TouchControllerLayout) == TouchControllerLayouts::StandardSDVX;
	for (int32 z = 0; z < TZ_Count && n < maxZones; z++)
	{
		// Start/Back corners are always shown; the rest only in gameplay screens
		if (!m_touchZonesEnabled && z != TZ_Start && z != TZ_Back)
			continue;
		// Dedicated FX zones only exist in the StandardSDVX layout
		if ((z == TZ_FX0 || z == TZ_FX1) && !standardLayout)
			continue;
		out[n].zoneId = z;
		ZoneRect(z, critY, out[n].x, out[n].y, out[n].w, out[n].h);
		n++;
	}
	return n;
}

int32 Input::GetActiveTouchPoints(float* outXY, int32 maxPoints) const
{
	int32 n = 0;
	for (int32 i = 0; i < MAX_TOUCHES && n < maxPoints; i++)
	{
		if (m_touchActive[i])
		{
			outXY[n * 2] = m_touchLastX[i];
			outXY[n * 2 + 1] = m_touchLastY[i];
			n++;
		}
	}
	return n;
}

bool Input::IsTouchZoneActive(int32 zoneId) const
{
	if (zoneId == TZ_Laser0)
		return m_touchLaserActive[0];
	if (zoneId == TZ_Laser1)
		return m_touchLaserActive[1];
	// Match by zone, not by button: lanes A/B share FX-L (and C/D FX-R), so
	// button matching would light the neighbouring lane up too
	for (int32 i = 0; i < MAX_TOUCHES; i++)
	{
		if (m_touchActive[i] && m_touchZone[i] == zoneId)
			return true;
	}
	return false;
}

// Assigns a touch slot to the zone at (x, y) and presses the zone's button(s).
// Shared by OnTouchDown and the ghost-release recovery path in OnTouchMove.
void Input::m_AssignZone(int32 slot, float x, float y)
{
	Vector2i wndSize = m_window->GetWindowSize();
	float critY = CritLineY(wndSize.x, wndSize.y);
	int32 zone = ZoneAt(x, y, critY);
	m_touchZone[slot] = zone; // for zone-accurate overlay highlighting

	// Start/Back corners work everywhere (menus included), so touch-only users
	// can always confirm/back out
	if (zone == TZ_Start || zone == TZ_Back)
	{
		Button b = ZoneButton(zone);
		m_touchButton[slot] = (int32)b;
		m_OnButtonInput(b, true, 0);
#ifdef ANDROID
		TouchButtonHaptic();
#endif
		return;
	}

	if (!m_touchZonesEnabled)
		return; // menus: touch is handled as mouse clicks by SDL emulation

	if (zone == TZ_Laser0 || zone == TZ_Laser1)
	{
		// Laser zone. Absolute (swipe-follow): the finger's x position maps
		// absolutely to the laser position (half the screen = full laser
		// range). Relative (trackpad): touch-down keeps the laser where it is
		// and OnTouchMove accumulates finger deltas into the target.
		int32 laser = zone - TZ_Laser0;
		m_touchLaser[slot] = laser;
		m_touchLaserActive[laser] = true;
		if (g_gameConfig.GetEnum<Enum_TouchLaserModes>(GameConfigKeys::TouchLaserMode) != TouchLaserModes::Relative)
		{
			m_touchLaserTarget[laser] = (x < 0.5f) ? Math::Clamp(x * 2.0f, 0.0f, 1.0f)
			                                       : Math::Clamp((x - 0.5f) * 2.0f, 0.0f, 1.0f);
			m_touchLaserEst[laser] = m_touchLaserTarget[laser];
		}
	}
	else
	{
		// Button lane; every lane merges its FX button (A/B -> FX-L, C/D -> FX-R).
		// Smart routing: when gameplay scoring installed the fire filter, each
		// merged button is only fired if it would be judged right now (hit
		// window / hold pickup / idle press). A button whose next note is still
		// more than 'good' early is suppressed instead: the engine judges such
		// a press as an instant miss, which would make a BT tap steal and kill
		// the side's upcoming FX note (and vice versa). The suppressed button
		// stays free, so a later lane tap can still judge its own note fresh.
		Button b = ZoneButton(zone);
		Button bAlt = ZoneButtonAlt(zone);
		if (m_touchFireFilter)
		{
			if (bAlt != Button::Length && !m_touchFireFilter(bAlt))
				bAlt = Button::Length;
			if (b != Button::Length && !m_touchFireFilter(b))
				b = Button::Length;
		}
		if (b != Button::Length)
		{
			m_touchButton[slot] = (int32)b;
			m_OnButtonInput(b, true, 0);
		}
		if (bAlt != Button::Length)
		{
			m_touchButtonAlt[slot] = (int32)bAlt;
			m_OnButtonInput(bAlt, true, 0);
		}
#ifdef ANDROID
		// One pulse per tap even when BT and FX fired together; suppressed
		// (smart-routed) buttons stay silent so a blocked tap feels blocked
		if (b != Button::Length || bAlt != Button::Length)
			TouchButtonHaptic();
#endif
	}
}

void Input::OnTouchDown(int64 fingerId, float x, float y)
{
	// Always track the finger (native multi-touch), even in menus where it only
	// acts as a mouse click via SDL's touch->mouse emulation
	int32 slot = m_FindTouchSlot(fingerId);
	if (slot >= 0)
	{
		// already tracked (duplicate DOWN): just refresh the position
		m_touchLastX[slot] = x;
		m_touchLastY[slot] = y;
		return;
	}
	{
		for (int32 i = 0; i < MAX_TOUCHES; i++)
		{
			if (!m_touchActive[i])
			{
				slot = i;
				break;
			}
		}
		if (slot < 0)
			return;
		m_touchActive[slot] = true;
		m_touchFingerIds[slot] = fingerId;
		m_activeTouchCount++;
	}

	m_touchLastX[slot] = x;
	m_touchLastY[slot] = y;
	m_touchButton[slot] = -1;
	m_touchButtonAlt[slot] = -1;
	m_touchZone[slot] = -1;
	m_touchLaser[slot] = -1;

	m_AssignZone(slot, x, y);
}

void Input::OnTouchMove(int64 fingerId, float x, float y)
{
	int32 slot = m_FindTouchSlot(fingerId);
	if (slot < 0)
	{
		// Ghost-release recovery ("dropped touch"): when several fingers land at
		// once, some digitizers/systems emit a spurious ACTION_POINTER_UP (or
		// cancel a subset of the gesture) for a finger that is still physically
		// on the screen. OnTouchUp then freed its slot and released its button
		// while Android keeps sending MOVE for that fingerId. Since Android
		// never sends MOVE after a real lift, a MOVE for an unknown id proves the
		// finger is still down: re-allocate the slot and re-press its zone so
		// the hold recovers instead of staying dead until the player re-taps.
		if (!m_touchZonesEnabled || !m_focused)
			return;
		for (int32 i = 0; i < MAX_TOUCHES; i++)
		{
			if (!m_touchActive[i])
			{
				slot = i;
				break;
			}
		}
		if (slot < 0)
			return;
		m_touchActive[slot] = true;
		m_touchFingerIds[slot] = fingerId;
		m_activeTouchCount++;
		m_touchButton[slot] = -1;
		m_touchButtonAlt[slot] = -1;
		m_touchZone[slot] = -1;
		m_touchLaser[slot] = -1;
		m_touchLastX[slot] = x;
		m_touchLastY[slot] = y;
		m_AssignZone(slot, x, y);
		Logf("Touch: re-acquired phantom-released finger %lld at (%.2f, %.2f)",
			Logger::Severity::Info, (long long)fingerId, x, y);
		return;
	}

	if (m_touchZonesEnabled && m_touchLaser[slot] >= 0)
	{
		int32 laser = m_touchLaser[slot];
		if (g_gameConfig.GetEnum<Enum_TouchLaserModes>(GameConfigKeys::TouchLaserMode) == TouchLaserModes::Relative)
		{
			// Trackpad: accumulate the finger's x delta into the laser target.
			// Scale 2.0 matches the absolute mapping, so the same physical
			// swipe distance covers the same laser distance.
			m_touchLaserTarget[laser] = Math::Clamp(
				m_touchLaserTarget[laser] + (x - m_touchLastX[slot]) * 2.0f, 0.0f, 1.0f);
		}
		else
		{
			// Swipe-follow: finger x maps absolutely to the laser target position
			m_touchLaserTarget[laser] = (x < 0.5f) ? Math::Clamp(x * 2.0f, 0.0f, 1.0f)
			                                       : Math::Clamp((x - 0.5f) * 2.0f, 0.0f, 1.0f);
		}
	}
	m_touchLastX[slot] = x;
	m_touchLastY[slot] = y;
}

void Input::OnTouchUp(int64 fingerId, float x, float y)
{
	int32 slot = m_FindTouchSlot(fingerId);
	if (slot < 0)
	{
		// Normally means a previous FINGERDOWN was lost (event flood) or focus
		// loss already released everything; log in gameplay so it is traceable
		if (m_touchZonesEnabled)
			Logf("Touch: FINGERUP for untracked finger %lld", Logger::Severity::Info, (long long)fingerId);
		return;
	}

	// Only release a button if no other finger is still holding the same one
	auto stillHeld = [&](int32 btn) {
		for (int32 i = 0; i < MAX_TOUCHES; i++)
		{
			if (i != slot && m_touchActive[i] && (m_touchButton[i] == btn || m_touchButtonAlt[i] == btn))
				return true;
		}
		return false;
	};
	if (m_touchButton[slot] >= 0 && !stillHeld(m_touchButton[slot]))
		m_OnButtonInput((Button)m_touchButton[slot], false, 0);
	if (m_touchButtonAlt[slot] >= 0 && !stillHeld(m_touchButtonAlt[slot]))
		m_OnButtonInput((Button)m_touchButtonAlt[slot], false, 0);
	if (m_touchLaser[slot] >= 0)
	{
		int32 laser = m_touchLaser[slot];
		// Only deactivate the laser if no other finger is still on it
		bool stillUsed = false;
		for (int32 i = 0; i < MAX_TOUCHES; i++)
		{
			if (i != slot && m_touchActive[i] && m_touchLaser[i] == laser)
			{
				stillUsed = true;
				break;
			}
		}
		if (!stillUsed)
			m_touchLaserActive[laser] = false;
	}

	m_touchActive[slot] = false;
	m_touchButton[slot] = -1;
	m_touchButtonAlt[slot] = -1;
	m_touchZone[slot] = -1;
	m_touchLaser[slot] = -1;
	m_activeTouchCount--;
}
