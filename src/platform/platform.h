/* --------------------------------------------------------------------
EXTREME TUXRACER -- platform layer internals

Shared between the sf:: compatibility modules and the Android/VR entry
point. Not included by game code.
---------------------------------------------------------------------*/

#ifndef ETR_PLATFORM_H
#define ETR_PLATFORM_H

#include "SFML/Window.hpp"

#include <string>

struct android_app;

namespace etr_platform {

/** Initialises the GL compatibility shim and the input state. The EGL
 *  context itself is created earlier, by android_main(). */
bool Init();
void Shutdown();

/** Size of the virtual 2D canvas the HUD and menus are laid out against.
 *  This is not a physical surface: in VR the result is composited into the
 *  eye buffers, which have a different size and aspect entirely. */
void GetDrawableSize(unsigned int& width, unsigned int& height);
void SetCanvasSize(unsigned int width, unsigned int height);

/** Drains the event queue, pumping the Android looper on the way through.
 *  Returns false when nothing is pending. */
bool PollEvent(sf::Event& event);

// Implemented in android_main.cpp.
::android_app* GetAndroidApp();
void PumpAndroidEvents();
bool IsResumed();
bool IsDestroyRequested();

/** Injects a synthetic event, used by the OpenXR input layer to deliver
 *  controller state through the game's existing joystick handlers. */
void PushEvent(const sf::Event& event);

/** Current state of the virtual joystick that the VR input layer drives. */
void SetJoystickAxis(unsigned int joystick, sf::Joystick::Axis axis,
                     float position);
void SetJoystickButton(unsigned int joystick, unsigned int button,
                       bool pressed);
float GetJoystickAxis(unsigned int joystick, sf::Joystick::Axis axis);
bool GetJoystickButton(unsigned int joystick, unsigned int button);
void SetJoystickConnected(unsigned int joystick, bool connected);

// --------------------------------------------------------------------
//	Assets
// --------------------------------------------------------------------

/** Writable per-app directory (Android internal storage). Both the
 *  extracted game data and the saved config live under here. */
const std::string& GetInternalDataPath();

/** Unpacks data.zip out of the APK's assets into the internal data path,
 *  the first time the app runs or whenever the stamped version changes.
 *  Reports progress as a 0..1 fraction. */
bool ExtractGameData(void (*progress)(float fraction) = nullptr);

}  // namespace etr_platform

#endif  // ETR_PLATFORM_H
