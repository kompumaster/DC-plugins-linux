#!/bin/sh
# Build the plugin, generate sample books, run the headless ZIP checks and the
# real-artifact harness. Needs a display; falls back to xvfb-run.
set -eu
cd "$(dirname "$0")/.."

./build.sh

echo "==> Generating sample books"
python3 test/make_samples.py build/samples >/dev/null
printf 'this is not an epub' > build/samples/garbage.epub

echo "==> ZIP reader checks"
gcc -D_GNU_SOURCE -O1 -Wall -Wextra -o build/zip_test test/zip_test.c zipreader.c -lz
./build/zip_test build/samples

echo "==> Building harness"
g++ -std=c++17 -fPIC -pthread -Wall -Wextra -o build/test_host test/test_host.cpp \
    $(pkg-config --cflags --libs Qt6Widgets Qt6WebEngineWidgets)

echo "==> Running harness against build/BookView.wlx"
if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
    ./build/test_host build/BookView.wlx build/samples
else
    # Headless WebEngine: software QtQuick + no sandbox + software GL.
    export QTWEBENGINE_DISABLE_SANDBOX="${QTWEBENGINE_DISABLE_SANDBOX:-1}"
    export QT_QUICK_BACKEND="${QT_QUICK_BACKEND:-software}"
    export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
    xvfb-run -a ./build/test_host build/BookView.wlx build/samples
fi
