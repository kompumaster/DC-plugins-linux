# MediaInfo (Linux) — Double Commander WDX content plugin

Linux/Double Commander port of [`../_mac_repo/media-info-wdx`](../_mac_repo/media-info-wdx). A
**content (WDX) plugin** that surfaces per-file media metadata as fields you can
show in Double Commander's **custom columns** and **tooltips** — so the
resolution of an image, the length of a clip, or a PDF's page count is visible
**right in the file listing**, without opening a preview or a second app.

The macOS plugin uses ImageIO, AVFoundation and CGPDF; Linux has no exact
equivalents, so this port uses the native system libraries:

| File type | Fields | Backend |
|-----------|--------|---------|
| **Images** (jpg, png, gif, tiff, bmp, webp, heic, raw, …) | Dimensions, Width, Height, Megapixels, DPI, Bit depth | **Qt `QImageReader`** (header only — never decodes pixels) |
| **Audio** (mp3, m4a, aac, wav, flac, ogg, opus, …) | Duration, Bitrate, Sample rate, Channels, Audio codec | **libavformat** (FFmpeg) |
| **Video** (mp4, mov, mkv, webm, avi, wmv, flv, ts, …) | Dimensions, Duration, Frame rate, Bitrate, Video/Audio codec | **libavformat** (FFmpeg) |
| **PDF** | Page count | **poppler-glib** |

No network access, ever.

## The `Summary` field

The headline field is **`Summary`** — the plugin picks the single most useful
string per file type, so **one column** covers every kind of file (and is blank
for files it doesn't apply to):

| File | `Summary` shows |
|------|-----------------|
| image | `3024 × 4032` |
| video | `1920 × 1080 · 12:30` |
| audio | `3:45` |
| pdf | `14 pages` |
| anything else | *(blank)* |

## Requirements

- Double Commander **1.2.x, Qt 6 build** (`doublecmd-qt6`)
- Qt 6 Gui development files
- FFmpeg development libraries (`libavformat`, `libavcodec`, `libavutil`)
- Poppler GLib development files
- `g++`, `pkg-config`

Arch: `qt6-base ffmpeg poppler-glib` (dev files ship with them).
Debian/Ubuntu: `qt6-base-dev libavformat-dev libavcodec-dev libavutil-dev
libpoppler-glib-dev`.

Image formats beyond Qt's built-in set (TIFF, WebP, HEIC/AVIF, JPEG XL, camera
RAW) need the matching Qt image plugin (`qt6-image-formats-plugins` /
`kimageformats`). Without it those files simply report no dimensions; nothing
crashes.

## Build

```sh
./build.sh
```

Produces `build/MediaInfo.wdx` — an ELF shared object (`.wdx` is just the
plugin extension; DC loads it with `dlopen`).

## Install

Quit Double Commander first (it rewrites its config on exit), then:

```sh
./install.sh
```

Copies the plugin to
`${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd/plugins/wdx/MediaInfo/` and
registers it in `doublecmd.xml` (timestamped backup, idempotent).

### Add a column

In a file panel: **right-click the column header → Configure / Columns…**, add a
column, and set its template to a plugin field. Double Commander's template
syntax is:

```
[Plugin(MediaInfo).Summary{}]
```

The editor's plugin-field menu inserts exactly that string; `Dimensions`,
`Duration`, `Page count`, … work the same way. You can mix fields with literal
text, e.g. `[Plugin(MediaInfo).Dimensions{}] px`. The same fields are available
in **tooltips** (Options → Tooltips → Fields).

## Fields

`Summary`, `Dimensions`, `Width`, `Height`, `Megapixels`, `DPI`, `Bit depth`,
`Duration`, `Duration (s)`, `Frame rate`, `Bitrate`, `Sample rate`, `Channels`,
`Video codec`, `Audio codec`, `Page count`, `Plugin version`.

`Duration (s)` is numeric (sortable); `Duration` is the formatted `m:ss` /
`h:mm:ss` string. **`Plugin version`** returns the build's version — add it as a
column to see which build is loaded (it is also embedded in the binary:
`strings MediaInfo.wdx | grep MediaInfo`).

## Supported extensions

```
images: jpg jpeg png gif tiff tif bmp webp heic heif avif ico icns psd jp2
        jxl dng cr2 cr3 nef arw orf rw2 raf sr2 pef
audio:  mp3 m4a m4b aac wav aiff aif aifc caf flac ogg oga opus wma mka ape wv
video:  mp4 mov m4v 3gp 3g2 avi mkv webm mpg mpeg ts m2ts mts vob wmv flv f4v ogv
pdf:    pdf
```

## How it works

- A `.wdx` is an ELF shared object exporting the Total Commander **Content
  plugin** C ABI (`ContentGetSupportedField`, `ContentGetValue(W)`,
  `ContentGetDetectString`, `ContentSetDefaultParams`, `ContentPluginUnloading`).
  DC asks which fields the plugin provides, then calls `ContentGetValue(W)` per
  file/field.
- A field returns `ft_fieldempty` for files it doesn't apply to, so one column
  serves every type without waste.
- Values are cached per `path + mtime + size` (256 entries) under a mutex,
  because DC queries columns from worker threads.
- **Non-blocking:** FFmpeg probing is the only slow path. When DC requests a
  value in the foreground with `CONTENT_DELAYIFSLOW`, the plugin returns
  `ft_delayed` and lets DC re-query it on a background thread — the panel never
  stalls while scrolling. Images, PDFs and cached values answer immediately.
- **Crash-safe under DC's runtime:** Double Commander is a Lazarus/FPC app that
  *enables* floating-point exception traps. Library code that divides by zero
  under the default masked environment then raises a fatal trap, which DC
  surfaces as an "Access violation". The plugin masks FP exceptions around every
  parse and restores the host's environment before returning.
- **AVI/MKV/WebM** are read by FFmpeg on Linux (the macOS plugin had to carry its
  own RIFF/EBML readers because AVFoundation can't open them). Display aspect
  ratio and the MP4/Matroska display matrix rotation are applied to the reported
  dimensions.
- Image DPI and bit depth are read from the headers: PNG (`pHYs`, `IHDR`), JPEG
  (`JFIF` density, `SOFn` precision) and TIFF (`XResolution`, `BitsPerSample`).
  Other image formats report dimensions/megapixels only.

## Tests

`test/test_host.cpp` `dlopen`s the real built `.wdx` and drives its C ABI the
way DC does. It synthesizes a 320×200 PNG (300 dpi), a JPEG, a 2-page PDF and a
1 s 8 kHz mono WAV, uses `ffmpeg` to generate AVI/MKV/MP4 clips, and checks
values, the `ContentGetValueW` UTF-16 path, the `CONTENT_DELAYIFSLOW` protocol,
FP-exception survival and concurrent queries:

```sh
./test/run.sh
```

No display is needed. The same column behaviour was verified in the real Double
Commander (Qt 6): a `Media` column rendered `320 × 240 · 0:02`, `2 pages`,
`0:01`, `640 × 480` for generated samples.

## Differences from the macOS plugin

- Backends are Qt/FFmpeg/Poppler instead of ImageIO/AVFoundation/CGPDF.
- More audio/video containers are recognized (flac, ogg/opus, wma, wmv, flv,
  mpeg, ts, vob, …), because FFmpeg reads them all.
- DPI/bit depth are header-parsed only for PNG/JPEG/TIFF.
- Image formats outside Qt's base plugins depend on the installed Qt image
  plugins (see Requirements).

## License

Plugin source: MIT (repository [`LICENSE`](../LICENSE)).
Backends are dynamically linked system libraries; see
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
