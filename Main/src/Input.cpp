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

// Cost of judging a candidate tick with a press on its channel right now
// (single weight source of the touch intent solver; defined after Update).
static float TouchCandidateCost(const Input::TouchWindowCandidate& c);

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

	// Intent solver candidate pool: all unjudged button ticks inside the
	// judgment window (±good), collected once per frame while touch zones are
	// active. P0: plumbing only, the solver consumer arrives in P1.
	m_touchCandidateCount = 0;
	if (m_touchCandidateProvider && m_touchZonesEnabled)
		m_touchCandidateCount = (int32)m_touchCandidateProvider(m_touchCandidates, MAX_TOUCH_CANDIDATES);

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

	// Touch intent solver (see GameConfigKeys::TouchSolver). Layers:
	//  - solver only: every lane press is routed through the candidate pool +
	//    cost function, which guesses whether the press means FX-L/R or
	//    BT-A..D (chart-aware disambiguation, no auto input)
	//  - + TouchAutoAssist: adds the semi-auto channel layer (state hold
	//    pickup, FX auto-taps) and replaces the pair-derived FX machinery
	const bool solverEnabled = g_gameConfig.GetBool(GameConfigKeys::TouchSolver)
		&& g_gameConfig.GetBool(GameConfigKeys::TouchAutoAssist)
		&& m_touchCandidateProvider && m_touchZonesEnabled && m_focused;
	// The legacy pair-derived FX machine only serves the non-solver mode
	const bool pairMachineEnabled = !g_gameConfig.GetBool(GameConfigKeys::TouchSolver)
		&& m_touchFXEdgeHint && m_touchZonesEnabled;

	// Pair-derived FX buttons: while two fingers hold both lanes of a side
	// (A+B or C+D), the pair acts as the side's FX button being held down.
	// The scoring system hints each frame whether its FX queue needs a press
	// edge (1), is clear to re-arm (0) or the hold sustain should be kept (2).
	if (!solverEnabled && pairMachineEnabled)
	// This makes e.g. an FX hold that starts *after* the BT holds, or repeated
	// FX holds during one continuous pair hold, work without lifting or
	// adding a third finger.
	{
		static const int32 pairLanes[2][2] =
		{
			{ TZ_Lane0, TZ_Lane1 }, // FX-L
			{ TZ_Lane2, TZ_Lane3 }, // FX-R
		};
		const Button fxButtons[2] = { Button::FX_0, Button::FX_1 };
		for (uint32 side = 0; side < 2; side++)
		{
			const Button fxButton = fxButtons[side];
			const int32 fx = (int32)fxButton;

			// Is the side's lane pair held by touch fingers?
			bool pairHeld = false;
			if (m_touchZonesEnabled)
			{
				bool lanes[2] = { false, false };
				for (uint32 i = 0; i < MAX_TOUCHES && !(lanes[0] && lanes[1]); i++)
				{
					if (!m_touchActive[i])
						continue;
					if (m_touchZone[i] == pairLanes[side][0])
						lanes[0] = true;
					else if (m_touchZone[i] == pairLanes[side][1])
						lanes[1] = true;
				}
				pairHeld = lanes[0] && lanes[1];
			}

			// Is a finger actually holding this FX button (merged press)?
			auto realHolder = [&]()
			{
				for (uint32 i = 0; i < MAX_TOUCHES; i++)
				{
					if (m_touchActive[i] && (m_touchButton[i] == fx || m_touchButtonAlt[i] == fx))
						return true;
				}
				return false;
			};

			if (!pairHeld)
			{
				// Pair broken: release our synthetic press, unless a finger
				// owns the button via a merged lane press
				if (m_synthFXHeld[side])
				{
					m_synthFXHeld[side] = false;
					if (!realHolder())
						m_TouchRelease(fxButton);
				}
				continue;
			}

			if (!m_touchFXEdgeHint || !m_focused)
				continue;
			const int32 hint = m_touchFXEdgeHint(fxButton);
			if (hint == 1)
			{
				// An FX tick needs a press edge right now. Hold starts (and
				// tap notes) are only judged on a rising edge, so a button
				// that was already held idle (e.g. the fingers pressed the
				// lanes long before the FX hold started) would miss it.
				const bool touchOwned = m_synthFXHeld[side] || realHolder();
				if (!m_buttonStates[(size_t)fxButton])
				{
					m_TouchSynthPress(fxButton);
					m_synthFXHeld[side] = true;
				}
				else if (touchOwned)
				{
					// Generate a fresh edge without lifting the fingers
					m_TouchRelease(fxButton);
					m_TouchSynthPress(fxButton);
					m_synthFXHeld[side] = true;
				}
				// else: another input device owns the button, leave it alone
			}
			else if (hint == 0 && m_synthFXHeld[side])
			{
				// Re-arm so a following tick can get a fresh press edge
				m_synthFXHeld[side] = false;
				if (!realHolder())
					m_TouchRelease(fxButton);
			}
			// hint 2: hold sustain in progress, keep the current state
		}
	}

	if (solverEnabled)
	{
		// Channel target states from the held zones
		bool lanesHeld[4] = { false, false, false, false };
		bool fxZoneHeld[2] = { false, false };
		for (uint32 i = 0; i < MAX_TOUCHES; i++)
		{
			if (!m_touchActive[i])
				continue;
			const int32 z = m_touchZone[i];
			if (z >= TZ_Lane0 && z <= TZ_Lane3)
				lanesHeld[z - TZ_Lane0] = true;
			else if (z == TZ_FX0)
				fxZoneHeld[0] = true;
			else if (z == TZ_FX1)
				fxZoneHeld[1] = true;
		}
		const bool mergedLayout =
			g_gameConfig.GetEnum<Enum_TouchControllerLayouts>(GameConfigKeys::TouchControllerLayout) == TouchControllerLayouts::UltimateMobile;
		bool chTarget[6] = { false, false, false, false, false, false };
		for (int32 b = 0; b < 4; b++)
			chTarget[b] = lanesHeld[b];
		if (mergedLayout)
		{
			chTarget[(int32)Button::FX_0] = lanesHeld[0] && lanesHeld[1];
			chTarget[(int32)Button::FX_1] = lanesHeld[2] && lanesHeld[3];
		}
		else
		{
			chTarget[(int32)Button::FX_0] = fxZoneHeld[0];
			chTarget[(int32)Button::FX_1] = fxZoneHeld[1];
		}

		for (int32 b = 0; b < 6; b++)
		{
			const Button button = (Button)b;
			// Tap auto-cycling is an FX-only assist: a resting finger must not
			// auto-play BT taps (four resting fingers would otherwise play the
			// whole chart). BT taps need a fresh finger-down edge.
			const bool tapCycleAllowed = (b == (int32)Button::FX_0 || b == (int32)Button::FX_1);
			bool activeHold = false; // front has a hold in progress
			bool wantsEdge = false;  // a press edge would be judged now
			for (int32 i = 0; i < m_touchCandidateCount; i++)
			{
				const TouchWindowCandidate& c = m_touchCandidates[i];
				if (c.button != b)
					continue;
				activeHold = activeHold || c.activeHold;
				if (!c.holdStart && !tapCycleAllowed)
					continue;
				wantsEdge = wantsEdge || (TouchCandidateCost(c) < 1.0f);
			}

			// Finger merged presses claim the button in m_touchButton(Alt)
			bool fingerClaims = false;
			for (uint32 i = 0; i < MAX_TOUCHES && !fingerClaims; i++)
			{
				if (m_touchActive[i] && (m_touchButton[i] == b || m_touchButtonAlt[i] == b))
					fingerClaims = true;
			}

			if (!chTarget[b])
			{
				// Fingers left the channel: drop the synthetic press. This must
				// also happen mid-hold - lifting the finger ends the hold, like
				// a real button (release is handled before the sustain case on
				// purpose; the old order locked holds until their end).
				if (m_solverHeld[b])
				{
					m_solverHeld[b] = false;
					if (!fingerClaims && m_buttonStates[(size_t)button])
						m_TouchRelease(button);
				}
				continue;
			}
			if (activeHold)
				continue; // sustain with fingers still on the channel: keep
			if (wantsEdge)
			{
				// Channel is covered and a tick wants the edge: press / cycle
				(void)m_SolverPressChannel(b);
			}
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
			m_TouchRelease((Button)m_touchButton[i]);
		if (m_touchButtonAlt[i] >= 0)
			m_TouchRelease((Button)m_touchButtonAlt[i]);
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

void Input::SetTouchFXEdgeHint(std::function<int32(Button)> hint)
{
	m_touchFXEdgeHint = hint;
	m_synthFXHeld[0] = m_synthFXHeld[1] = false;
}

void Input::SetTouchCandidateProvider(TouchCandidateProvider provider)
{
	// Drop any synthetic presses the intent solver still holds
	for (int32 b = 0; b < 6; b++)
	{
		if (!m_solverHeld[b])
			continue;
		m_solverHeld[b] = false;
		if (m_buttonStates[(size_t)b])
			m_TouchRelease((Button)b);
	}
	m_touchCandidateProvider = provider;
	m_touchCandidateCount = 0;
}

void Input::SetTouchSyntheticEdgeNotify(std::function<void(Button)> notify)
{
	m_touchSynthEdgeNotify = notify;
}

void Input::SetTouchSyntheticReleaseNotify(std::function<void(Button)> notify)
{
	m_touchReleaseNotify = notify;
}

// Presses a button from the touch layer as a synthetic edge. Must notify the
// scoring side first so its bounce guard is bypassed: a cycle is a
// release+press within the guard window by construction and would otherwise
// be swallowed (the release itself re-arms the guard).
void Input::m_TouchSynthPress(Button b)
{
	if (m_touchSynthEdgeNotify)
		m_touchSynthEdgeNotify(b);
	m_OnButtonInput(b, true, 0);
}

// Releases a button from the touch layer. Touch releases bypass the scoring
// bounce guard: they never chatter, and a swallowed release leaves hold
// objects (and the observable button state) stuck.
void Input::m_TouchRelease(Button b)
{
	if (m_buttonStates[(size_t)b] && m_touchReleaseNotify)
		m_touchReleaseNotify(b);
	m_OnButtonInput(b, false, 0);
	m_solverHeld[(size_t)b] = false;
}

// Cost of judging a candidate tick with a press on its channel right now.
// Lower is better; >= 1.0 means the solver must not press for it yet.
//   - running holds must not be disturbed by a press (dominant cost)
//   - not pressable yet (outside the useful window) costs exactly 1.0
//   - otherwise the timing distance (|delta| / good) scales the cost
// This is the single weight source for both the finger-down edges and the
// per-frame channel machinery; extend the terms here (lane affinity, pair
// bonuses, ...) as the solver grows.
static float TouchCandidateCost(const Input::TouchWindowCandidate& c)
{
	if (c.activeHold)
		return 1000.0f;
	if (!c.pressReady)
		return 1.0f;
	const float goodMs = 150.0f; // nominal good window, only scales the tie-break
	return Math::Min(fabsf((float)c.delta) / goodMs, 0.99f);
}

// Presses or cycles (release+press) one touch channel. Cycling gives the
// scoring side the rising edge it needs when the channel is already held.
bool Input::m_SolverPressChannel(int32 b)
{
	const Button button = (Button)b;
	bool fingerClaims = false;
	for (uint32 i = 0; i < MAX_TOUCHES && !fingerClaims; i++)
	{
		if (m_touchActive[i] && (m_touchButton[i] == b || m_touchButtonAlt[i] == b))
			fingerClaims = true;
	}
	if (!m_buttonStates[(size_t)button])
	{
		// State pickup: the channel is covered, judge the note / pick it up
		m_TouchSynthPress(button);
		m_solverHeld[b] = true;
		return true;
	}
	if (m_solverHeld[b] || fingerClaims)
	{
		// Held idle without a usable edge: cycle to generate one
		m_TouchRelease(button);
		m_TouchSynthPress(button);
		m_solverHeld[b] = true;
		return true;
	}
	// Another input device owns the button: leave it alone
	return false;
}

// Decides which buttons a fresh lane touch fires, from the candidate pool and
// the cost function instead of blind merged presses. The closest candidate
// wins; a second channel only joins in when it is nearly as close (a real
// chord), so a distant note on the other channel is not judged early. Hold
// starts are always taken when they are inside their pickup window (pickup
// has no rating penalty). Returns true when at least one channel was pressed.
bool Input::m_SolverFingerDown(int32 zone, bool& firedPrimary, bool& firedAlt)
{
	firedPrimary = false;
	firedAlt = false;
	TouchWindowCandidate pool[MAX_TOUCH_CANDIDATES];
	const int32 count = m_touchCandidateProvider
		? (int32)m_touchCandidateProvider(pool, MAX_TOUCH_CANDIDATES) : 0;

	const Button primary = ZoneButton(zone);
	const Button alt = ZoneButtonAlt(zone);
	const int32 primaryButton = (int32)primary;
	const int32 altButton = (alt == Button::Length) ? -1 : (int32)alt;

	bool primaryHoldActive = false;
	bool altHoldActive = false;
	float altCost = 2.0f;
	for (int32 i = 0; i < count; i++)
	{
		const TouchWindowCandidate& c = pool[i];
		if (c.button == primaryButton)
			primaryHoldActive = primaryHoldActive || c.activeHold;
		else if (c.button == altButton)
		{
			altHoldActive = altHoldActive || c.activeHold;
			altCost = Math::Min(altCost, TouchCandidateCost(c));
		}
	}

	// Assist, not full control: the press itself ALWAYS reaches the pad. The
	// primary BT of the zone is fired (or cycled when already held) no matter
	// what the chart says; out-of-window presses are ignored by the engine,
	// so this can never kill an approaching note. An active hold on that
	// button is the only case we do not cycle over (it would break the hold).
	if (primary != Button::Length && !primaryHoldActive)
		firedPrimary = m_SolverPressChannel(primaryButton);

	// The merged FX button is where the guessing happens: fire it only when an
	// FX tick would actually be judged by this press.
	if (altButton >= 0 && !altHoldActive && altCost < 1.0f)
		firedAlt = m_SolverPressChannel(altButton);

	return firedPrimary || firedAlt;
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
		// The press itself always reaches the pad (the engine ignores presses
		// outside the judgment window, so nothing gets stolen); the intent
		// solver additionally guesses whether the merged FX button should fire.
		// A finger only claims buttons it actually pressed, so releasing the
		// pressing finger always releases the button.
		Button b = ZoneButton(zone);
		Button bAlt = ZoneButtonAlt(zone);
		bool fireB = false, fireAlt = false;
		const bool solverEdge = g_gameConfig.GetBool(GameConfigKeys::TouchSolver) && m_touchCandidateProvider;
		if (solverEdge)
		{
			// Edges go through the intent solver: candidate pool + cost function
			// decide which channels this touch presses (or cycles).
			m_SolverFingerDown(zone, fireB, fireAlt);
		}
		else
		{
			// Plain merged press (pre-solver behavior). No chart filtering: the
			// engine ignores out-of-window presses now, so a merged press can
			// never steal an approaching note. Presses bypass the bounce guard
			// like every other touch edge.
			fireB = (b != Button::Length);
			fireAlt = (bAlt != Button::Length);
			if (fireB)
				m_TouchSynthPress(b);
			if (fireAlt)
				m_TouchSynthPress(bAlt);
		}
		// Assist, not full control: a finger only claims what it actually
		// pressed. A resting finger never keeps an untouched button held, so
		// releasing the pressing finger always releases the button.
		if (b != Button::Length && fireB)
			m_touchButton[slot] = (int32)b;
		if (bAlt != Button::Length && fireAlt)
			m_touchButtonAlt[slot] = (int32)bAlt;
#ifdef ANDROID
		// One pulse per tap even when BT and FX fired together; suppressed
		// buttons stay silent so a blocked tap feels blocked
		if (fireB || fireAlt)
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
		m_TouchRelease((Button)m_touchButton[slot]);
	if (m_touchButtonAlt[slot] >= 0 && !stillHeld(m_touchButtonAlt[slot]))
		m_TouchRelease((Button)m_touchButtonAlt[slot]);
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
