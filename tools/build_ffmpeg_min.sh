#!/usr/bin/env bash
# Builds the decode-only FFmpeg the Windows release ships (instead of BtbN's full gpl-shared build: 150 MB of
# DLLs with every encoder, filter and external library, of which the game uses none).
#
#   bash tools/build_ffmpeg_min.sh [ffmpeg source] [prefix]
#
# Run it in MSYS2's own shell (Git Bash's runtime differs from MSYS2's tools):
#   MSYSTEM=MINGW64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc "bash tools/build_ffmpeg_min.sh"
# Needs MSYS2 (mingw64 gcc, nasm, make: pacman -S make diffutils mingw-w64-x86_64-nasm mingw-w64-x86_64-dav1d).
# The source must be the FFmpeg commit BtbN's build was made from (same headers/ABI as the rest of the build):
# `ffmpeg -version` of .cmake-deps/ffmpeg_prebuilt-src/bin says N-xxxx-g<commit>.
#
# What the game uses (src/libs/video/av.h, src/libs/audio/audio.cpp, src/libs/optional/nub.cpp): demuxing local
# files (and a custom AVIO), every decoder, swscale, swresample. No encoders, muxers, filters, devices or network.
# AV1 decodes through dav1d (FFmpeg's own av1 decoder needs a hardware decoder), linked in statically like zlib,
# so the DLLs need nothing but Windows and msvcrt.
set -euo pipefail

SRC="${1:-$(cd "$(dirname "$0")/../.." && pwd)/ffmpeg-min/src}"
PREFIX="${2:-$(cd "$(dirname "$0")/../.." && pwd)/ffmpeg-min/prefix}"
export PATH="/mingw64/bin:/usr/bin:$PATH"
export TMPDIR="$(dirname "$PREFIX")/tmp"   # configure can't use a Windows-style %TEMP%
mkdir -p "$TMPDIR"

# static zlib / dav1d: a folder holding only the .a files, searched before mingw64/lib (whose .dll.a would win)
STATIC="$(dirname "$PREFIX")/static-libs"
mkdir -p "$STATIC/lib/pkgconfig"
cp /mingw64/lib/libz.a /mingw64/lib/libdav1d.a "$STATIC/lib/"
sed "s#^prefix=.*#prefix=/mingw64#; s#^libdir=.*#libdir=$STATIC/lib#" \
    /mingw64/lib/pkgconfig/dav1d.pc > "$STATIC/lib/pkgconfig/dav1d.pc"
export PKG_CONFIG_PATH="$STATIC/lib/pkgconfig"

BUILD="$(dirname "$PREFIX")/build"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" --prefix="$PREFIX" --arch=x86_64 --target-os=mingw32 \
    --enable-shared --disable-static --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --enable-w32threads --enable-zlib --enable-libdav1d \
    --disable-network --disable-avdevice --disable-avfilter \
    --disable-encoders --disable-muxers --disable-indevs --disable-outdevs --disable-filters \
    --disable-protocols --enable-protocol=file,pipe \
    --disable-hwaccels \
    --pkg-config-flags=--static \
    --extra-cflags="-O2" \
    --extra-ldflags="-L$STATIC/lib -static-libgcc -Wl,--gc-sections" \
    --extra-version=zdon-min
make -j"$(nproc)"
make install
strip --strip-unneeded "$PREFIX"/bin/*.dll
ls -l "$PREFIX"/bin/*.dll
for d in "$PREFIX"/bin/*.dll; do
    echo "$(basename "$d"): $(objdump -p "$d" | awk '/DLL Name/ {print $3}' | tr '\n' ' ')"
done
