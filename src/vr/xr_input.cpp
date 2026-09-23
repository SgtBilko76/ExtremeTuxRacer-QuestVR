/* --------------------------------------------------------------------
EXTREME TUXRACER -- OpenXR controller input, implementation

See xr_input.h for the button map. The results are pushed onto the
platform layer's virtual joystick, which turns them into the same
sf::Event stream the game already handles, so no game code changes.
---------------------------------------------------------------------*/

#include "xr_input.h"

#ifdef ENABLE_OPENXR

#include "vr.h"
#include "xr_manager.hpp"
#include "../platform/platform.h"

#include <android/log.h>

#include <cmath>
#include <ctime>
#include <cstring>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "ETRInput", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRInput", __VA_ARGS__)

namespace {

// Joystick button numbers CRacing::Jbutt already understands.
const unsigned int BUTTON_PADDLE = 0;
const unsigned int BUTTON_TRICK  = 1;
const unsigned int BUTTON_BRAKE  = 2;
const unsigned int BUTTON_CHARGE = 3;

// A trigger has to be pulled this far before it counts as pressed.
const float TRIGGER_THRESHOLD = 0.5f;

struct Input {
	XRManager* manager = nullptr;
	XrInstance instance = XR_NULL_HANDLE;
	XrSession  session  = XR_NULL_HANDLE;

	XrActionSet action_set = XR_NULL_HANDLE;

	XrAction steer  = XR_NULL_HANDLE;   // left thumbstick
	XrAction paddle = XR_NULL_HANDLE;   // right trigger
	XrAction brake  = XR_NULL_HANDLE;   // left trigger
	XrAction jump   = XR_NULL_HANDLE;   // A / X
	XrAction trick  = XR_NULL_HANDLE;   // grip
	XrAction pause  = XR_NULL_HANDLE;   // B
	XrAction menu   = XR_NULL_HANDLE;   // menu button

	bool ready = false;

	// Previous edge-triggered states, so a held button is not re-sent.
	bool was_pause = false;
	bool was_menu  = false;

	// Menu navigation
	bool ui_mode = false;
	bool was_confirm = false;
	bool was_cancel  = false;
	int  held_x = 0, held_y = 0;   // current stick direction, -1/0/+1
	double repeat_at = 0.0;        // when the held direction next repeats
};

/** Seconds on CLOCK_MONOTONIC, for menu key auto-repeat. */
double nowSeconds() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Feels close to a desktop keyboard: a pause before the first repeat, then
// a steady rate, so a long course list is still navigable.
const double kRepeatDelay    = 0.40;
const double kRepeatInterval = 0.15;

/** Quantises an axis to -1 / 0 / +1 with a generous dead zone, so a
 *  diagonal push does not fire both axes. */
int quantise(float v) {
	if (v >  0.6f) return  1;
	if (v < -0.6f) return -1;
	return 0;
}

Input g;

bool check(XrResult r, const char* what) {
	if (XR_SUCCEEDED(r)) return true;
	LOGE("%s failed (%d)", what, static_cast<int>(r));
	return false;
}

XrAction makeAction(XrActionType type, const char* name,
                    const char* localised) {
	XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
	ci.actionType = type;
	std::strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	std::strncpy(ci.localizedActionName, localised,
	             XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);

	XrAction action = XR_NULL_HANDLE;
	if (!check(xrCreateAction(g.action_set, &ci, &action), name))
		return XR_NULL_HANDLE;
	return action;
}

XrPath path(const char* s) {
	XrPath p = XR_NULL_PATH;
	xrStringToPath(g.instance, s, &p);
	return p;
}

/** Injects a key event so the game's existing keyboard handlers deal with
 *  it, rather than duplicating their logic here. */
void sendKey(sf::Keyboard::Key key, bool pressed) {
	sf::Event ev;
	ev.type = pressed ? sf::Event::KeyPressed : sf::Event::KeyReleased;
	ev.key.code = key;
	ev.key.alt = ev.key.control = ev.key.shift = ev.key.system = false;
	etr_platform::PushEvent(ev);
}

/** A complete press-and-release, for discrete menu actions. The GUI acts
 *  on the press, but states also watch for the release. */
void tapKey(sf::Keyboard::Key key) {
	sendKey(key, true);
	sendKey(key, false);
}

float readFloat(XrAction action) {
	XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = action;
	XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
	if (XR_FAILED(xrGetActionStateFloat(g.session, &gi, &st)) || !st.isActive)
		return 0.f;
	return st.currentState;
}

bool readBool(XrAction action) {
	XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = action;
	XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
	if (XR_FAILED(xrGetActionStateBoolean(g.session, &gi, &st)) || !st.isActive)
		return false;
	return st.currentState == XR_TRUE;
}

XrVector2f readVector2(XrAction action) {
	XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = action;
	XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
	if (XR_FAILED(xrGetActionStateVector2f(g.session, &gi, &st)) || !st.isActive)
		return XrVector2f{0.f, 0.f};
	return st.currentState;
}

}  // namespace

