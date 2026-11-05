# Third-party licenses — media-info-wdx-linux

Unlike the macOS plugin, this port does not vendor any code. It dynamically
links system libraries provided by the distribution; they are not part of this
repository or of the built `.wdx`.

| Library | Used for | License | Project |
|---------|----------|---------|---------|
| Qt 6 (QtGui) | image header reading (`QImageReader`) | LGPL-3.0 (or commercial) | https://www.qt.io/ |
| FFmpeg (`libavformat`, `libavcodec`, `libavutil`) | audio/video metadata | LGPL-2.1-or-later (some builds are GPL-2.0-or-later) | https://ffmpeg.org/ |
| Poppler (`poppler-glib`) | PDF page count | GPL-2.0-or-later | https://poppler.freedesktop.org/ |

Because the plugin is dynamically linked against these libraries, they remain
separate works; install them through your distribution's package manager. If you
redistribute a bundle that includes them, their licenses apply.

The plugin's own source is MIT-licensed — see the repository [LICENSE](../LICENSE).
