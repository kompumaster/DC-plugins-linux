#!/bin/sh
# Build MediaInfo.wdx for Double Commander on Linux (Qt 6, FFmpeg, Poppler).
set -eu
cd "$(dirname "$0")"

OUT="build/MediaInfo.wdx"
CXX="${CXX:-g++}"
PKGS="Qt6Gui libavformat libavcodec libavutil poppler-glib"

mkdir -p build

echo "==> Compiling $OUT"
$CXX -std=c++17 -fPIC -shared -O2 -fvisibility=hidden -Wall -Wextra \
  $(pkg-config --cflags $PKGS) -o "$OUT" MediaInfo.cpp $(pkg-config --libs $PKGS)

echo "==> Exported symbols:"
nm -D --defined-only "$OUT" | grep ' T Content' | sed 's/^/    /'

echo "==> Done: $OUT"
