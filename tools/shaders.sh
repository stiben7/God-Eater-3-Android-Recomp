#!/usr/bin/env bash
# Compila los shaders propios del runtime (runtime/src/nnsdk/nvn/shaders/*.comp) a SPIR-V con el glslc del NDK y los
# deja como arrays en shaders/spirv.h (generado: se sube al repo para no depender de glslc al compilar).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# The NDK's glslc (macOS or Linux), or one on the PATH (GLSLC=… overrides both).
if [ -z "${GLSLC:-}" ]; then
  for host in darwin-x86_64 linux-x86_64; do
    cand="${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/28.2.13676358/shader-tools/$host/glslc"
    [ -x "$cand" ] && GLSLC="$cand" && break
  done
fi
GLSLC="${GLSLC:-$(command -v glslc || true)}"
[ -x "$GLSLC" ] || { echo "glslc not found (NDK 28.2.13676358 shader-tools, or set GLSLC)" >&2; exit 1; }
DIR="$ROOT/runtime/src/nnsdk/nvn/shaders"
FSR="$ROOT/third_party/eden/externals/FidelityFX-FSR/ffx-fsr"
OUT="$DIR/spirv.h"
# An FSR shader that doesn't compile here (they need Eden's externals/FidelityFX-FSR, which tools/setup.sh doesn't
# fetch) keeps the array it already has in spirv.h. Any other shader has to compile: an old array kept for a shader
# whose source changed would run the old code with the new executor (bc_decode with BC4/BC5 sent to it, say).
TMP="$(mktemp)"
trap 'rm -f "$TMP" "$DIR"/*.spv "$DIR"/*.err' EXIT
{
  echo "#pragma once"
  echo "// Generado por tools/shaders.sh: no editar."
  echo "#include <cstdint>"
  for src in "$DIR"/*.comp; do
    name=$(basename "$src" .comp)
    echo "#define HAVE_${name}_SPV 1"
    if "$GLSLC" -O --target-env=vulkan1.1 -I "$FSR" "$src" -o "$DIR/$name.spv" 2> "$DIR/$name.err"; then
      echo "inline constexpr uint32_t k_${name}_spv[] = {"
      python3 -c "import struct,sys; d=open('$DIR/$name.spv','rb').read(); w=struct.unpack('<%dI'%(len(d)//4),d); print(','.join('0x%08x'%x for x in w))"
      echo "};"
      rm "$DIR/$name.spv"
    elif grep -q "ffx_" "$src" && python3 - "$OUT" "$name" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"inline constexpr uint32_t k_%s_spv\[\] = \{\n.*?\n\};" % re.escape(sys.argv[2]), text, re.S)
if not m:
    sys.exit(1)
print(m.group(0))
PY
    then
      echo "$name: didn't compile here, kept its SPIR-V from spirv.h" >&2
    else
      cat "$DIR/$name.err" >&2
      exit 1
    fi
    rm -f "$DIR/$name.err"
  done
} > "$TMP"
mv "$TMP" "$OUT"
trap - EXIT
echo "$OUT"