bool InitInput(XRManager* manager) {
	if (!manager) return false;

	g.manager  = manager;
	g.instance = manager->getInstance();
	g.session  = manager->getSession();

	XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strncpy(si.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	std::strncpy(si.localizedActionSetName, "Gameplay",
	             XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	si.priority = 0;
	if (!check(xrCreateActionSet(g.instance, &si, &g.action_set),
	           "xrCreateActionSet"))
		return false;

	g.steer  = makeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "steer", "Steer");
	g.paddle = makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "paddle", "Paddle");
	g.brake  = makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "brake", "Brake");
	g.jump   = makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "jump", "Jump");
	g.trick  = makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "trick", "Trick");
	g.pause  = makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "pause", "Pause");
	g.menu   = makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu");

	if (!g.steer || !g.paddle || !g.brake || !g.jump || !g.trick ||
	    !g.pause || !g.menu)
		return false;

	const XrActionSuggestedBinding bindings[] = {
		{ g.steer,  path("/user/hand/left/input/thumbstick") },
		{ g.paddle, path("/user/hand/right/input/trigger/value") },
		{ g.brake,  path("/user/hand/left/input/trigger/value") },
		{ g.jump,   path("/user/hand/right/input/a/click") },
		{ g.jump,   path("/user/hand/left/input/x/click") },
		{ g.trick,  path("/user/hand/right/input/squeeze/value") },
		{ g.trick,  path("/user/hand/left/input/squeeze/value") },
		{ g.pause,  path("/user/hand/right/input/b/click") },
		{ g.menu,   path("/user/hand/left/input/menu/click") },
	};

	// Meta Touch and every Pico controller share the same layout (A/B on
	// the right, X/Y and menu on the left), so one binding list serves all
	// of them. The Pico profiles exist only with XR_BD_controller_interaction,
	// and pico4s (Pico 4 Ultra) only from its spec version 2, so a runtime
	// rejecting one is expected and skipped rather than fatal.
	std::vector<const char*> profiles;
	profiles.push_back("/interaction_profiles/oculus/touch_controller");
	if (manager->hasBDControllerInteraction()) {
		profiles.push_back("/interaction_profiles/bytedance/pico4_controller");
		profiles.push_back("/interaction_profiles/bytedance/pico4s_controller");
		profiles.push_back("/interaction_profiles/bytedance/pico_neo3_controller");
	}

	int accepted = 0;
	for (const char* profile : profiles) {
		XrInteractionProfileSuggestedBinding suggested{
			XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		suggested.interactionProfile = path(profile);
		suggested.suggestedBindings = bindings;
		suggested.countSuggestedBindings =
		    sizeof(bindings) / sizeof(bindings[0]);

		if (check(xrSuggestInteractionProfileBindings(g.instance, &suggested),
		          profile))
			accepted++;
	}
	if (accepted == 0)
		return false;

	XrSessionActionSetsAttachInfo ai{
		XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	ai.countActionSets = 1;
	ai.actionSets = &g.action_set;
	if (!check(xrAttachSessionActionSets(g.session, &ai),
	           "xrAttachSessionActionSets"))
		return false;

	g.ready = true;
	LOGI("Controller bindings attached (%d profiles)", accepted);
	return true;
}

void ShutdownInput() {
	if (g.action_set != XR_NULL_HANDLE) xrDestroyActionSet(g.action_set);
	g = Input();
}

namespace vr {

void UpdateInput() {
	if (!g.ready) return;

	XrActiveActionSet active{};
	active.actionSet = g.action_set;
	active.subactionPath = XR_NULL_PATH;

	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;

	const XrResult r = xrSyncActions(g.session, &sync);
	// XR_SESSION_NOT_FOCUSED is normal whenever a system overlay is up.
	if (XR_FAILED(r) || r == XR_SESSION_LOSS_PENDING) return;

	const XrVector2f stick = readVector2(g.steer);

	// ---- menu navigation ---------------------------------------------
	if (g.ui_mode) {
		// The thumbstick becomes a d-pad driving the arrow keys the GUI
		// already navigates with, so no menu screen needs changing.
		const int dir_x = quantise(stick.x);
		const int dir_y = quantise(stick.y);
		const double now = nowSeconds();

		if (dir_x != g.held_x || dir_y != g.held_y) {
			g.held_x = dir_x;
			g.held_y = dir_y;
			g.repeat_at = now + kRepeatDelay;
			// Thumbstick +y is up; the GUI's "previous" key is Up.
			if (dir_y > 0)      tapKey(sf::Keyboard::Up);
			else if (dir_y < 0) tapKey(sf::Keyboard::Down);
			else if (dir_x < 0) tapKey(sf::Keyboard::Left);
			else if (dir_x > 0) tapKey(sf::Keyboard::Right);
		} else if ((dir_x || dir_y) && now >= g.repeat_at) {
			g.repeat_at = now + kRepeatInterval;
			if (dir_y > 0)      tapKey(sf::Keyboard::Up);
			else if (dir_y < 0) tapKey(sf::Keyboard::Down);
			else if (dir_x < 0) tapKey(sf::Keyboard::Left);
			else if (dir_x > 0) tapKey(sf::Keyboard::Right);
		}

		// A confirms, B goes back -- the usual Quest convention.
		const bool confirm = readBool(g.jump);
		if (confirm && !g.was_confirm) tapKey(sf::Keyboard::Return);
		g.was_confirm = confirm;

		const bool cancel = readBool(g.pause) || readBool(g.menu);
		if (cancel && !g.was_cancel) tapKey(sf::Keyboard::Escape);
		g.was_cancel = cancel;

		// Leave the gameplay axes centred so nothing is held down when the
		// race resumes.
		etr_platform::SetJoystickAxis(0, sf::Joystick::X, 0.f);
		etr_platform::SetJoystickAxis(0, sf::Joystick::Y, 0.f);
		return;
	}

	// ---- steering ----------------------------------------------------
	etr_platform::SetJoystickAxis(0, sf::Joystick::X, stick.x);

	// ---- paddle / brake ----------------------------------------------
	// The game reads a single axis where negative paddles and positive
	// brakes, so the two triggers are combined into one value.
	const float paddle = readFloat(g.paddle);
	const float brake  = readFloat(g.brake);
	etr_platform::SetJoystickAxis(0, sf::Joystick::Y, brake - paddle);

	// The digital buttons back up the analogue axis, so a fully pulled
	// trigger works even if the axis path is unbound on some controller.
	etr_platform::SetJoystickButton(0, BUTTON_PADDLE,
	                                paddle > TRIGGER_THRESHOLD);
	etr_platform::SetJoystickButton(0, BUTTON_BRAKE,
	                                brake > TRIGGER_THRESHOLD);

	// ---- buttons -----------------------------------------------------
	etr_platform::SetJoystickButton(0, BUTTON_CHARGE, readBool(g.jump));
	etr_platform::SetJoystickButton(0, BUTTON_TRICK, readBool(g.trick));

	// ---- pause / abort, as key events --------------------------------
	const bool pause_now = readBool(g.pause);
	if (pause_now != g.was_pause) {
		sendKey(sf::Keyboard::P, pause_now);
		g.was_pause = pause_now;
	}

	const bool menu_now = readBool(g.menu);
	if (menu_now != g.was_menu) {
		sendKey(sf::Keyboard::Escape, menu_now);
		g.was_menu = menu_now;
	}
}

void SetUiNavigation(bool enabled) {
	if (g.ui_mode == enabled) return;
	g.ui_mode = enabled;

	// Reset the edge-detection state on every switch, so a button still
	// held from the other mode does not immediately fire in this one.
	g.was_confirm = g.was_cancel = false;
	g.was_pause = g.was_menu = false;
	g.held_x = g.held_y = 0;
	g.repeat_at = 0.0;
}

}  // namespace vr

#endif  // ENABLE_OPENXR
