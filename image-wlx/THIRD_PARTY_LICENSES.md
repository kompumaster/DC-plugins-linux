# Third-party licenses — image-wlx

This port does not vendor any code or assets. It dynamically links system
libraries provided by the distribution; they are not part of this repository or
of the built `.wlx`.

| Library | Used for | License | Project |
|---------|----------|---------|---------|
| Qt 6 (Widgets, Svg, Concurrent, Gui) | widgets, view, SVG, image decoding, clipboard | LGPL-3.0 (or commercial) | https://www.qt.io/ |
| zlib | SVGZ decompression | Zlib | https://zlib.net/ |
| Qt image plugins (optional) | TIFF/WebP/HEIC/AVIF/JXL/RAW/… | per plugin (MIT/LGPL/BSD) | https://www.qt.io/ , https://invent.kde.org/frameworks/kimageformats |

Because the plugin is dynamically linked against these libraries, they remain
separate works; install them through your distribution's package manager. If you
redistribute a bundle that includes them, their licenses apply.

The plugin's own source is MIT-licensed — see the repository [LICENSE](../LICENSE).
