# BookView (Linux) — Double Commander WLX e-book reader

Linux/Double Commander port of
[`book-wlx`](https://github.com/NikolaiSachok/DC-plugins/tree/main/book-wlx). Reads **EPUB 2/3** and **FictionBook (FB2 / FBZ)** in DC's F3 viewer: contents sidebar, one
continuous reflowable column, cover and metadata, light/dark/sepia themes,
session reading position and text size.

The macOS plugin serves the book from its container through a private
`x-book://` scheme. QtWebEngine only accepts custom scheme handlers registered
before `QApplication` exists, and a plugin is loaded into an already-running
host, so this port serves the same bytes from a **loopback-only HTTP server**
with a per-load token:

```
http://127.0.0.1:<port>/<token>/__dcreader__/index.html    reader shell
http://127.0.0.1:<port>/<token>/__dcreader__/reader.js     reader + DOMPurify
http://127.0.0.1:<port>/<token>/OEBPS/chapter1.xhtml       an EPUB entry
```

The socket binds `127.0.0.1` only, the token is 128 random bits and changes on
every load, and the page's Content-Security-Policy allows exactly that origin
plus `data:` — so a book still cannot reach the network, and nothing is written
to disk. The server stops when the viewer closes.

## Features

- **Contents sidebar** from the EPUB 3 `nav` document, the EPUB 2 NCX, or FB2
  section titles — nested, current chapter highlighted as you scroll.
- **FictionBook structures**: epigraphs with attribution, poems in stanzas,
  cites, tables, footnotes that jump to the notes and back.
- **Reading position** remembered per book for the session, plus a progress
  readout.
- **Text size** with `A-` / `A+` or `-` / `+`; the choice sticks for the session.
- **Cover** shown when the book doesn't already open on one.
- **Nothing fetched from the internet**: EPUB chapters pass through DOMPurify;
  FB2 is rebuilt element by element (never parsed as markup); remote `http(s)`
  references are dropped; CSP forbids network connections.
- **Search** across the whole book (DC's Find / F7), including the last chapter
  even when the book is still arriving.
- Format decided from **content**: a ZIP with `META-INF/container.xml` is an
  EPUB, a ZIP with a `.fb2` entry is a zipped FictionBook, bare XML naming
  FictionBook is an FB2.

## Requirements

- Double Commander **1.2.x, Qt 6 build** (`doublecmd-qt6`)
- Qt 6 Widgets + WebEngine development files, `zlib`
- Python 3 only for the test sample generator

Arch: `qt6-base qt6-webengine`.
Debian/Ubuntu: `qt6-base-dev qt6-webengine-dev libqt6webenginewidgets6`.

## Build

```sh
./build.sh
```

Produces `build/BookView.wlx` (ELF shared object) plus `build/assets/` with
`reader.js`, `reader.css` and DOMPurify. The plugin loads those from `assets/`
next to the `.wlx`.

## Install

Quit Double Commander first, then:

```sh
./install.sh
```

Copies the plugin and its assets to
`${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd/plugins/wlx/BookView/` and
registers it in `doublecmd.xml` (timestamped backup, idempotent).

## Usage

| Action | How |
|---|---|
| Open a book | **F3** on `.epub`, `.fb2`, `.fbz` |
| Show/hide contents | the ☰ button, or **t** |
| Jump to a chapter | click it in the contents |
| Follow a footnote | click the marker |
| Larger / smaller text | `A+` / `A-`, or **+** / **-** |
| Scroll | wheel, arrows, PgUp/PgDn, Home/End, Space |
| Search the book | **F7**, then **F3** (next), **Shift+F3** (previous) |
| Copy the selection | Ctrl+A / Ctrl+C (routed through the WLX ABI) |
| Close the viewer | **Esc** / **Q** |
| Read raw text instead | the viewer's own mode switch |

Every key the reader does not use (Esc/Q, N/P, Alt+arrows, the view-mode
digits) is handed to Double Commander. The page itself never takes keyboard
focus; scroll keys and `t` / `+` / `-` are driven from the plugin, and text
selection still works with the mouse. Double Commander calls `SetFocus` for Qt
plugins, so the book responds to keys as soon as F3 opens it, while Quick View
keeps the file panel's focus.

## Configuration

Add a `[BookView]` section to DC's `wlx.ini`
(`~/.config/doublecmd/wlx.ini`) — see `BookView.ini.sample` for every key
(`theme`, `fontsize`, `maxwidth`, `lineheight`, `justify`, `toc`,
`publishercss`, `showversion`).

## Supported books

- EPUB 2 and EPUB 3 (OCF/ZIP), including legacy encodings declared by chapters
- FictionBook 2.0, including `windows-1251` files
- `.fbz`: a zipped FictionBook

`foo.fb2.zip` is not claimed — DC sees `ZIP` — rename it to `.fbz`.

## How it works

- `zipreader.c` (shared with the macOS plugin) reads the central directory and
  inflates stored/deflated entries with zlib (ZIP64, CRC-32, size caps).
- The native side detects the format from content and starts the loopback
  server. `reader.js` — the same document model as macOS — fetches
  `container.xml`, the OPF, spine documents, nav/NCX and FB2 `document.fb2`
  from that origin, so relative hrefs, images and fonts work unchanged.
- Chrome (title bar, contents, progress, version) draws its labels as CSS
  generated content, so Find and Select All never hit them.
- Searches are queued until the reader posts `ready` (every chapter is in) and
  then run through `QWebEnginePage::findText`; loading another book drops the
  queue. A timeout releases the queue if a page never reports ready.
- Copy reads the selection out of the page and sets the clipboard itself (as
  plain text and HTML), because Chromium's Copy action needs a focused frame
  and the page deliberately has none.
- Reading position and text size arrive as small POSTs to the same server and
  are kept in memory for the session.

## Tests

```sh
./test/run.sh
```

Builds the plugin, generates the sample books with `test/make_samples.py`,
runs the headless ZIP harness (`test/zip_test.c`), then drives the real built
`.wlx` against the live DOM: all four books, TOC, EPUB 2/3 and windows-1251
text, FB2 epigraphs/tables, sanitising (script/iframe/remote images), required
images, `t` / `+` / PgDn, Esc and digit hand-off, queued search before ready,
copy, `ListLoadNextW`, and the malformed-book error page. No display is needed
(`xvfb-run` is used when headless).

Verified in the real Double Commander (Qt 6): EPUB 3 and EPUB 2, windows-1251
chapter, FB2 and FBZ text, TOC sidebar via `t`, larger text via `+`, F7 search
jumping to the last chapter with a highlight, Ctrl+A/C copying the prose (not
the sidebar), Esc closing, and the error page for a non-book file.

## Differences from the macOS plugin

- **Transport**: loopback HTTP with a token instead of `x-book://`. Cookies,
  cache and the port stay inside this process; the server is stopped on close.
  A local process that learned the token could read the book's bytes — the same
  threat model as reading the file itself.
- Copy goes through the Qt clipboard (text + HTML) rather than WebKit's
  pasteboard copy; the selection is the same.
- No printing (the macOS plugin has none either).
- Keep `assets/reader.js` in sync with the upstream
  [book-wlx reader.js](https://github.com/NikolaiSachok/DC-plugins/blob/main/book-wlx/assets/reader.js);
  the Linux copy differs only in `post()` (a POST to the server instead of a WKWebView
  script message), the readiness global and the origin-aware sanitiser regex.

## License

Plugin source: MIT (repository [`LICENSE`](../LICENSE)). Vendored DOMPurify and
the dynamically linked Qt/zlib libraries are covered in
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
