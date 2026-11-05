# MarkdownView (Linux) — Double Commander Markdown viewer

Linux/Double Commander port of [`../_mac_repo/markdown-wlx`](../_mac_repo/markdown-wlx). It renders
Markdown in DC's F3 viewer with GitHub-style CSS, syntax highlighting, GFM
tables and task lists, Mermaid diagrams, KaTeX math and light/dark theming.

The rendering core is shared with the macOS plugin: the same vendored
`assets/` (marked, DOMPurify, highlight.js, KaTeX, Mermaid, GitHub CSS) and the
same generated page. Only the host layer is new — Qt 6 `QWidget` +
`QWebEngineView` instead of AppKit `NSView` + `WKWebView`, because Double
Commander on Unix hands WLX plugins a `QWidget*`, not an `NSView*`.

## Requirements

- Double Commander **1.2.x, Qt 6 build** (`doublecmd-qt6`)
- Qt 6 Widgets and WebEngine development files
- `g++`, `pkg-config`

Arch: `qt6-base qt6-webengine` (dev files ship with them).
Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev`.

## Build

```sh
./build.sh
```

Produces `build/MarkdownView.wlx` — an ELF shared object (`.wlx` is just the
plugin extension; DC loads it with `dlopen`) — and mirrors `assets/` next to it.
The plugin finds its assets relative to its own path via `dladdr`.

## Install

Quit Double Commander first (it rewrites its config on exit), then:

```sh
./install.sh
```

The installer copies the plugin to
`${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd/plugins/wlx/MarkdownView/` and
registers it in `doublecmd.xml` (with a timestamped backup). Re-running it is
idempotent.

Manual alternative: Configuration → Options → Plugins → WLX → Add, point at
the installed `MarkdownView.wlx`, and use this detect string:

```
EXT="MD"|EXT="MARKDOWN"|EXT="MDOWN"|EXT="MKD"|EXT="MKDN"|EXT="MDWN"|EXT="MDTXT"|EXT="MDTEXT"|EXT="MARKDN"|EXT="RMD"|EXT="QMD"
```

## Usage

Select a `.md` file and press **F3**. If DC's viewer is still in Text mode,
press **7** once (**View → Plugins**) — Double Commander remembers the mode for
next time. Press **1** to flip back to the raw Markdown source.

## Configuration

DC passes every WLX plugin the same ini path: `wlx.ini` in DC's configuration
directory (`~/.config/doublecmd/wlx.ini`). Add a `[MarkdownView]` section:

```ini
[MarkdownView]
theme       = auto      ; auto (system) | light | dark
maxwidth    = 980       ; content column width, px
fontsize    = 16        ; base font size, px
mermaid     = 1         ; render ```mermaid blocks
math        = 1         ; render $$…$$ and \(…\)/\[…\]
mathdollar  = 0         ; also treat single $…$ as math (off by default)
showversion = 1         ; faint version badge in the bottom-right corner
```

The file is re-read on every open, so edits apply without restarting DC. See
`MarkdownView.ini.sample` for a template. Inline `$…$` math is off by default
for the same reason as on macOS: it mangles prose such as “$5 to $10”.

## Keys

The plugin keeps keyboard focus itself and hands every key it does not use to
Double Commander, so DC's viewer shortcuts keep working:

| Key | Action |
| --- | --- |
| Esc, Q | close the viewer (DC) |
| PgUp / PgDn / ↑ / ↓ / Space / Shift+Space | scroll the rendered page (plugin) |
| Home / End | top / bottom of the page (plugin) |
| F7, F3, Shift+F3 | Find, Find Next, Find Previous (DC → `ListSearchTextW`) |
| Ctrl+A / Ctrl+C | select the rendered text / copy it (DC → `ListSendCommand`) |
| Alt+→ / Alt+← | next / previous file (DC) |
| 1…9 | switch viewer mode, e.g. 1 = raw text (DC) |

Find matches the **rendered** text. The version badge is drawn as CSS
generated content, so Find and Select All cannot hit it.

## Tests

`test/load_verify.cpp` `dlopen`s the real built `.wlx` the way DC does, creates
a `QWidget` container, calls `ListLoadW`, and then drives the actual
`QWebEngineView`: rendering, highlight.js, relative images, Esc/Esc-override
focus contract, Select All, search and `ListLoadNextW`. Run it against the
artifact:

```sh
./test/run.sh
```

It needs a display. On a headless machine use `xvfb-run -a ./test/run.sh`, and
set `QTWEBENGINE_DISABLE_SANDBOX=1` in containers/CI.

GUI-bound behaviour was also verified in the real Double Commander (Qt 6,
Wayland/XWayland): F3 rendering, Esc/Q, PageUp/PageDown, F7 search with
highlight and scroll, Ctrl+A/C through the clipboard, Alt+←/→, Mermaid, KaTeX,
the `wlx.ini` theme and width settings, and teardown on close.

## Known differences from the macOS plugin

- **Printing** is not implemented yet: `ListPrintW` is not exported, so DC
  greys out File → Print. QtWebEngine has no native print dialog; a future
  implementation would go through `QWebEnginePage::printToPdf`.
- **Scroll position** is not remembered per file yet.
- The macOS plugin writes its own temp HTML page too; here it lands in the
  system temp directory and is removed when the viewer closes or the file
  changes.
- **Remote images are not fetched.** `LocalContentCanAccessRemoteUrls` is off,
  so `![](https://…)` stays blank. This matches the repo's "no network at view
  time" rule; relative local images do load.
- Requires Qt 6.4+ (this is where `QWebEngineFindTextResult` landed); developed
  and tested against Qt 6.11/6.12.

## How it works (host layer)

- `ListLoadW` creates an `MDView` (a `QWidget`) with a `QWebEngineView` and
  returns it as the WLX `HWND` (`QWidget*`); DC moves/resizes/focuses it.
- On each file, a self-contained HTML page is written to a temp file and loaded
  from `file://`, with `<base href>` set to the document's directory. That lets
  relative images work and keeps the vendored scripts/CSS on one origin.
  Markdown is base64-embedded, decoded in the page, rendered by marked.js and
  sanitised by DOMPurify.
- The page never takes keyboard focus. Unused keys are re-sent to DC's
  top-level form with `QCoreApplication::sendSpontaneousEvent` — LCL's Qt
  widgetset ignores non-spontaneous events, and a focused `QWebEngineView`
  would otherwise swallow Esc (the same trap as `WKWebView` on macOS).
- Mermaid/KaTeX are only loaded when the document contains their delimiters,
  matching the macOS plugin.

## License

Plugin source: MIT (repository [`LICENSE`](../LICENSE)).
Vendored libraries: see [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
