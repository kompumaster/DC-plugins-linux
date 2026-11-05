#!/bin/sh
# Install BookView.wlx into Double Commander's user plugin directory and
# register it in doublecmd.xml. Works from the source tree (builds first) or
# from a prebuilt release bundle (BookView.wlx next to this script).
#
# Double Commander MUST be quit before running — it rewrites its config on exit.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd"
DEST="$CFG_DIR/plugins/wlx/BookView"

if [ -f "$HERE/BookView.wlx" ]; then
    WLX="$HERE/BookView.wlx"              # prebuilt release bundle
else
    echo "==> Building from source"
    "$HERE/build.sh" >/dev/null
    WLX="$HERE/build/BookView.wlx"
fi

echo "==> Installing to $DEST"
mkdir -p "$DEST"
cp -f "$WLX" "$DEST/BookView.wlx"
rm -rf "$DEST/assets"
cp -R "$HERE/assets" "$DEST/assets"
cp -f "$HERE/BookView.ini.sample" "$DEST/BookView.ini.sample"

if pgrep -x doublecmd >/dev/null 2>&1; then
    echo "!! Double Commander is running. Quit it, then re-run this script to register."
    exit 1
fi

if [ ! -f "$CFG_DIR/doublecmd.xml" ]; then
    echo "!! $CFG_DIR/doublecmd.xml not found. Start Double Commander once, quit it, then re-run."
    exit 1
fi

echo "==> Registering in $CFG_DIR/doublecmd.xml"
python3 "$HERE/register_plugin.py" "$CFG_DIR/doublecmd.xml" "$DEST/BookView.wlx"

cat <<'MSG'
==> Done. Press F3 on an .epub, .fb2 or .fbz file.
MSG
