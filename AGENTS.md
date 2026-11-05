# DC-plugins-linux — Agent Charter

Public repo of **native Linux plugins for Double Commander**, ported from the
macOS plugins by Nikolai Sachok. Every plugin holds the same bar and the history
stays leak-clean and professional.

`AGENTS.md` is the canonical instruction file for all agents; `CLAUDE.md` is a
relative symlink to it. Edit `AGENTS.md`.

## Upstream snapshot

`_mac_repo/` is a read-only snapshot of the upstream macOS repository
(<https://github.com/NikolaiSachok/DC-plugins>, MIT). It is reference and
attribution material:

- never build from it, never edit it, never "fix" it here;
- refresh it deliberately (replace the tree) when syncing upstream;
- Linux code that derives from it keeps the upstream copyright via `LICENSE` and
  `NOTICE`.

## The SDLC every plugin follows

Per change, in order:

1. **Branch for non-trivial work** — never commit straight to `main` for a
   feature or fix. (Docs/process tweaks may go direct.)
2. **Build** — `build.sh` → one ELF `.wlx`/`.wdx` for the host architecture,
   with plain C exports (`-fvisibility=hidden`, `DCPCALL` empty on Unix).
   Verify with `file` and `nm -D --defined-only`: all ABI exports present.
3. **Test the REAL built artifact** — harnesses in `test/` that `dlopen` the
   built plugin and drive the actual ABI. GUI-bound behaviour (keys, focus,
   clipboard, gestures) must be verified in real Double Commander; a mock host
   can pass while the app fails.
4. **Deterministic pre-commit gate runs automatically** — `.githooks/pre-commit`
   (leak-guard always; build + ABI check when a plugin's native source is
   staged). Enable once per clone: `git config core.hooksPath .githooks`.
5. **Semantic review at PR / release boundaries** — independent review of the
   accumulated diff by a fresh agent (never the author).
6. **Leak hygiene** — `scripts/leak-guard.sh` must be clean. The repo is public:
   no secrets, no private absolute paths, no business/domain terms.
7. **Ship** — push over HTTPS, issues/PRs/releases via `gh`. Bump the plugin's
   `VERSION` constant (single source of truth, must match the tag), update
   `CHANGELOG.md`, then tag `<plugin>-vX.Y.Z`; the release workflow builds
   the plugin and publishes an install bundle. Tags are per plugin.

### Cadence

- Every commit → deterministic hook only (fast, blocking).
- Every PR / release → independent semantic review on the accumulated diff.
- Reviewing WIP micro-commits wastes tokens and breeds alert fatigue.

### Scope discipline

Work beyond the change in hand gets a GitHub issue (new plugin → `new-plugin`;
plugin-scoped enhancement → `enhancement`) with the rationale and options, not a
silent expansion. The roadmap is an index, not a container.

## Repo invariants

- **Public + leak-clean by construction.** Genericize anything domain-revealing.
- **One directory per plugin**, self-contained: source, `build.sh`, `install.sh`,
  `register_plugin.py`, `test/`, README, `THIRD_PARTY_LICENSES.md`, a `VERSION`
  constant and a user-visible version surface.
- **Assets vendored offline and attributed.** No network at view time.
- **Untrusted content is sanitised** before rendering (DOMPurify for EPUB/HTML;
  FB2 is mapped element-by-element onto a fixed vocabulary).
- **Plugins version independently.**

## Platform facts (don't re-learn these)

- A `.wlx`/`.wcx`/`.wdx`/`.wfx` on Linux is an **ELF shared object** exporting
  the Total Commander plugin **C ABI** (`cdecl`; `__stdcall` is empty). DC passes
  and expects `QWidget*` for viewer windows. DC prefers the wide entry points
  (`ListLoadW`, `ListLoadNextW`, `ListSearchTextW`, `ContentGetValueW`); wide
  strings are UTF-16 (`char16_t`), ANSI ones are UTF-8 on Linux.
- `ListSetDefaultParams` runs immediately after the module loads; DC passes
  `wlx.ini`/`wdx.ini` in its config directory. Per-plugin settings belong in a
  `[PluginName]` section there, with a plugin-directory ini as fallback for
  harnesses.
- **Focus:** unlike macOS, DC's `TWlxModule.SetFocus` has a working Qt branch,
  so a viewer plugin gets focus when F3 opens it. Keep focus on the plugin
  widget, not on a web view; in Quick View DC deliberately does not focus the
  plugin and the file panel must keep its keys.
- **Keys:** every key a plugin does not use belongs to DC's viewer. LCL ignores
  non-spontaneous events, so re-send a fresh `QKeyEvent` to DC's top-level form
  through the private-but-exported
  `QCoreApplication::sendSpontaneousEvent` (resolved with `dlsym`), keeping focus
  where it is. Esc/Q, N/P, Alt+←/→, F3/F7 and the view-mode digits then work.
  Scroll/zoom keys stay in the plugin and drive the page through JS where needed.
- **QWebEngine custom schemes** must be registered before `QApplication` exists;
  a plugin loads too late. `book-wlx` therefore serves the book from a
  loopback-only HTTP server bound to `127.0.0.1` with a per-load token and a
  strict CSP, and stops it when the viewer closes. Never widen that to a
  non-loopback interface.
- **QAbstractScrollArea update:** `QWidget::update()` on a `QGraphicsView` does
  not repaint its viewport. Call `viewport()->update()` (this is why animated
  images in `image-wlx` looked frozen at first).
- **FP traps:** Double Commander is a Lazarus/FPC app and enables floating-point
  exception traps. Mask exceptions (`fegetenv` / `fesetenv(FE_DFL_ENV)`) around
  library work (FFmpeg, Qt image/SVG code, transforms) and restore the host
  environment before returning.
- **Copy (web views):** DC routes Ctrl+C/Ctrl+A through `ListSendCommand`
  (`lc_copy`, `lc_selectall`), not to the page. Select the content element's
  children, never the whole document. Chromium's `triggerAction(Copy)` needs a
  focused frame, and viewer pages deliberately have none — read the selection
  and set `QClipboard` yourself (text + HTML).
- **Search:** `ListSearchTextW` + `QWebEnginePage::findText`. Chromium's find
  highlights are not a DOM selection. For incrementally built pages (a book),
  queue searches until the page reports that every chapter is in; a search must
  never hang across `ListLoadNext`.
- **Content plugins (WDX):** DC queries from worker threads. Cache values under a
  mutex keyed by path+mtime+size; return `ft_delayed` for slow FFmpeg probes
  when `CONTENT_DELAYIFSLOW` is set; guard FP traps. Use Qt `QImageReader`
  (runtime format plugins), `libavformat` and `poppler-glib` — no network.
- **Image formats** come from Qt image plugins; generate the detect string from
  `QImageReader::supportedImageFormats()` so the plugin claims only what it can
  decode.
- **CI** is Ubuntu 24.04: install the Qt/FFmpeg/Poppler dev packages, run the
  plugin's `test/run.sh`; for QtWebEngine set
  `QTWEBENGINE_DISABLE_SANDBOX=1` and `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`
  and run under `xvfb-run`. Builds are per architecture (x86_64 releases;
  arm64 builds from source).

## Per-plugin checklist (Linux)

Each plugin directory owns: `build.sh` (ELF + ABI dump), `install.sh`
(idempotent, backs up `doublecmd.xml`), `register_plugin.py` if it edits config,
`test/` (real-artifact harnesses, including the viewer key/Esc/focus checks),
`README.md` (build, install, keys, config, limitations), `THIRD_PARTY_LICENSES.md`,
a `VERSION` constant and an on-screen version surface, and `CHANGELOG.md` entries.
Add a row to the root `README.md` and extend `.github/workflows/` for a new plugin.

## Pointers

- [CONTRIBUTING.md](CONTRIBUTING.md) — build/test loop, layout convention, hook.
- [docs/ADDING-A-PLUGIN.md](docs/ADDING-A-PLUGIN.md) — scaffold a new plugin.
- Each plugin's README — behaviour, keys, dependencies, known limitations.
- `_mac_repo/docs/` — the upstream architecture notes and the original platform
  deep-dive (macOS-specific; useful for the "why").
