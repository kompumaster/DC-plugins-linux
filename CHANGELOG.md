# Changelog

All notable changes to this collection are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); plugins are versioned
independently and tagged `<plugin>-linux-vX.Y.Z`.

## [Unreleased]

### Added
- **markdown-wlx-linux 0.1.0:** Linux/Double Commander port of the Markdown
  viewer. Shared rendering core and vendored assets; Qt 6 `QWidget` +
  `QWebEngineView` host. F3 rendering, `wlx.ini` settings, Find through
  `ListSearchTextW`, Ctrl+A/C through `ListSendCommand`, Mermaid, KaTeX,
  PageUp/PageDown scrolling, and the Esc/key hand-off that keeps DC's viewer
  shortcuts working while the page has no keyboard focus. Printing and per-file
  scroll memory are not implemented.
- **media-info-wdx-linux 0.1.0:** Linux/Double Commander port of the media-info
  content plugin. Same fields and adaptive Summary, with Qt `QImageReader` for
  image headers, FFmpeg (`libavformat`) for audio/video metadata and
  poppler-glib for PDF page counts. Includes the `CONTENT_DELAYIFSLOW` deferral
  protocol, an FP-exception guard for FPC's traps, thread-safe caching, a
  real-artifact harness and a Linux CI job.
- **image-wlx-linux 0.1.0:** Linux/Double Commander port of the image viewer.
  Qt `QGraphicsView` zoom/pan, `QImageReader` decoding with a runtime-generated
  detect string, vector-sharp SVG/SVGZ through `QSvgRenderer`, `QMovie`
  animations, checkerboard transparency, EXIF orientation, size caps, and
  Ctrl+C through `ListSendCommand` (rendered PNG/TIFF plus the original bytes).
  Keys the image does not use are handed back to DC; Quick View keeps the panel
  focus.
- **book-wlx-linux 0.1.0:** Linux/Double Commander port of the EPUB/FB2 reader.
  Same `reader.js` document model and chrome, served to QtWebEngine from a
  loopback-only HTTP server with a per-load token (custom URL schemes cannot be
  registered after QApplication starts). Includes the shared `zipreader.c`,
  contents sidebar, themes, session reading position and text size, queued Find
  across the whole book, and selection copy through the WLX ABI.

### Fixed
- **markdown-wlx-linux:** no white flash when opening a dark document. The page
  background is set to the theme colour before navigation (and the first HTML
  byte paints it), instead of WebEngine's white blank page showing until the
  stylesheet arrives.
