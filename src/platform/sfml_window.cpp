/* --------------------------------------------------------------------
EXTREME TUXRACER -- SFML compatibility layer: Window module

There is no window in the desktop sense: the OpenXR runtime owns every
surface the player actually sees, and the EGL context lives in
android_main.cpp. What remains here is the event queue and the input
state the game polls.

The joystick is virtual. Nothing is plugged into a Quest, so the OpenXR
controller action set writes axis and button state here (see
src/vr/xr_input.cpp) and CRacing::Jaxis / CRacing::Jbutt keep working
untouched.
---------------------------------------------------------------------*/

#include "SFML/Window.hpp"
#include "platform.h"

#include "../gles/gl_compat.h"

#include <android/log.h>

#include <deque>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "ETRPlat", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRPlat", __VA_ARGS__)

namespace etr_platform {

// Provided by android_main.cpp.
void PumpAndroidEvents();
bool IsDestroyRequested();

namespace {

struct WindowState {
	bool open = false;

	// The layout resolution for the 2D/HUD pass. In VR this is a virtual
	// canvas rather than any physical surface, so it is chosen for a sane
	// widget layout rather than measured from a display.
	unsigned int width = 1280;
	unsigned int height = 720;

	std::deque<sf::Event> queue;

	bool joy_connected[sf::Joystick::Count] = {false};
	float joy_axis[sf::Joystick::Count][sf::Joystick::AxisCount] = {{0.f}};
	bool joy_button[sf::Joystick::Count][sf::Joystick::ButtonCount] = {{false}};

	int mouse_x = 0, mouse_y = 0;
	bool keys[sf::Keyboard::KeyCount] = {false};
};

WindowState g;

}  // namespace

// --------------------------------------------------------------------
//	Lifecycle
// --------------------------------------------------------------------

bool Init() {
	if (!etrgl_Init()) {
		LOGE("GL compatibility shim failed to initialise");
		return false;
	}

	// Joystick 0 always reports connected: the Touch controllers are mapped
	// onto it, and the game only polls a joystick it believes exists.
	g.joy_connected[0] = true;
	g.open = true;
	LOGI("platform ready, 2D canvas %ux%u", g.width, g.height);
	return true;
}

void Shutdown() {
	etrgl_Shutdown();
	g.open = false;
}

void GetDrawableSize(unsigned int& width, unsigned int& height) {
	width = g.width;
	height = g.height;
}

void SetCanvasSize(unsigned int width, unsigned int height) {
	if (width == 0 || height == 0) return;
	g.width = width;
	g.height = height;
}

bool PollEvent(sf::Event& event) {
	// The Android looper is drained here because this is the one call the
	// game makes every single frame, in every state.
	PumpAndroidEvents();

	if (IsDestroyRequested()) {
		event.type = sf::Event::Closed;
		return true;
	}

	if (!g.queue.empty()) {
		event = g.queue.front();
		g.queue.pop_front();
		return true;
	}
	return false;
}

void PushEvent(const sf::Event& event) { g.queue.push_back(event); }

void SetJoystickAxis(unsigned int joystick, sf::Joystick::Axis axis,
                     float position) {
	if (joystick >= sf::Joystick::Count) return;
	if (g.joy_axis[joystick][axis] == position) return;

	g.joy_axis[joystick][axis] = position;

	sf::Event ev;
	ev.type = sf::Event::JoystickMoved;
	ev.joystickMove.joystickId = joystick;
	ev.joystickMove.axis = axis;
	// The game divides the incoming position by 100, matching SFML's
	// -100..100 axis range, so scale up from the normalised OpenXR value.
	ev.joystickMove.position = position * 100.f;
	PushEvent(ev);
}

void SetJoystickButton(unsigned int joystick, unsigned int button,
                       bool pressed) {
	if (joystick >= sf::Joystick::Count ||
	    button >= sf::Joystick::ButtonCount)
		return;
	if (g.joy_button[joystick][button] == pressed) return;

	g.joy_button[joystick][button] = pressed;

	sf::Event ev;
	ev.type = pressed ? sf::Event::JoystickButtonPressed
	                  : sf::Event::JoystickButtonReleased;
	ev.joystickButton.joystickId = joystick;
	ev.joystickButton.button = button;
	PushEvent(ev);
}

float GetJoystickAxis(unsigned int joystick, sf::Joystick::Axis axis) {
	if (joystick >= sf::Joystick::Count) return 0.f;
	return g.joy_axis[joystick][axis];
}

bool GetJoystickButton(unsigned int joystick, unsigned int button) {
	if (joystick >= sf::Joystick::Count ||
	    button >= sf::Joystick::ButtonCount)
		return false;
	return g.joy_button[joystick][button];
}

void SetJoystickConnected(unsigned int joystick, bool connected) {
	if (joystick < sf::Joystick::Count) g.joy_connected[joystick] = connected;
}

}  // namespace etr_platform

