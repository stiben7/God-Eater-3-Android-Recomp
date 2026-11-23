#!/usr/bin/env bash
# Builds a minimal static FFmpeg (MP4 demuxer + H.264/AAC parsers, LGPL only) for arm64 Android into
# third_party/ffmpeg-android/. Only libavformat is used at run time (to read the game's videos when Android's own
# extractor finds no tracks); decoding stays in MediaCodec. Run once, before tools/build_apk.sh. If this folder is
# missing the APK still builds, just without the FFmpeg fallback.
# Construye un FFmpeg mínimo y estático (demuxer MP4, LGPL) para arm64 en third_party/ffmpeg-android/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
NDK="${NDK_DIR:-$SDK/ndk/28.2.13676358}"
case "$(uname -s)" in
  Darwin) HOST_TAG=darwin-x86_64 ;;
  *) HOST_TAG=linux-x86_64 ;;
esac
TCB="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
[ -d "$TCB" ] || { echo "NDK not found: $NDK" >&2; exit 1; }

VER=9.0.2  # same as gu-native
SHA=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
TP="$ROOT/third_party"
SRC="$TP/ffmpeg-src"
OUT="$TP/ffmpeg-android"
BUILD="$TP/ffmpeg-build"
API=30
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)}"

[ -f "$OUT/lib/libavformat.a" ] && { echo "ffmpeg: already built ($OUT)"; exit 0; }

mkdir -p "$TP"
if [ ! -d "$SRC" ]; then
  tmp="$TP/ffmpeg-dl"
  mkdir -p "$tmp"
  curl -fsSL -o "$tmp/ffmpeg.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
  echo "$SHA  $tmp/ffmpeg.tar.xz" | (shasum -a 256 -c - 2> /dev/null || sha256sum -c -)
  tar xf "$tmp/ffmpeg.tar.xz" -C "$tmp"
  mv "$tmp/ffmpeg-$VER" "$SRC"
  rm -rf "$tmp"
fi

export PATH="$TCB/bin:$PATH"
rm -rf "$BUILD" "$OUT"
mkdir -p "$BUILD" "$OUT"
cd "$BUILD"
"$SRC/configure" --prefix="$OUT" \
  --enable-cross-compile --target-os=android --arch=aarch64 \
  --cc="aarch64-linux-android$API-clang" --cxx="aarch64-linux-android$API-clang++" \
  --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip \
  --sysroot="$TCB/sysroot" --enable-pic \
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov \
  --enable-parser=h264,aac --enable-decoder=h264,aac > configure.log 2>&1 \
  || { tail -30 configure.log; exit 1; }
make -j"$JOBS" install-libs install-headers > build.log 2>&1 || { tail -40 build.log; exit 1; }
echo "ffmpeg: ok -> $OUT"
