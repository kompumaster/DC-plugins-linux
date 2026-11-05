#!/bin/sh
# Install MarkdownView.wlx into Double Commander's user plugin directory and
# register it in doublecmd.xml. Works from the source tree (builds first) or
# from a prebuilt release bundle (MarkdownView.wlx sits next to this script).
#
# Double Commander MUST be quit before running — it rewrites its config on exit.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd"
DEST="$CFG_DIR/plugins/wlx/MarkdownView"

if [ -f "$HERE/MarkdownView.wlx" ]; then
    WLX="$HERE/MarkdownView.wlx"          # prebuilt release bundle
else
    echo "==> Building from source"
    "$HERE/build.sh" >/dev/null
    WLX="$HERE/build/MarkdownView.wlx"
fi

echo "==> Installing files to $DEST"
mkdir -p "$DEST"
cp -f "$WLX" "$DEST/MarkdownView.wlx"
rm -rf "$DEST/assets"
cp -R "$HERE/assets" "$DEST/assets"
cp -f "$HERE/MarkdownView.ini.sample" "$DEST/MarkdownView.ini.sample"

if pgrep -x doublecmd >/dev/null 2>&1; then
    echo "!! Double Commander is running. Quit it, then re-run this script to register."
    exit 1
fi

if [ ! -f "$CFG_DIR/doublecmd.xml" ]; then
    echo "!! $CFG_DIR/doublecmd.xml not found. Start Double Commander once, quit it, then re-run."
    exit 1
fi

echo "==> Registering in $CFG_DIR/doublecmd.xml"
python3 "$HERE/register_plugin.py" "$CFG_DIR/doublecmd.xml" "$DEST/MarkdownView.wlx"

echo "==> Done. Launch Double Commander and open any .md file (F3)."
echo "    Optional settings: add a [MarkdownView] section to $CFG_DIR/wlx.ini"
echo "    (see $DEST/MarkdownView.ini.sample)."
