# Contributing

Thanks for your interest. This collection values small, correct, well-tested
native plugins over feature sprawl.

## Prerequisites

- Linux with Double Commander 1.2.x (Qt 6 build)
- `g++`, `pkg-config`, `python3`
- Python 3 for the sample generators
- Per-plugin libraries: Qt 6 Widgets/WebEngine/Svg, FFmpeg, Poppler, zlib
  (see the root [README](README.md#requirements) and each plugin's README)

## Working with agents

The repository charter is [AGENTS.md](AGENTS.md); `CLAUDE.md` is a relative
symlink to it. Enable the pre-commit hook once per clone:

```sh
git config core.hooksPath .githooks
```

## Build / test loop (per plugin)

Each plugin is self-contained in its own directory:

```sh
cd markdown-wlx-linux      # or media-info-wdx-linux / image-wlx-linux / book-wlx-linux
./build.sh                 # build/<Name>.wlx|.wdx

./test/run.sh              # builds the plugin and runs the real-artifact harness
```

The harnesses `dlopen` the actual built plugin and drive the ABI the way DC
does. QtWebEngine plugins need a display; `test/run.sh` falls back to
`xvfb-run` when neither `DISPLAY` nor `WAYLAND_DISPLAY` is set. In containers
also set `QTWEBENGINE_DISABLE_SANDBOX=1`.

Before committing:

```sh
bash scripts/leak-guard.sh      # must print "clean ✓"
```

### Pre-commit hook (deterministic gate)

[`.githooks/pre-commit`](.githooks/pre-commit) runs the **Tier 1** gate:
leak-guard on every commit, plus an ELF build + ABI-export check whenever a
plugin's native source is staged. It is fast and blocking; bypass in an
emergency with `git commit --no-verify`. Independent semantic review is **not**
part of the hook — run it at push / PR / release boundaries on the accumulated
diff.

## Plugin layout convention

```
<name>-<type>-linux/      # e.g. markdown-wlx-linux
├── <Name>.cpp            # implementation (C++/Qt; plain C for shared readers)
├── listplug.h            # the relevant slice of the DC plugin ABI
├── build.sh              # produces build/<Name>.<ext>
├── install.sh            # installs + registers (idempotent; backs up config)
├── register_plugin.py    # optional: edits doublecmd.xml
├── assets/               # vendored runtime assets (attributed)
├── test/                 # harnesses that load the real built plugin
├── README.md             # what it does, install, keys, config, limitations
└── THIRD_PARTY_LICENSES.md
```

See [docs/ADDING-A-PLUGIN.md](docs/ADDING-A-PLUGIN.md) to scaffold a new one.

## Upstream snapshot

[`_mac_repo/`](_mac_repo/) is a read-only snapshot of the macOS project these
plugins were ported from. Do not edit or build it here. If upstream changes
shared code (`zipreader.c`, `reader.js`/`reader.css`, DOMPurify, Markdown
assets), port the change deliberately and note the divergence in the plugin's
README.

## Code style

- C++17, Qt 6; match the surrounding code; 4-space indent (see `.editorconfig`).
- Keep the C ABI surface minimal and `extern "C"`-clean — export only what DC
  calls, with `-fvisibility=hidden`.
- Mask floating-point exceptions around library work (DC is an FPC app and
  enables traps).
- No network at view time. The book reader's loopback server is the only socket,
  bound to `127.0.0.1`, token-gated and stopped with the viewer.
- Fix root causes. If a workaround is unavoidable, label it clearly and file the
  proper fix.

## Pull requests

Checklist: build succeeds, all exports present, the plugin's `./test/run.sh` is
green, leak-guard is clean, and GUI-bound behaviour was checked in a real Double
Commander. Add a `CHANGELOG.md` entry and bump the plugin's `VERSION` when
releasing.

## Reporting bugs / proposing plugins

Use the issue templates (bug, feature, new-plugin). Include your distribution,
Qt/Double Commander versions and architecture.
