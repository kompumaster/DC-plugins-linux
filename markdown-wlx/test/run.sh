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
if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
    ./build/load_verify build/MarkdownView.wlx
else
    # Headless WebEngine: software QtQuick + no sandbox + software GL.
    export QTWEBENGINE_DISABLE_SANDBOX="${QTWEBENGINE_DISABLE_SANDBOX:-1}"
    export QT_QUICK_BACKEND="${QT_QUICK_BACKEND:-software}"
    export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
    xvfb-run -a ./build/load_verify build/MarkdownView.wlx
fi
