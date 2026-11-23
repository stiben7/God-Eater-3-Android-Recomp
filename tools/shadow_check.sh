#!/usr/bin/env bash
# Captures a frame with each given ini switch turned on (one at a time; none = the current settings) and prints how much of
# each shadow cascade (rt1 layers) the frame drew. Needs the game running in a scene with shadows.
# Usage: tools/shadow_check.sh [switch ...]     e.g. tools/shadow_check.sh no_deferred_clear no_store_ops no_area
export MSYS_NO_PATHCONV=1
D="/storage/FAFB-3360/Emu/Switch/God Eater 3 Recomp"
OUT="${TMPDIR:-${TEMP:-/tmp}}/shadow_check"
capture() {  # label
  adb shell "rm -rf /storage/emulated/0/NieR-volcados; printf 'completa' > '$D/trazar'"
  for i in $(seq 1 15); do sleep 1; adb shell "test -e '$D/trazar'" || break; done
  sleep 4
  rm -rf "$OUT" && mkdir -p "$OUT"
  adb pull /storage/emulated/0/NieR-volcados "$OUT/" > /dev/null 2>&1
  python - "$OUT" "$1" <<'PY'
import sys, glob, numpy as np
out, label = sys.argv[1], sys.argv[2]
def pgm(p):
    b = open(p, "rb").read(); i = b.index(b"\n"); h = b[:i].split()
    return np.frombuffer(b[i+1:], dtype=">u2" if int(h[3]) > 255 else np.uint8).reshape(int(h[2]), int(h[1]))
files = sorted(glob.glob(out + "/NieR-volcados/*rt1_c*_p16.pgm"))
if not files: print(f"{label:22s} no cascade images captured"); sys.exit()
parts = []
for p in files:
    a = pgm(p); parts.append("c%s: drawn %.2f%% zero %.0f%%" % (p.split("_c")[1].split("_")[0], 100 * ((a != 0) & (a != 65535)).mean(), 100 * (a == 0).mean()))
print(f"{label:22s} " + " | ".join(parts))
PY
}
capture "current settings"
for s in "$@"; do
  adb shell "printf '$s = 1\n' >> '$D/mhr.ini'"; sleep 4
  capture "$s"
  adb shell "sed -i '/^$s = /d' '$D/mhr.ini'"
done
