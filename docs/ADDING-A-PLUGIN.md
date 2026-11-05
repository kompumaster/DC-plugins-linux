# Adding a new plugin

A short, opinionated path to a new Linux plugin that holds the collection's
standard.

## 1. Pick the type and ABI

Decide WLX / WCX / WDX / WFX. Copy only the slice of the ABI you need into a
local `listplug.h`-style header (see the existing plugins). On Linux a plugin is
an ELF shared object; `cdecl` is the C calling convention and viewer window
handles are `QWidget*`.

Find the exact entry points by inspecting a working plugin or the installed
Double Commander:

```sh
nm -D --defined-only ./markdown-wlx/build/MarkdownView.wlx
```

DC prefers the wide entry points (`ListLoadW`, `ContentGetValueW`, …); wide
strings are UTF-16, ANSI ones UTF-8.

## 2. Scaffold

```
<name>-<type>/
├── <Name>.cpp            # C++/Qt (plain C for portable readers)
├── listplug.h
├── build.sh              # ELF shared object, -fvisibility=hidden, ABI dump
├── install.sh            # idempotent; backs up doublecmd.xml
├── register_plugin.py    # only if it edits the config
├── assets/               # vendored + attributed, if any
├── test/                 # real-artifact harnesses
├── README.md
└── THIRD_PARTY_LICENSES.md
```

Match an existing `build.sh`: compile with the needed `pkg-config` modules,
link with `-shared`, export only the ABI symbols, and print `nm -D` output.

## 3. Test headlessly

Write a `test/` harness that `dlopen`s the built plugin, calls the entry points
the way DC does, and asserts an observable outcome (the page rendered; a value
came back; a key routed correctly). Use the real built artifact, not a mock.

Viewer plugins built on QtWebEngine need:

- **key hand-off** — keep focus on the plugin widget, drive scroll/zoom keys
  yourself and re-send every other key to DC through
  `QCoreApplication::sendSpontaneousEvent`; verify Esc/Q, N/P, Alt+arrows and the
  view-mode digits in a real Double Commander;
- **copy** — export `ListSendCommand` for `lc_copy`/`lc_selectall` and select the
  content element's children, never the whole document.

Headless harnesses must work under `xvfb-run`; set
`QTWEBENGINE_DISABLE_SANDBOX=1` in containers.

## 4. Register & verify live

`install.sh` must be idempotent, back up `doublecmd.xml`, and place the entry so
detection order is correct. Quit DC before editing its config. Then press F3 on
a real file in a real Double Commander — the harness is a regression net, not
proof.

## 5. Wire it into the repo standard

- Add a row to the root `README.md` plugins table.
- Add a `CHANGELOG.md` entry (tag `<plugin>-vX.Y.Z`).
- Add the plugin to `.github/workflows/ci.yml` and the release workflow.
- Add an ELF/ABI block to `.githooks/pre-commit`.
- Confirm `bash scripts/leak-guard.sh` is clean.
