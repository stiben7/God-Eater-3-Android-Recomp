#!/usr/bin/env bash
# Dev loop for the God Eater 3 port on the connected Android device: reinstall the CI APK (keeping the app's private
# files, e.g. an imported GPU driver, across the uninstall that a new CI signing key forces), start the game, print the log.
# Usage: tools/ge3_run.sh [apk] [seconds to wait]
set -u
export MSYS_NO_PATHCONV=1
P=io.github.stiben7.ge3recomp
D="/storage/FAFB-3360/Emu/Switch/God Eater 3 Recomp"
APK="${1:-apk/ge3recomp/mhriseport.apk}"
BAK="${TMPDIR:-${TEMP:-/tmp}}/ge3_app_files.tar"
rm -f "$BAK"
adb exec-out "run-as $P tar -cf - files" > "$BAK" 2>/dev/null
[ -s "$BAK" ] && echo "backed up app files ($(wc -c < "$BAK") bytes)" || echo "no app files to back up"
adb uninstall $P >/dev/null 2>&1
adb install "$APK" 2>&1 | tail -1
adb shell appops set $P MANAGE_EXTERNAL_STORAGE allow
if [ -s "$BAK" ]; then
  adb push "$BAK" /data/local/tmp/ge3_files.tar > /dev/null
  adb shell "chmod 644 /data/local/tmp/ge3_files.tar; run-as $P tar -xf /data/local/tmp/ge3_files.tar; rm /data/local/tmp/ge3_files.tar"
fi
adb shell "run-as $P sh -c \"mkdir -p files && printf '%s' '$D' > files/carpeta.txt && printf '%s' '$D' > files/game_folder.txt\""
adb shell input keyevent KEYCODE_WAKEUP
adb shell rm -f "$D/log.txt"
adb logcat -c
adb shell am start -n "$P/io.github.iamwilmar.mhriseport.GameActivity" >/dev/null 2>&1
sleep "${2:-30}"
echo "pid: $(adb shell pidof $P)"
adb shell "grep -v -E '^\[(hid|entrada)\]|^  stub:' '$D/log.txt'" | cut -c1-200
