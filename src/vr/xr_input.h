/* --------------------------------------------------------------------
EXTREME TUXRACER -- OpenXR controller input

Quest Touch controllers are mapped onto the virtual joystick the platform
layer exposes, so CRacing::Jaxis and CRacing::Jbutt keep working exactly
as they do with a gamepad on the desktop build. Nothing in the game's
input handling needed to change.

  left thumbstick X  -> axis 0   steering
  right trigger      -> axis 1   paddle   (negative)
  left trigger       -> axis 1   brake    (positive)
  A / X              -> button 3 jump charge
  grip (either hand) -> button 1 trick modifier
  B                  -> P        pause
  menu               -> Escape   abort race
---------------------------------------------------------------------*/

#ifndef ETR_XR_INPUT_H
#define ETR_XR_INPUT_H

#ifdef ENABLE_OPENXR

class XRManager;

/** Creates the action set and suggests the Touch controller bindings.
 *  Must be called after the session exists. */
bool InitInput(XRManager* manager);
void ShutdownInput();

#endif  // ENABLE_OPENXR

#endif  // ETR_XR_INPUT_H
