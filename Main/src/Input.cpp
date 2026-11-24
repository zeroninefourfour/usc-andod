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
//
// The class/method lookup is cached: this runs on every lane press, i.e. up to
// several times per frame in a four finger chord, and GetStaticMethodID is a
// fair amount of work to repeat there. The cache is a global ref to the
// activity class, which lives as long as the process.
static void TouchButtonHaptic()
{
	if (!g_gameConfig.GetBool(GameConfigKeys::TouchButtonVibration))
		return;

	// Coalesce a chord into one pulse: four fingers landing in the same frame
	// are one physical hit as far as the player is concerned, and each
	// Vibrator.vibrate() is a binder round trip on the thread that also has to
	// deliver the remaining touch events.
	static uint32 lastPulseTick = 0;
	static bool pulsed = false;
	const uint32 now = SDL_GetTicks();
	if (pulsed && now - lastPulseTick < 25)
		return;

	JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
	if (!env)
		return;

	static jclass hapticClass = nullptr;
	static jmethodID hapticMethod = nullptr;
	if (!hapticMethod)
	{
		jobject activity = (jobject)SDL_AndroidGetActivity();
		if (!activity)
			return;
		jclass localClass = env->GetObjectClass(activity);
		if (localClass)
		{
			hapticClass = (jclass)env->NewGlobalRef(localClass);
			env->DeleteLocalRef(localClass);
		}
		env->DeleteLocalRef(activity);
		if (!hapticClass)
			return;
		hapticMethod = env->GetStaticMethodID(hapticClass, "hapticVibrate", "(I)V");
		if (!hapticMethod)
			return;
	}

	env->CallStaticVoidMethod(hapticClass, hapticMethod, (jint)15);
	lastPulseTick = now;
	pulsed = true;
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

	memset(m_touchDeviceIds, 0, sizeof(m_touchDeviceIds));
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
	m_activeTouchCount = 0;

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
	if (m_laserDevice == InputDevice::Mouse)
	{
		if (m_activeTouchCount > 0)
		{
			// SDL synthesizes mouse motion from the tracked touch while a finger
			// is down. Drop it instead of letting it pile up: otherwise the
			// whole accumulated movement is applied as one laser delta on the
			// frame the last finger lifts, i.e. the laser jumps after every hold.
			m_mousePos[0] = 0;
			m_mousePos[1] = 0;
		}
		else
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
	//
	// The chase is a discrete-time feedback loop: with per-frame gain K the
	// remaining error is multiplied by (1 - K) every frame, which only converges
	// for 0 < K < 2. The previous version used a fixed gain of 10 with a +/-0.3
	// clamp, i.e. a bang-bang limit cycle: after any movement bigger than ~3% of
	// the lane it emitted +0.3 / -0.3 on alternating frames forever, even with
	// the finger perfectly still. That also made the scoring side see a "wrong
	// direction" input every other frame (draining the laser assist) and lose
	// every other laser tick. Scale the gain by deltaTime and cap the speed
	// instead, so the behaviour is identical at 30, 60, 120 or 144 Hz.
	const float laserFollowRate = 14.0f; // 1/s, ~90% of the distance in 165 ms
	const float laserMaxSpeed = 3.5f;    // lanes/s, about the speed of good key play
	for (uint32 i = 0; i < 2; i++)
	{
		if (!m_touchLaserActive[i])
			continue;

		// Closed loop on the real pointer position when the scoring side can
		// report it: the estimate is then always "where the in-game laser
		// actually is", so the chase cannot drift away from it. This matters
		// because the auto-laser assist snaps the pointer to the chart at any
		// time, so an open-loop integral (what the old code kept) silently
		// diverged from the visible laser position.
		float pointer = 0.0f;
		if (m_touchLaserPositionProvider && m_touchLaserPositionProvider(i, pointer))
			m_touchLaserEst[i] = Math::Clamp(pointer, 0.0f, 1.0f);

		const float factor = Math::Min(1.0f, laserFollowRate * deltaTime);
		const float maxStep = laserMaxSpeed * deltaTime;
		const float d = Math::Clamp(
			(m_touchLaserTarget[i] - m_touchLaserEst[i]) * factor, -maxStep, maxStep);
		m_touchLaserEst[i] = Math::Clamp(m_touchLaserEst[i] + d, 0.0f, 1.0f);
		m_laserStates[i] = d;
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
					m_TouchSynthPress(fxButton, 0);
					m_synthFXHeld[side] = true;
				}
				else if (touchOwned)
				{
					// Generate a fresh edge without lifting the fingers
					m_TouchRelease(fxButton);
					m_TouchSynthPress(fxButton, 0);
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
				(void)m_SolverPressChannel(b, 0);
			}
		}
	}

	for (size_t i = 0; i < 2; i++)
	{
		// The touch layer folds the inversion into its target mapping (see
		// m_TouchLaserTargetFromX). Scaling the delta again here would make the
		// finger chase a mirrored target with the wrong sign, so the laser would
		// walk away from the finger and pin at one end.
		if (!m_touchLaserActive[i])
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

int32 Input::m_FindTouchSlot(int64 touchId, int64 fingerId) const
{
	for (int32 i = 0; i < MAX_TOUCHES; i++)
	{
		// Finger ids are only unique per touch device: a stylus and a finger
		// can both report finger id 0 at the same time. Matching on the finger
		// id alone made the second device a "duplicate" of the first one (its
		// press was swallowed and its release freed the other device's button).
		if (m_touchActive[i] && m_touchFingerIds[i] == fingerId && m_touchDeviceIds[i] == touchId)
			return i;
	}
	return -1;
}

// A finger that is tracked but currently owns no button and no laser. That is
// either a finger that was suspended (focus loss / zones disabled) or one that
// landed on a screen where touch zones are mapped but not active (menus). Both
// have to be re-assigned when the zones become active again, because no new
// DOWN will ever arrive for them.
bool Input::m_TouchHoldsNothing(int32 slot) const
{
	return m_touchButton[slot] < 0 && m_touchButtonAlt[slot] < 0 && m_touchLaser[slot] < 0;
}

// Releases everything the fingers were pressing but keeps tracking them. Used
// when the input layer has to drop the button state for a reason that is not
// "the finger is gone" (focus loss, touch zones temporarily disabled): the
// fingers are usually still on the glass, and Android/SDL will not send a new
// DOWN for them, so throwing the tracking away leaves the pad dead until the
// player lifts and taps again.
void Input::m_ReleaseTouchButtons()
{
	for (int32 i = 0; i < MAX_TOUCHES; i++)
	{
		if (!m_touchActive[i])
			continue;
		if (m_touchButton[i] >= 0)
			m_TouchRelease((Button)m_touchButton[i]);
		if (m_touchButtonAlt[i] >= 0)
			m_TouchRelease((Button)m_touchButtonAlt[i]);
		// m_touchActive / m_touchFingerIds / m_touchLastX/Y are kept on purpose:
		// the slot now "holds nothing", which is what m_ReassignTouchZones()
		// looks for when the zones or the focus come back.
		m_touchButton[i] = -1;
		m_touchButtonAlt[i] = -1;
		m_touchZone[i] = -1;
		m_touchLaser[i] = -1;
	}
	m_touchLaserActive[0] = m_touchLaserActive[1] = false;
}

// Re-presses the zones of fingers that are still down but currently hold
// nothing (after focus loss or a temporary touch-zone disable).
void Input::m_ReassignTouchZones()
{
	if (!m_touchZonesEnabled || !m_window || !m_focused)
		return;
	Vector2i wndSize = m_window->GetWindowSize();
	const float critY = CritLineY(wndSize.x, wndSize.y);
	int32 reassigned = 0;
	for (int32 i = 0; i < MAX_TOUCHES; i++)
	{
		if (!m_touchActive[i] || !m_TouchHoldsNothing(i))
			continue;
		// Never re-press Start/Back: those are supposed to be deliberate taps,
		// and a palm that happens to rest in a top corner must not fire a
		// second Back edge every time the window regains focus.
		const int32 zone = ZoneAt(m_touchLastX[i], m_touchLastY[i], critY);
		if (zone == TZ_Start || zone == TZ_Back)
			continue;
		m_AssignZone(i, m_touchLastX[i], m_touchLastY[i], 0);
		reassigned++;
	}
	if (reassigned > 0)
		Logf("Touch: re-pressed %d finger(s) still on screen", Logger::Severity::Info, reassigned);
}

void Input::OnFocusChanged(bool focused)
{
	m_focused = focused;
	// On focus loss SDL may never deliver FINGERUP for held fingers, so the
	// presses have to be dropped. The fingers themselves are usually still on
	// the glass (notification shade, system dialog, a stray palm at the top
	// edge), and neither SDL nor Android will send a new DOWN for them, so keep
	// the tracking and re-press their zones as soon as we are focused again -
	// dropping the slots entirely made the whole pad dead until the player
	// lifted every finger and tapped again.
	if (!focused)
	{
		Logf("Touch: focus lost, releasing %d held touch(es)", Logger::Severity::Info, m_activeTouchCount);
		m_ReleaseTouchButtons();
		m_focusLostTick = SDL_GetTicks();
	}
	else if (SDL_GetTicks() - m_focusLostTick <= 1500)
	{
		// Only trust the kept slots for a short loss. Android normally sends a
		// CANCEL (which clears them through FINGERUP) but if it did not and the
		// player lifted the fingers in the meantime, re-pressing here would
		// leave a button held forever - worse than the dead pad we are fixing.
		m_ReassignTouchZones();
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
	{
		// A dialog or a screen without touch zones took over: release what the
		// fingers were pressing, but keep tracking them. Zones come back while
		// the player is still holding the pad (pause menu, transition into the
		// song), and there is no new DOWN coming for those fingers.
		m_ReleaseTouchButtons();
	}
	else
	{
		m_ReassignTouchZones();
	}
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

void Input::SetTouchLaserPositionProvider(TouchLaserPositionProvider provider)
{
	m_touchLaserPositionProvider = provider;
	// Nothing else to reset: the provider is only read while a laser finger is
	// held, and the next read overwrites the estimate with the real pointer.
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
void Input::m_TouchSynthPress(Button b, int32 delta)
{
	if (m_touchSynthEdgeNotify)
		m_touchSynthEdgeNotify(b);
	m_OnButtonInput(b, true, delta);
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
bool Input::m_SolverPressChannel(int32 b, int32 delta)
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
		m_TouchSynthPress(button, delta);
		m_solverHeld[b] = true;
		return true;
	}
	if (m_solverHeld[b] || fingerClaims)
	{
		// Held idle without a usable edge: cycle to generate one
		m_TouchRelease(button);
		m_TouchSynthPress(button, delta);
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
bool Input::m_SolverFingerDown(int32 zone, bool& firedPrimary, bool& firedAlt, int32 delta)
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
		firedPrimary = m_SolverPressChannel(primaryButton, delta);

	// The merged FX button is where the guessing happens: fire it only when an
	// FX tick would actually be judged by this press.
	if (altButton >= 0 && !altHoldActive && altCost < 1.0f)
		firedAlt = m_SolverPressChannel(altButton, delta);

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

// Maps a normalized screen x to the target of the laser the finger owns.
// The half of the play area is taken from the laser index, never from x: a
// left-laser finger that slides past the middle of the screen must keep
// addressing the left half. Mapping by x made the target jump from 1.0 to 0.0
// when the finger crossed the centre, i.e. the laser teleported to the other
// end while the player was still holding the same side.
// Laser axis inversion (InvertLaserInput) is folded in here, because the touch
// layer feeds the same signed delta integration the knob uses; applying the
// inversion again in Update would make the finger chase a mirrored target with
// the wrong sign.
float Input::m_TouchLaserTargetFromX(int32 laser, float x)
{
	const float halfX = (laser == 0) ? x : x - 0.5f;
	float t = Math::Clamp(halfX * 2.0f, 0.0f, 1.0f);
	if (m_laserDirections[laser] < 0.0f)
		t = 1.0f - t;
	return t;
}

// Assigns a touch slot to the zone at (x, y) and presses the zone's button(s).
// Shared by OnTouchDown and the ghost-release recovery path in OnTouchMove.
void Input::m_AssignZone(int32 slot, float x, float y, int32 delta)
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
		m_OnButtonInput(b, true, delta);
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
			m_touchLaserTarget[laser] = m_TouchLaserTargetFromX(laser, x);
			// Do not snap the estimate to the finger target: the estimate is
			// where the in-game laser pointer currently is, and the chase has to
			// pull the pointer to the finger. Snapping it made the emitted delta
			// exactly 0 on touch-down, so "absolute" mode never moved the laser
			// to the finger and only reacted to movement afterwards.
			float currentPointer = 0.0f;
			if (m_touchLaserPositionProvider && m_touchLaserPositionProvider((uint32)laser, currentPointer))
				m_touchLaserEst[laser] = Math::Clamp(currentPointer, 0.0f, 1.0f);
		}
	}
	else
	{
		// Button lane; every lane merges its FX button (A/B -> FX-L, C/D -> FX-R).
		// The lane's own BT always reaches the pad (that is the button the
		// player touched); the merged FX button is a guess and is filtered. A
		// finger only claims buttons it actually pressed, so releasing the
		// pressing finger always releases the button.
		Button b = ZoneButton(zone);
		Button bAlt = ZoneButtonAlt(zone);
		bool fireB = false, fireAlt = false;
		const bool solverEdge = g_gameConfig.GetBool(GameConfigKeys::TouchSolver) && m_touchCandidateProvider;
		if (solverEdge)
		{
			// Edges go through the intent solver: candidate pool + cost function
			// decide which channels this touch presses (or cycles).
			m_SolverFingerDown(zone, fireB, fireAlt, delta);
		}
		else
		{
			// Plain merged press (non-solver path). The engine no longer kills a
			// note on an out-of-window press, so this is not about stealing
			// notes any more; it is about not pressing FX at all when no FX
			// tick is anywhere near (which used to judge far-away FX notes and
			// kept FX held for the length of every BT hold). Presses bypass the
			// bounce guard like every other touch edge.
			fireB = (b != Button::Length);
			// The merged FX button is the guess, so only take it when the
			// scoring side says a press would be judged now (or would be
			// harmless). Firing it unconditionally meant every BT tap also
			// pressed FX: an FX note up to 'good' away got judged by a press
			// meant for another lane, and the FX button stayed held for the
			// whole length of every BT hold.
			fireAlt = (bAlt != Button::Length) &&
				(!m_touchFireFilter || m_touchFireFilter(bAlt));
			if (fireB)
				m_TouchSynthPress(b, delta);
			if (fireAlt)
				m_TouchSynthPress(bAlt, delta);
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

void Input::OnTouchDown(int64 touchId, int64 fingerId, float x, float y, int32 delta)
{
	// Always track the finger (native multi-touch), even in menus where it only
	// acts as a mouse click via SDL's touch->mouse emulation
	int32 slot = m_FindTouchSlot(touchId, fingerId);
	if (slot >= 0)
	{
		// Already tracked. A repeated DOWN for a finger we still track means our
		// table and the platform disagree (a lost UP, or a slot kept across a
		// focus loss). If that slot currently holds nothing it is a suspended
		// finger, so claim its zone for real instead of swallowing the press -
		// otherwise the player has to lift and tap again before anything
		// registers. If it does hold something this is a genuine duplicate.
		m_touchLastX[slot] = x;
		m_touchLastY[slot] = y;
		if (m_TouchHoldsNothing(slot))
			m_AssignZone(slot, x, y, delta);
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
		// Table full: evict a suspended finger (tracked, but holding nothing)
		// rather than dropping the new touch on the floor.
		if (slot < 0)
		{
			for (int32 i = 0; i < MAX_TOUCHES; i++)
			{
				if (m_touchActive[i] && m_TouchHoldsNothing(i))
				{
					slot = i;
					m_touchActive[i] = false;
					m_activeTouchCount--;
					break;
				}
			}
		}
		if (slot < 0)
		{
			Logf("Touch: no free slot for FINGERDOWN %lld", Logger::Severity::Warning, (long long)fingerId);
			return;
		}
		m_touchActive[slot] = true;
		m_touchFingerIds[slot] = fingerId;
		m_touchDeviceIds[slot] = touchId;
		m_activeTouchCount++;
	}

	m_touchLastX[slot] = x;
	m_touchLastY[slot] = y;
	m_touchButton[slot] = -1;
	m_touchButtonAlt[slot] = -1;
	m_touchZone[slot] = -1;
	m_touchLaser[slot] = -1;

	m_AssignZone(slot, x, y, delta);
}

void Input::OnTouchMove(int64 touchId, int64 fingerId, float x, float y, int32 delta)
{
	int32 slot = m_FindTouchSlot(touchId, fingerId);
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
		m_touchDeviceIds[slot] = touchId;
		m_activeTouchCount++;
		m_touchButton[slot] = -1;
		m_touchButtonAlt[slot] = -1;
		m_touchZone[slot] = -1;
		m_touchLaser[slot] = -1;
		m_touchLastX[slot] = x;
		m_touchLastY[slot] = y;
		// A re-acquire is triggered by a MOVE, not by the press the player
		// made, so there is no useful event age to give here.
		m_AssignZone(slot, x, y, 0);
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
			// Swipe-follow: finger x maps absolutely to the laser target
			// position inside the finger's own half of the play area.
			m_touchLaserTarget[laser] = m_TouchLaserTargetFromX(laser, x);
		}
	}
	m_touchLastX[slot] = x;
	m_touchLastY[slot] = y;
}

void Input::OnTouchUp(int64 touchId, int64 fingerId, float x, float y, int32 delta)
{
	int32 slot = m_FindTouchSlot(touchId, fingerId);
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
