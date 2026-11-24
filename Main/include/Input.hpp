#pragma once

#include <functional>

// Types of input device
DefineEnum(InputDevice,
	Keyboard,
	Mouse,
	Controller)

typedef Ref<int32> MouseLockHandle;

/*
	Class that handles game keyboard (and soon controller input)
*/
class Input : Unique
{
public:
	DefineEnum(Button,
		BT_0,
		BT_1,
		BT_2,
		BT_3,
		FX_0,
		FX_1,
        BT_S, //Start Button
		LS_0Neg, // Left laser- 
		LS_0Pos, // Left laser+		(|---->)
		LS_1Neg, // Right laser-	(<----|)
		LS_1Pos, // Right laser+
		Back,
		Length)

	~Input();
	void Init(Graphics::Window& wnd);
	void Cleanup();

	// Poll/Update input
	void Update(float deltaTime);

	[[nodiscard]]
	bool GetButton(Button button) const;
	[[nodiscard]]
	float GetAbsoluteLaser(int laser) const;
	[[nodiscard]]
	bool Are3BTsHeld() const;
	[[nodiscard]]
	uint32 GetButtonBits() const;

	// Controller state as a string
	// Primarily used for debugging
	[[nodiscard]]
	String GetControllerStateString() const;

	// Returns a handle to a mouse lock, release it to unlock the mouse
	[[nodiscard]]
	MouseLockHandle LockMouse();

	// Event handlers
	virtual void OnKeyPressed(SDL_Scancode code, int32 delta);
	virtual void OnKeyReleased(SDL_Scancode code, int32 delta);
	virtual void OnMouseMotion(int32 x, int32 y);
	// Touch handlers, x/y are normalized screen coordinates (0..1)
	virtual void OnTouchDown(int64 fingerId, float x, float y);
	virtual void OnTouchMove(int64 fingerId, float x, float y);
	virtual void OnTouchUp(int64 fingerId, float x, float y);
	virtual void OnFocusChanged(bool focused);

