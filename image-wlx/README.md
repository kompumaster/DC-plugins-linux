# ImageView (Linux) — Double Commander WLX image viewer

Linux/Double Commander port of
[`image-wlx`](https://github.com/NikolaiSachok/DC-plugins/tree/main/image-wlx).
Views images in the F3 viewer and Quick View with **zoom and pan that work**:
wheel/keyboard/toolbar zoom, drag panning, and SVG that stays **vector-sharp at
any magnification**. It shows pixel size, format, frame count and file size in
its toolbar.

The upstream macOS plugin is AppKit + ImageIO + CoreSVG. Double Commander on
Unix hands a WLX plugin a `QWidget*` and expects a `QWidget*` back, so this port
is C++/Qt:

| Concern | macOS | Linux |
|---|---|---|
| View | `NSScrollView` magnification | `QGraphicsView` transform |
| Raster | ImageIO | `QImageReader` + Qt image plugins |
| SVG | CoreSVG | `QSvgRenderer` (re-rasterized through the painter transform) |
| Animation | ImageIO frames + display link | `QMovie` |
| Clipboard | NSPasteboard | `QClipboard` + `QMimeData` |
| Toolbar | AppKit views | Qt widgets |

- **Zoom** — Ctrl+wheel (around the pointer), Ctrl+= / Ctrl+-, `+` / `-` and
  keypad `+` / `-`, Ctrl+0 actual size, Ctrl+9 fit, the zoom menu (25%…1600%),
  double-click toggles Fit ↔ 100% at the pointer. Up to 6400% (more for a tiny
  icon), down to half of fit.
- **Pan** — drag with the hand cursor, scroll, or the arrows / PgUp / PgDn /
  Home / End / Space (Shift+Space pages up, Ctrl+↑/↓ jump to top/bottom).
- **SVG and SVGZ** redrawn as vectors at every zoom level — no external
  fetches, no scripts.
- **Pixel-exact zoom** — past 200% raster pixels are drawn as crisp squares.
- **Transparency** over a checkerboard; EXIF orientation applied; the largest
  entry of an `.ico` / `.icns` / `.cur` is shown.
- **Animation** (GIF, WebP, AVIF, JXL, ANI) plays with the file's own timing and
  loop count. APNG is the one format from the macOS plugin that Qt does not
  animate; it shows its first frame.
- **Ctrl+C** copies the image as PNG (plus TIFF when small enough) and keeps
  the file's own bytes under its own type — the SVG source, an animated GIF,
  JPEG/WebP/AVIF originals.
- **Light and dark** follow the Qt palette.
- Files that Qt cannot decode are declined (`ListLoad` returns `NULL`), so
  Double Commander falls through to its own Graphics mode or the next plugin.

## Requirements

- Double Commander **1.2.x, Qt 6 build** (`doublecmd-qt6`)
- Qt 6 Widgets, Svg and Concurrent development files, `zlib`
- Qt image format plugins for the extra formats you want (TIFF/WebP/HEIC/AVIF/
  JXL/RAW/…). Without them the plugin simply does not claim those extensions.

Arch: `qt6-base qt6-svg qt6-imageformats` (dev files ship with them).
Debian/Ubuntu: `qt6-base-dev qt6-svg-dev qt6-image-formats-plugins`, plus
`kimageformat-plugins` for HEIC/AVIF/JXL/RAW.

## Build

```sh
./build.sh
```

Produces `build/ImageView.wlx` — an ELF shared object (`.wlx` is just the plugin
extension; DC loads it with `dlopen`).

## Install

Quit Double Commander first (it rewrites its config on exit), then:

```sh
./install.sh
```

Copies the plugin to
`${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd/plugins/wlx/ImageView/` and
registers it in `doublecmd.xml` (timestamped backup, idempotent). The plugin's
detect string is read from the built `.wlx` itself (`ListGetDetectString`), so
the list of claimed formats has one source of truth: the formats this runtime
can actually decode.

### Manual registration (alternative)

*Configuration → Options → Plugins → Plugins WLX → Add*, pick `ImageView.wlx`,
and move it above any catch-all lister.

## Usage

Press **F3** on an image, or turn on Quick View (**Ctrl+Q**).

| Action | Keys / gesture |
|---|---|
| Zoom in / out | Ctrl+= / Ctrl+-, `+` / `-`, keypad `+` / `-`, Ctrl+wheel, pinch |
| Actual size | Ctrl+0 |
| Fit to window | Ctrl+9 |
| Toggle Fit ↔ 100% | double-click |
| Pan | drag, scroll, arrows, PgUp / PgDn, Space / Shift+Space |
| Top / bottom | Home / End, Ctrl+↑ / Ctrl+↓ |
| Copy image | Ctrl+C |
| Next / previous file | N / P, Alt+→ / Alt+← (F3 viewer) |
| Close viewer | Esc, Q |

Every key the image does not use is handed to Double Commander, so the viewer's
own shortcuts keep working. Double Commander calls `SetFocus` for Qt plugins, so
the image has focus as soon as F3 opens it — no macOS-style focus hacks. In
Quick View DC deliberately does not focus the plugin, and the file panel keeps
its cursor keys.

The zoom an image **opens** at follows DC's own *Image → Stretch* settings
(keys **F** and **L** in the viewer): with *Stretch only large* (the default) a
large image fits and a small one opens at 100%; with *Stretch* everything fits;
with neither, everything opens at 100%. Changing the setting re-fits the open
image at once (`lc_newparams`).

## Supported formats

The detect string is generated at runtime from `QImageReader::supportedImageFormats()`,
so the plugin claims exactly what the installed Qt image plugins can decode.
On a typical KDE/Qt system that is ~90 formats: PNG, JPEG, GIF, BMP, TIFF, WebP,
ICO/CUR/ICNS, PSD/PSB, TGA, EXR, HDR, DDS, JPEG 2000, JXL, AVIF, HEIC (with
libheif), camera RAW, PBM/PGM/PPM/PFM, XCF, QOI, SVG and SVGZ, among others.

Not supported by Qt on Linux (the file falls through to DC's Graphics mode):
APNG animation, DICOM, PICT, MPO, ASTC/KTX/PVR textures, animated HEICS.

## Tests

`test/test_host.cpp` `dlopen`s the real built `.wlx`, creates a `QWidget`
container like DC does, calls `ListLoadW`, and then drives the actual
`QGraphicsView`: formats (PNG/JPEG/BMP/TIFF/ICO/GIF/SVG/SVGZ/WebP), fall-through
for non-images, opening zoom from `ShowFlags`, `lc_newparams`, zoom keys and
limits, pixelated rendering past 200%, scroll keys, animation frames,
double-click toggle, clipboard (image + SVG/GIF sources), resize refitting,
`ListLoadNextW`, size caps for huge rasters, and the FP-trap guard.

```sh
./test/run.sh        # uses the current display, or xvfb-run when headless
```

The same behaviour was verified in the real Double Commander (Qt 6): F3 view
with toolbar, Ctrl+/- and Ctrl+wheel zoom, drag panning, checkerboard
transparency, animated GIF, SVG at 400% staying sharp, Ctrl+C into the
clipboard, Esc/Q closing, and Quick View keeping the panel's cursor keys.

## Differences from the upstream macOS plugin

- Backends are Qt/`QImageReader`/`QSvgRenderer`/`QMovie` instead of
  ImageIO/CoreSVG.
- The claimed format list is discovered at runtime; HEIC/AVIF/JXL/RAW depend on
  the installed Qt image plugins (HEIC needs `libheif`).
- APNG is not animated by Qt (first frame shown); DICOM/PICT/MPO and texture
  containers are not supported.
- Pinch is delivered as `QNativeGestureEvent` where the platform provides it;
  Ctrl+wheel is the reliable path everywhere.
- Zoom shortcuts use Ctrl instead of Cmd.

## License

Plugin source: MIT (repository [`LICENSE`](../LICENSE)). Qt and zlib are
dynamically linked system libraries; see
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
