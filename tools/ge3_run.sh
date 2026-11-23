#!/usr/bin/env bash
# Dev loop for the God Eater 3 port on the connected Android device: reinstall the CI APK, start the game, print the log.
# Usage: tools/ge3_run.sh [apk] [seconds to wait]
set -u
export MSYS_NO_PATHCONV=1
P=io.github.stiben7.ge3recomp
D="/storage/FAFB-3360/Emu/Switch/God Eater 3 Recomp"
APK="${1:-apk/ge3recomp/mhriseport.apk}"
adb uninstall $P >/dev/null 2>&1
adb install "$APK" 2>&1 | tail -1
adb shell appops set $P MANAGE_EXTERNAL_STORAGE allow
adb shell "run-as $P sh -c \"mkdir -p files && printf '%s' '$D' > files/carpeta.txt && printf '%s' '$D' > files/game_folder.txt\""
adb shell input keyevent KEYCODE_WAKEUP
adb shell rm -f "$D/log.txt"
adb logcat -c
adb shell am start -n "$P/io.github.iamwilmar.mhriseport.GameActivity" >/dev/null 2>&1
sleep "${2:-30}"
echo "pid: $(adb shell pidof $P)"
adb shell "grep -v -E '^\[(hid|entrada)\]|^  stub:' '$D/log.txt'" | cut -c1-200
