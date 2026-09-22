# Extreme Tux Racer Quest VR

[![Sponsor](https://img.shields.io/badge/Sponsor-SgtBilko76-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/SgtBilko76)

[Extreme Tux Racer](https://sourceforge.net/projects/extremetuxracer/) running standalone on Meta Quest in true stereo at 72 Hz. Race Tux the penguin down icy slopes and grab herrings along the way.

- Native OpenXR app. Each race is simulated once per frame and rendered once per eye.
- Menus are shown on a floating screen and navigated with the thumbstick.
- A GLES 3 compatibility layer runs the game's original OpenGL 1.2 renderer unchanged.

## Install

1. Enable developer mode on your Quest.
2. Download the APK from the [Releases](../../releases) page.
3. Sideload it with [SideQuest](https://sidequestvr.com/) or `adb install -r <file>.apk`.
4. Launch it from *Library → Unknown Sources*.

The game data is unpacked to the app's internal storage on first launch.

## Building

The Quest build lives in `android/` (Gradle + CMake, arm64-v8a, minSdk 29). The desktop build is unaffected. `vr-probe/` is a minimal standalone OpenXR app for checking the toolchain.

---

Original project: http://sourceforge.net/projects/extremetuxracer/
