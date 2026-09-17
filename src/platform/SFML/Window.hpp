/* --------------------------------------------------------------------
EXTREME TUXRACER -- SFML compatibility layer for Android

Window module: keyboard/mouse/joystick enums, the event union, and the
window/context objects. Input is fed from SDL2, and on the Quest the
joystick axes are driven by the OpenXR controller action set (see
src/vr/xr_input.cpp), so the game's existing Jaxis/Jbutt handlers work
unchanged.
---------------------------------------------------------------------*/

#ifndef ETR_SFML_WINDOW_HPP
#define ETR_SFML_WINDOW_HPP

#include "System.hpp"

namespace sf {

// --------------------------------------------------------------------
//	Keyboard
// --------------------------------------------------------------------

class Keyboard {
public:
	// Values follow SFML 2.x ordering. The game stores these in its config
	// file, so the numbering must stay stable across runs.
	enum Key {
		Unknown = -1,
		A = 0, B, C, D, E, F, G, H, I, J, K, L, M,
		N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
		Num0, Num1, Num2, Num3, Num4,
		Num5, Num6, Num7, Num8, Num9,
		Escape,
		LControl, LShift, LAlt, LSystem,
		RControl, RShift, RAlt, RSystem,
		Menu, LBracket, RBracket, SemiColon, Comma, Period, Quote,
		Slash, BackSlash, Tilde, Equal, Dash,
		Space, Return, BackSpace, Tab,
		PageUp, PageDown, End, Home, Insert, Delete,
		Add, Subtract, Multiply, Divide,
		Left, Right, Up, Down,
		Numpad0, Numpad1, Numpad2, Numpad3, Numpad4,
		Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
		F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
		F13, F14, F15,
		Pause,
		KeyCount
	};

	static bool isKeyPressed(Key key);
};

// --------------------------------------------------------------------
//	Mouse
// --------------------------------------------------------------------

class Window;

class Mouse {
public:
	enum Button { Left = 0, Right, Middle, XButton1, XButton2, ButtonCount };

	static Vector2i getPosition();
	static Vector2i getPosition(const Window& relativeTo);
};

// --------------------------------------------------------------------
//	Joystick
// --------------------------------------------------------------------

class Joystick {
public:
	enum { Count = 8, ButtonCount = 32, AxisCount = 8 };
	enum Axis { X = 0, Y, Z, R, U, V, PovX, PovY };

	static bool isConnected(unsigned int joystick);
	static unsigned int getButtonCount(unsigned int joystick);
	static bool hasAxis(unsigned int joystick, Axis axis);
	static float getAxisPosition(unsigned int joystick, Axis axis);
	static bool isButtonPressed(unsigned int joystick, unsigned int button);
};

// --------------------------------------------------------------------
//	Event
// --------------------------------------------------------------------

class Event {
public:
	struct SizeEvent { unsigned int width, height; };
	struct KeyEvent {
		Keyboard::Key code;
		bool alt, control, shift, system;
	};
	struct TextEvent { Uint32 unicode; };
	struct MouseMoveEvent { int x, y; };
	struct MouseButtonEvent { Mouse::Button button; int x, y; };
	struct MouseWheelScrollEvent { int wheel; float delta; int x, y; };
	struct JoystickMoveEvent {
		unsigned int joystickId;
		Joystick::Axis axis;
		float position;
	};
	struct JoystickButtonEvent { unsigned int joystickId, button; };
	struct JoystickConnectEvent { unsigned int joystickId; };

	enum EventType {
		Closed,
		Resized,
		LostFocus,
		GainedFocus,
		TextEntered,
		KeyPressed,
		KeyReleased,
		MouseWheelScrolled,
		MouseButtonPressed,
		MouseButtonReleased,
		MouseMoved,
		MouseEntered,
		MouseLeft,
		JoystickButtonPressed,
		JoystickButtonReleased,
		JoystickMoved,
		JoystickConnected,
		JoystickDisconnected,
		Count
	};

	EventType type;

	union {
		SizeEvent             size;
		KeyEvent              key;
		TextEvent             text;
		MouseMoveEvent        mouseMove;
		MouseButtonEvent      mouseButton;
		MouseWheelScrollEvent mouseWheelScroll;
		JoystickMoveEvent     joystickMove;
		JoystickButtonEvent   joystickButton;
		JoystickConnectEvent  joystickConnect;
	};
};

// --------------------------------------------------------------------
//	Video mode / context settings / style
// --------------------------------------------------------------------

class VideoMode {
public:
	unsigned int width, height, bitsPerPixel;

	VideoMode() : width(0), height(0), bitsPerPixel(32) {}
	VideoMode(unsigned int w, unsigned int h, unsigned int bpp = 32)
		: width(w), height(h), bitsPerPixel(bpp) {}

	static VideoMode getDesktopMode();
};

class ContextSettings {
public:
	unsigned int depthBits, stencilBits, antialiasingLevel;
	unsigned int majorVersion, minorVersion;

	explicit ContextSettings(unsigned int depth = 0, unsigned int stencil = 0,
	                         unsigned int antialiasing = 0,
	                         unsigned int major = 1, unsigned int minor = 1)
		: depthBits(depth), stencilBits(stencil),
		  antialiasingLevel(antialiasing),
		  majorVersion(major), minorVersion(minor) {}
};

namespace Style {
enum {
	None       = 0,
	Titlebar   = 1 << 0,
	Resize     = 1 << 1,
	Close      = 1 << 2,
	Fullscreen = 1 << 3,
	Default    = Titlebar | Resize | Close
};
}

// --------------------------------------------------------------------
//	Window
// --------------------------------------------------------------------

/** On Android there is exactly one window, owned by SDL. The size/style
 *  arguments are accepted and ignored -- the display is whatever the
 *  compositor gives us. */
class Window {
public:
	Window();
	virtual ~Window();

	void create(VideoMode mode, const String& title,
	            Uint32 style = Style::Default,
	            const ContextSettings& settings = ContextSettings());
	void close();
	bool isOpen() const;

	Vector2u getSize() const;
	void setMouseCursorVisible(bool visible);
	void setVerticalSyncEnabled(bool enabled);
	void setKeyRepeatEnabled(bool enabled);
	void setFramerateLimit(unsigned int limit);
	void setActive(bool active = true) const;

	bool pollEvent(Event& event);
	void display();

protected:
	bool m_open;
};

/** SFML uses this to fetch GL extension pointers. On GLES the one
 *  extension the game looks for (GL_EXT_compiled_vertex_array) does not
 *  exist, so this always reports failure and the game falls back cleanly. */
class Context {
public:
	static void* getFunction(const char* name);
};

}  // namespace sf

#endif  // ETR_SFML_WINDOW_HPP
