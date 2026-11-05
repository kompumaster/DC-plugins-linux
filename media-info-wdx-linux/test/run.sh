#!/bin/sh
# Build the plugin, generate media samples with ffmpeg, and run the
# real-artifact harness against the built .wdx. No display needed.
set -eu
cd "$(dirname "$0")/.."

./build.sh

mkdir -p build/samples
FFMPEG="${FFMPEG:-ffmpeg}"
if command -v "$FFMPEG" >/dev/null 2>&1; then
    echo "==> Generating video samples"
    "$FFMPEG" -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=25 -t 2 \
        -c:v mpeg4 build/samples/sample.avi
    "$FFMPEG" -loglevel error -y -f lavfi -i testsrc=size=640x480:rate=24 -t 1 \
        -c:v mpeg4 build/samples/sample.mkv
    "$FFMPEG" -loglevel error -y -f lavfi -i testsrc=size=480x270:rate=30 \
        -f lavfi -i sine=frequency=440:duration=1 -t 1 \
        -c:v mpeg4 -c:a aac build/samples/sample.mp4
else
    echo "!! ffmpeg not found: video checks will be skipped"
fi

echo "==> Building harness"
g++ -std=c++17 -fPIC -Wall -Wextra -pthread -o build/test_host test/test_host.cpp \
    $(pkg-config --cflags --libs Qt6Gui)

echo "==> Running harness against build/MediaInfo.wdx"
./build/test_host build/MediaInfo.wdx build/samples
