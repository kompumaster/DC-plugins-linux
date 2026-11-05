# Double Commander plugins for Linux

Native Linux plugins for [Double Commander](https://doublecmd.sourceforge.io/),
a Linux port built on the macOS plugins in
[`NikolaiSachok/DC-plugins`](https://github.com/NikolaiSachok/DC-plugins) (MIT).
One directory per plugin, self-contained, built as an ELF shared object that DC
loads through the Total Commander plugin ABI, with headless harnesses that drive
the real built artifact.

| Plugin | Type | What it does |
|--------|------|--------------|
| [`markdown-wlx`](markdown-wlx/) | WLX (lister) | Renders Markdown in the viewer (F3) — GitHub-style CSS, syntax highlighting, tables, Mermaid, KaTeX, configurable light/dark. |
| [`media-info-wdx`](media-info-wdx/) | WDX (content) | Image/video resolution, audio/video duration, bitrate, codecs, PDF page count and an adaptive `Summary` for custom columns and tooltips. |
| [`image-wlx`](image-wlx/) | WLX (lister) | Image viewer with wheel/keyboard/toolbar zoom, drag panning, vector-sharp SVG, animations, and copy. |
| [`book-wlx`](book-wlx/) | WLX (lister) | EPUB 2/3 and FictionBook (FB2/FBZ) reader with a contents sidebar, themes, readable typography and whole-book search. |

The original macOS plugins live upstream:
[`NikolaiSachok/DC-plugins`](https://github.com/NikolaiSachok/DC-plugins). This
repository contains only the Linux ports; the upstream repository is the
source of truth for the macOS code and shared assets.

## Requirements

- Double Commander **1.2.x, Qt 6 build** (`doublecmd-qt6`)
- `g++`, `pkg-config`, `python3` (installers/tests)
- Per plugin:
  - **markdown**: Qt 6 Widgets + WebEngine dev files
  - **media-info**: Qt 6 Gui, FFmpeg (`libavformat`, `libavcodec`, `libavutil`), Poppler GLib, Qt image format plugins
  - **image**: Qt 6 Widgets + Svg + Concurrent, Qt image format plugins, zlib (`qt6-imageformats` / `kimageformats` for TIFF/WebP/HEIC/AVIF/JXL/RAW)
  - **book**: Qt 6 Widgets + WebEngine dev files, zlib

Arch packages: `qt6-base qt6-webengine qt6-svg qt6-imageformats ffmpeg poppler-glib`.
Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev qt6-svg-dev qt6-image-formats-plugins libavformat-dev libavcodec-dev libavutil-dev libpoppler-glib-dev ffmpeg`.

## Build / install

Quit Double Commander before running an installer (it rewrites its config on
exit). Each plugin is independent:

```sh
cd markdown-wlx      # or media-info-wdx / image-wlx / book-wlx
./build.sh                 # build/<Name>.wlx|.wdx (+ assets/ where needed)
./install.sh               # copies to ~/.config/doublecmd/plugins/... and registers
```

The installers are idempotent, back up `doublecmd.xml`, and register the plugin
before any catch-all lister. See each plugin's README for its settings, keys and
known limitations.

## Releases

Each release is cut from a `linux-vX.Y.Z` tag and publishes **one GitHub
release with four archives** plus `SHA256SUMS`:

```
markdown-wlx-<version>-linux.zip
media-info-wdx-<version>-linux.zip
image-wlx-<version>-linux.zip
book-wlx-<version>-linux.zip
SHA256SUMS
```

Unzip the one you need, quit Double Commander, run `./install.sh`, and verify the
download with `sha256sum -c SHA256SUMS`. The version in each archive name comes
from that plugin's own `VERSION` constant, so a release train can carry a newer
plugin next to unchanged ones. Archives are built for x86_64; on other
architectures build from source with `./build.sh`.

## Tests

Every plugin carries a headless (or xvfb-capable) harness that loads the real
built plugin the way DC does:

```sh
cd markdown-wlx && ./test/run.sh   # renders, keys/Esc, search, copy, links
cd media-info-wdx && ./test/run.sh # formats, PDF/WAV/video, threads, FP traps
cd image-wlx && ./test/run.sh      # formats, zoom, animation, SVG, clipboard
cd book-wlx && ./test/run.sh       # EPUB/FB2 DOM, sanitising, search, copy, ZIP
```

GUI-bound behaviour is also confirmed in a real Double Commander before a
release; the harnesses are a regression net, not proof.

## Repository layout

```
markdown-wlx/
media-info-wdx/
image-wlx/
book-wlx/
scripts/leak-guard.sh
.githooks/pre-commit
.github/workflows/    Linux CI and release workflows
```

The macOS plugins are not vendored here — they live upstream at
[`NikolaiSachok/DC-plugins`](https://github.com/NikolaiSachok/DC-plugins). Fixes
to them belong there; this repository only carries the Linux ports.

## Syncing with upstream

The Linux plugins share code and assets with the macOS ones (`zipreader.c`,
`reader.js`/`reader.css`, DOMPurify, Markdown's vendored libraries). Upstream
changes are brought over manually from
[`NikolaiSachok/DC-plugins`](https://github.com/NikolaiSachok/DC-plugins) — clone
it separately (or add it as a `git` remote) and port the change; see each
plugin's README for what diverges and why.

## License

The Linux ports are MIT-licensed, like the upstream project. The original
copyright notice is preserved in [`LICENSE`](LICENSE); see [`NOTICE`](NOTICE)
for attribution. Vendored JavaScript libraries keep their own licenses, listed
in each plugin's `THIRD_PARTY_LICENSES.md`.
