#!/bin/sh
# Build, install and launch the VR probe on an attached Quest.
set -e
cd "$(dirname "$0")"
./gradlew assembleDebug
adb install -r build/outputs/apk/debug/vrprobe-debug.apk
adb shell am start -n org.etr.vrprobe/android.app.NativeActivity
echo "--- logcat (ctrl-c to stop) ---"
adb logcat -c
adb logcat -s ETRVR:V
