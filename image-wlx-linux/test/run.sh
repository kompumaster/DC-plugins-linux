#!/bin/sh
# Build the plugin, generate samples, and run the real-artifact harness.
# Needs a display; falls back to xvfb-run when neither DISPLAY nor
# WAYLAND_DISPLAY is set.
set -eu
cd "$(dirname "$0")/.."

./build.sh

mkdir -p build/samples
echo "==> Generating samples"
g++ -std=c++17 -fPIC -o build/make_samples test/make_samples.cpp \
    $(pkg-config --cflags --libs Qt6Gui) -lz
./build/make_samples build/samples >/dev/null

IM=""
if command -v magick >/dev/null 2>&1; then IM=magick
elif command -v convert >/dev/null 2>&1; then IM=convert
fi
if [ -n "$IM" ]; then
    frames=build/samples/.frames
    mkdir -p "$frames"
    "$IM" -size 32x32 xc:'#e33' "$frames/f0.png"
    "$IM" -size 32x32 xc:'#3e3' "$frames/f1.png"
    "$IM" -size 32x32 xc:'#33e' "$frames/f2.png"
    "$IM" -delay 20 -loop 0 "$frames/f0.png" "$frames/f1.png" "$frames/f2.png" \
        build/samples/anim.gif
    rm -rf "$frames"
    "$IM" build/samples/photo.png -resize 16x16! build/samples/.i16.png
    "$IM" build/samples/photo.png -resize 32x32! build/samples/.i32.png
    "$IM" build/samples/.i16.png build/samples/.i32.png build/samples/icon.ico
    rm -f build/samples/.i16.png build/samples/.i32.png
    "$IM" build/samples/photo.png build/samples/alpha.png build/samples/pages.tiff
    "$IM" build/samples/photo.png build/samples/photo.webp 2>/dev/null || true
else
    echo "!! ImageMagick not found: anim.gif / icon.ico / pages.tiff are skipped"
fi

echo "==> Building harness"
g++ -std=c++17 -fPIC -pthread -Wall -Wextra -o build/test_host test/test_host.cpp \
    $(pkg-config --cflags --libs Qt6Widgets)

echo "==> Running harness against build/ImageView.wlx"
if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
    ./build/test_host build/ImageView.wlx build/samples
else
    xvfb-run -a ./build/test_host build/ImageView.wlx build/samples
fi