// --------------------------------------------------------------------
//	sf:: surface
// --------------------------------------------------------------------

namespace sf {

bool Keyboard::isKeyPressed(Key key) {
	if (key < 0 || key >= KeyCount) return false;
	return etr_platform::g.keys[key];
}

Vector2i Mouse::getPosition() {
	return Vector2i(etr_platform::g.mouse_x, etr_platform::g.mouse_y);
}
Vector2i Mouse::getPosition(const Window&) { return getPosition(); }

bool Joystick::isConnected(unsigned int joystick) {
	return joystick < Count && etr_platform::g.joy_connected[joystick];
}

unsigned int Joystick::getButtonCount(unsigned int joystick) {
	return isConnected(joystick) ? ButtonCount : 0;
}

bool Joystick::hasAxis(unsigned int joystick, Axis axis) {
	// The virtual pad exposes only the two axes the game steers with;
	// advertising the rest would just confuse its config screen.
	return isConnected(joystick) && (axis == X || axis == Y);
}

float Joystick::getAxisPosition(unsigned int joystick, Axis axis) {
	return etr_platform::GetJoystickAxis(joystick, axis);
}

bool Joystick::isButtonPressed(unsigned int joystick, unsigned int button) {
	return etr_platform::GetJoystickButton(joystick, button);
}

VideoMode VideoMode::getDesktopMode() {
	unsigned int w, h;
	etr_platform::GetDrawableSize(w, h);
	return VideoMode(w, h, 32);
}

Window::Window() : m_open(false) {}
Window::~Window() {}

void Window::create(VideoMode mode, const String&, Uint32,
                    const ContextSettings&) {
	// The context already exists and cannot be recreated, but the game does
	// use this to choose its 2D layout resolution.
	etr_platform::SetCanvasSize(mode.width, mode.height);
	m_open = true;
}

void Window::close() { m_open = false; }
bool Window::isOpen() const { return m_open; }

Vector2u Window::getSize() const {
	unsigned int w, h;
	etr_platform::GetDrawableSize(w, h);
	return Vector2u(w, h);
}

void Window::setMouseCursorVisible(bool) {}
void Window::setVerticalSyncEnabled(bool) {}
void Window::setKeyRepeatEnabled(bool) {}
void Window::setFramerateLimit(unsigned int) {}
void Window::setActive(bool) const {}

bool Window::pollEvent(Event& event) {
	return etr_platform::PollEvent(event);
}

void Window::display() {
	// Frames are presented by xrEndFrame; there is nothing to swap.
}

void* Context::getFunction(const char*) {
	// The game uses this for exactly one thing: resolving
	// glLockArraysEXT/glUnlockArraysEXT. That extension does not exist on
	// GLES, and eglGetProcAddress is free to hand back a non-null pointer
	// to a function the driver does not implement -- which would crash
	// quadtree.cpp's null-guarded fast path. Reporting failure is correct.
	return nullptr;
}

}  // namespace sf
