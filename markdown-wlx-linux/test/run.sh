#!/bin/sh
# Build the plugin and run the real-artifact harness against it.
#
# Needs a display. Headless:
#   xvfb-run -a ./test/run.sh
# In a container/CI, also set QTWEBENGINE_DISABLE_SANDBOX=1.
set -eu
cd "$(dirname "$0")/.."

./build.sh

PKGS="Qt6Widgets Qt6WebEngineWidgets"
echo "==> Building harness"
g++ -std=c++17 -fPIC -o build/load_verify test/load_verify.cpp \
    $(pkg-config --cflags --libs $PKGS)

echo "==> Running harness against build/MarkdownView.wlx"
./build/load_verify build/MarkdownView.wlx