	// Touch zone layout (shared between hit-testing and the on-screen overlay)
	enum TouchZone : int32
	{
		// Button console below the crit line.
		// UltimateMobile: 4 big lanes; each lane merges its side's FX button
		// (A/B -> FX-L, C/D -> FX-R, smart routed by the scoring filter).
		// StandardSDVX: 4 pure BT lanes with dedicated FX strips below them
		// (TZ_FX0 spans under A+B -> FX-L, TZ_FX1 under C+D -> FX-R).
		TZ_Lane0 = 0, TZ_Lane1, TZ_Lane2, TZ_Lane3,
		TZ_FX0, TZ_FX1,                                     // FX columns (StandardSDVX layout only)
		TZ_Laser0, TZ_Laser1,                             // left/right half of the play area
		TZ_Start, TZ_Back,                                // top-right / top-left corners
		TZ_Count
	};
	struct TouchZoneInfo { float x, y, w, h; int32 zoneId; }; // normalized screen coords
	// One hittable button/hold tick inside the judgment window, as collected by
	// the gameplay scoring system for the touch intent solver (candidate pool).
	struct TouchWindowCandidate
	{
		int32 button = -1;     // Input::Button index (0..5)
		int32 time = 0;        // tick time, ms (MapTime)
		int32 delta = 0;       // current - tick time, negative = tick still ahead
		bool holdStart = false; // hold start tick (pickup candidate)
		bool activeHold = false; // a hold on this button is currently active
		bool pressReady = false; // a press edge right now would be judged (hold pickup or tap in perfect window)
	};
	static const int32 MAX_TOUCHES = 10;
	// Enable/disable touch-zone mapping (gameplay screens). When disabled, touch
	// acts as plain mouse clicks (handled by SDL touch->mouse emulation).
	void SetTouchZonesEnabled(bool enabled);
	// Smart routing for merged touch lane presses: the gameplay scoring system
	// installs a filter that decides whether firing a button right now would be
	// judged (hit window / hold pickup) or would harm an upcoming note (a press
	// more than 'good' early is judged as an instant miss by the engine, so a
	// merged press must not steal it). A merged lane press only fires the
	// buttons the filter accepts. Null = fire everything (legacy behavior).
	void SetTouchButtonFireFilter(std::function<bool(Button)> filter);
	// Pair-derived FX (A+B or C+D held together): per-frame hint from the
	// gameplay scoring system about its FX queues. 0 = release (re-arm so the
	// next tick gets a fresh press edge), 1 = press now (an FX tick needs the
	// edge), 2 = keep (hold sustain in progress). Null = disabled.
	void SetTouchFXEdgeHint(std::function<int32(Button)> hint);
	// Touch intent solver plumbing: the gameplay scoring system provides all
	// unjudged button ticks whose |delta| is inside the judgment window
	// (±good). The solver consumes this candidate pool instead of the
	// per-button filter/hint callbacks. Returns the number of candidates
	// written. Null = no pool available.
	using TouchCandidateProvider = std::function<uint32(TouchWindowCandidate* out, uint32 max)>;
	void SetTouchCandidateProvider(TouchCandidateProvider provider);
	// Called right before a synthetic touch press edge (intent solver cycles /
	// pair-derived FX cycles). The scoring side uses it to bypass its bounce
	// guard, which would otherwise swallow a release+press cycle entirely.
	void SetTouchSyntheticEdgeNotify(std::function<void(Button)> notify);
	// Same for touch releases: touch has no mechanical chatter, and a release
	// swallowed by the bounce guard leaves hold objects (and the perceived
	// button state) stuck.
	void SetTouchSyntheticReleaseNotify(std::function<void(Button)> notify);
	// Candidates collected for the current frame (intent solver working set)
	[[nodiscard]]
	int32 GetTouchCandidateCount() const { return m_touchCandidateCount; }
	[[nodiscard]]
	const TouchWindowCandidate* GetTouchCandidates() const { return m_touchCandidates; }
	[[nodiscard]]
	bool TouchZonesEnabled() const { return m_touchZonesEnabled; }
	[[nodiscard]]
	int32 GetTouchZoneInfos(TouchZoneInfo* out, int32 maxZones) const;
	// Currently held fingers as normalized x,y pairs (native multi-touch feedback)
	[[nodiscard]]
	int32 GetActiveTouchPoints(float* outXY, int32 maxPoints) const;
	[[nodiscard]]
	bool IsTouchZoneActive(int32 zoneId) const;
	static int32 ZoneAt(float x, float y, float critY);
	static float CritLineY(int32 wndW, int32 wndH);
	static void ZoneRect(int32 zoneId, float critY, float& x, float& y, float& w, float& h);
	static Button ZoneButton(int32 zoneId);   // primary button of a zone
	static Button ZoneButtonAlt(int32 zoneId); // merged secondary button (FX on outer lanes), Length if none

	// Request laser input state
	[[nodiscard]]
	float GetInputLaserDir(uint32 laserIdx);

	// Request laser input state without sensitivity applied
	[[nodiscard]]
	float GetAbsoluteInputLaserDir(uint32 laserIdx);
	[[nodiscard]]
	static double CalculateRealMouseSens(double sensSetting);
	[[nodiscard]]
	static double EstimatePprFromSens(double sens);
	[[nodiscard]]
	static double CalculateSensFromPpr(double ppr);

	// Button delegates
	Delegate<Button, int32> OnButtonPressed;
	Delegate<Button, int32> OnButtonReleased;

private:
	void m_InitKeyboardMapping();
	void m_InitControllerMapping();
	void m_OnButtonInput(Button b, bool pressed, int32 delta);

	void m_OnGamepadButtonPressed(uint8 button, int32 delta);
	void m_OnGamepadButtonReleased(uint8 button, int32 delta);

	static const double mouseSensVars[3];

	int32 m_mouseLockIndex = 0;
	Vector<MouseLockHandle> m_mouseLocks;

