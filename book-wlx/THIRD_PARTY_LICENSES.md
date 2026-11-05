# Third-party licenses — book-wlx

This port vendors one JavaScript library and dynamically links system
libraries; it does not include the upstream plugin's code beyond the shared,
MIT-licensed `zipreader.c` from the upstream [book-wlx](https://github.com/NikolaiSachok/DC-plugins/tree/main/book-wlx).

| Component | File(s) | License | Project |
|-----------|---------|---------|---------|
| DOMPurify | `assets/dompurify.min.js` | Apache-2.0 OR MPL-2.0 | https://github.com/cure53/DOMPurify |
| Qt 6 (Widgets, WebEngine) | dynamically linked | LGPL-3.0 (or commercial) | https://www.qt.io/ |
| zlib | dynamically linked | Zlib | https://zlib.net/ |

`assets/reader.js` and `assets/reader.css` are this repository's own code
(shared with the upstream [book-wlx](https://github.com/NikolaiSachok/DC-plugins/tree/main/book-wlx)). The plugin's source is MIT-licensed — see the
repository [LICENSE](../LICENSE).
