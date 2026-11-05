#!/bin/sh
# Build ImageView.wlx for Double Commander on Linux (Qt 6 Widgets + SVG, zlib).
set -eu
cd "$(dirname "$0")"

OUT="build/ImageView.wlx"
CXX="${CXX:-g++}"
PKGS="Qt6Widgets Qt6Svg Qt6Concurrent"

mkdir -p build

echo "==> Compiling $OUT"
$CXX -std=c++17 -fPIC -shared -O2 -fvisibility=hidden -Wall -Wextra \
  $(pkg-config --cflags $PKGS) -o "$OUT" ImageView.cpp \
  $(pkg-config --libs $PKGS) -lz

echo "==> Exported symbols:"
nm -D --defined-only "$OUT" | grep ' T List' | sed 's/^/    /'

echo "==> Done: $OUT"
