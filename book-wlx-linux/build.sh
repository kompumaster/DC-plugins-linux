#!/bin/sh
# Build BookView.wlx for Double Commander on Linux (Qt 6 Widgets + WebEngine).
set -eu
cd "$(dirname "$0")"

OUT="build/BookView.wlx"
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
PKGS="Qt6Widgets Qt6WebEngineWidgets"

mkdir -p build/obj

echo "==> Compiling zipreader.c"
$CC -D_GNU_SOURCE -std=c11 -fPIC -O2 -Wall -Wextra -Wno-unknown-pragmas -c -o build/obj/zipreader.o zipreader.c

echo "==> Compiling BookView.cpp"
$CXX -std=c++17 -fPIC -O2 -fvisibility=hidden -Wall -Wextra \
  $(pkg-config --cflags $PKGS) -c -o build/obj/BookView.o BookView.cpp

echo "==> Linking $OUT"
$CXX -shared -o "$OUT" build/obj/BookView.o build/obj/zipreader.o \
  $(pkg-config --libs $PKGS) -lz

echo "==> Staging assets"
rm -rf build/assets
cp -R assets build/assets

echo "==> Exported symbols:"
nm -D --defined-only "$OUT" | grep ' T List' | sed 's/^/    /'

echo "==> Done: $OUT"
