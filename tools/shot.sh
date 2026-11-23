#!/usr/bin/env bash
# tools/shot.sh <seconds> <out.png>: wait, then take a screenshot of the connected device.
sleep "${1:-5}"
adb exec-out screencap -p > "${2:-shot.png}"
