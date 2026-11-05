#!/bin/sh
# Install MediaInfo.wdx into Double Commander's user plugin directory and
# register it as a content plugin in doublecmd.xml. Works from the source tree
# (builds first) or from a prebuilt release bundle (MediaInfo.wdx next to this
# script).
#
# Double Commander MUST be quit before running — it rewrites its config on exit.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd"
DEST="$CFG_DIR/plugins/wdx/MediaInfo"

if [ -f "$HERE/MediaInfo.wdx" ]; then
    WDX="$HERE/MediaInfo.wdx"             # prebuilt release bundle
else
    echo "==> Building from source"
    "$HERE/build.sh" >/dev/null
    WDX="$HERE/build/MediaInfo.wdx"
fi

echo "==> Installing to $DEST"
mkdir -p "$DEST"
cp -f "$WDX" "$DEST/MediaInfo.wdx"

if pgrep -x doublecmd >/dev/null 2>&1; then
    echo "!! Double Commander is running. Quit it, then re-run this script to register."
    exit 1
fi

if [ ! -f "$CFG_DIR/doublecmd.xml" ]; then
    echo "!! $CFG_DIR/doublecmd.xml not found. Start Double Commander once, quit it, then re-run."
    exit 1
fi

echo "==> Registering in $CFG_DIR/doublecmd.xml"
python3 "$HERE/register_plugin.py" "$CFG_DIR/doublecmd.xml" "$DEST/MediaInfo.wdx"

cat <<'MSG'
==> Done. Launch Double Commander, then add a column:
      right-click the column header -> Configure / Columns...
      add a column and set its template to a plugin field:
        [Plugin(MediaInfo).Summary{}]
      (or Dimensions / Duration / Page count for a dedicated column).
MSG
