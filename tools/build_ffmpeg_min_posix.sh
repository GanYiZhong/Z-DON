#!/usr/bin/env bash
# Linux / macOS counterpart of tools/build_ffmpeg_min.sh: a static, decode-only FFmpeg for the release builds
# (cmake -DYATAIDON_FFMPEG_PREFIX=<prefix>). The distribution's FFmpeg pulled every encoder library into the
# Linux package (x265, codec2, SVT-AV1, aom, ...: 178 MB); this links only what the game uses into the executable.
#
#   bash tools/build_ffmpeg_min_posix.sh [prefix]
#
# Same FFmpeg commit as the Windows build (FFMPEG_COMMIT), so every platform decodes alike: every decoder,
# demuxer and parser, file/pipe; no encoders, muxers, filters, devices or network. AV1 through dav1d when
# pkg-config finds it (linked as the system has it: the release workflow bundles libdav1d).
set -euo pipefail

FFMPEG_COMMIT="${FFMPEG_COMMIT:-3d16b774d71bb9cf25e9da0fb7886ac11640d712}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${1:-$REPO_ROOT/.ffmpeg-min}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ffmpeg-min.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

curl --fail --location --retry 3 "https://github.com/FFmpeg/FFmpeg/archive/$FFMPEG_COMMIT.tar.gz" -o "$WORK/src.tgz"
tar -xzf "$WORK/src.tgz" -C "$WORK"
cd "$WORK/FFmpeg-$FFMPEG_COMMIT"

DAV1D=""   # a plain string: macOS's bash 3.2 rejects an empty array under set -u
if pkg-config --exists dav1d; then
    DAV1D="--enable-libdav1d"
fi

./configure --prefix="$PREFIX" \
    --enable-static --disable-shared --enable-pic \
    --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --enable-pthreads --enable-zlib $DAV1D \
    --disable-network --disable-avdevice --disable-avfilter \
    --disable-encoders --disable-muxers --disable-indevs --disable-outdevs --disable-filters \
    --disable-protocols --enable-protocol=file,pipe \
    --disable-hwaccels \
    --extra-version=zdon-min
make -j"$JOBS"
make install
echo "FFmpeg (decode-only, static) in $PREFIX ${DAV1D}"
