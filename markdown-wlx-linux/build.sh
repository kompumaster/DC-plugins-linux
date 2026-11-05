#!/bin/sh
# Build MarkdownView.wlx for Double Commander on Linux (Qt 6 + QtWebEngine).
set -eu
cd "$(dirname "$0")"

OUT="build/MarkdownView.wlx"
CXX="${CXX:-g++}"

mkdir -p build

PKGS="Qt6Widgets Qt6WebEngineWidgets"
CFLAGS=$(pkg-config --cflags $PKGS)
LIBS=$(pkg-config --libs $PKGS)

echo "==> Compiling $OUT"
$CXX -std=c++17 -fPIC -shared -O2 -fvisibility=hidden -Wall -Wextra \
  $CFLAGS -o "$OUT" MarkdownView.cpp $LIBS

echo "==> Staging assets"
rm -rf build/assets
cp -R assets build/assets

echo "==> Exported symbols:"
nm -D --defined-only "$OUT" | grep ' T List' | sed 's/^/    /'

echo "==> Done: $OUT"