	InputDevice m_laserDevice;
	InputDevice m_buttonDevice;

	bool m_buttonStates[(size_t)Button::Length];
	bool m_backComboHold = false;
	bool m_backComboInstant = false;
	bool m_backSent = false;
	float m_laserStates[2] = { 0.0f };
	float m_rawLaserStates[2] = { 0.0f };
	float m_rawKeyLaserStates[2] = { 0.0f };
	float m_prevLaserStates[2] = { 0.0f };
	float m_absoluteLaserStates[2] = { 0.0f };
	float m_comboHoldTimer = 0.0f;
	float m_laserDirections[2] = { 1.0f };

	// Keyboard bindings
	Multimap<int32, Button> m_buttonMap;
	float m_keySensitivity;
	float m_keyLaserReleaseTime;

	// Mouse bindings
	uint32 m_mouseAxisMapping[2] = { 0,1 };
	float m_mouseSensitivity;
	int32 m_lastMousePos[2];
	int32 m_mousePos[2];

	// Controller bindings
	Multimap<uint32, Button> m_controllerMap;
	uint32 m_controllerAxisMapping[2] = { 0,1 };
	float m_controllerSensitivity;
	float m_controllerDeadzone;
	bool m_controllerDirectMode;

	Ref<Gamepad> m_gamepad;

	Graphics::Window* m_window = nullptr;

	// Touch state
	int64 m_touchFingerIds[MAX_TOUCHES];
	bool m_touchActive[MAX_TOUCHES];
	int32 m_touchButton[MAX_TOUCHES];    // Button index or -1
	int32 m_touchButtonAlt[MAX_TOUCHES]; // merged second button of the zone or -1
	int32 m_touchZone[MAX_TOUCHES];      // TZ_* zone id of the finger or -1 (zone-accurate overlay highlight)
	int32 m_touchLaser[MAX_TOUCHES];     // -1 or laser index 0/1
	float m_touchLastX[MAX_TOUCHES];
	float m_touchLastY[MAX_TOUCHES];
	bool m_touchLaserActive[2] = { false, false };
	// Laser swipe-follow state: finger position maps absolutely to laser position
	float m_touchLaserTarget[2] = { 0.0f, 0.0f };
	float m_touchLaserEst[2] = { 0.0f, 0.0f };
	int32 m_activeTouchCount = 0;
	bool m_touchZonesEnabled = false;
	bool m_focused = true;
	int32 m_FindTouchSlot(int64 fingerId) const;
	void m_AssignZone(int32 slot, float x, float y);
	void m_TouchSynthPress(Button b); // synthetic touch edge (bypasses scoring bounce guard)
	void m_TouchRelease(Button b);    // touch release (bypasses scoring bounce guard)
	bool m_SolverPressChannel(int32 button); // intent solver: press / cycle one channel
	bool m_SolverFingerDown(int32 zone, bool& firedPrimary, bool& firedAlt); // intent solver: decide lane-press edges
	void m_ReleaseAllTouches();
	std::function<bool(Button)> m_touchFireFilter; // smart routing, see SetTouchButtonFireFilter
	std::function<int32(Button)> m_touchFXEdgeHint; // pair-derived FX, see SetTouchFXEdgeHint
	bool m_synthFXHeld[2] = { false, false }; // our synthetic FX press is held (per side)
	TouchCandidateProvider m_touchCandidateProvider; // intent solver, see SetTouchCandidateProvider
	std::function<void(Button)> m_touchSynthEdgeNotify; // see SetTouchSyntheticEdgeNotify
	std::function<void(Button)> m_touchReleaseNotify;   // see SetTouchSyntheticReleaseNotify
	static const int32 MAX_TOUCH_CANDIDATES = 32;
	TouchWindowCandidate m_touchCandidates[MAX_TOUCH_CANDIDATES];
	int32 m_touchCandidateCount = 0;
	bool m_solverHeld[6] = { false }; // intent solver synthetic press per channel
};
