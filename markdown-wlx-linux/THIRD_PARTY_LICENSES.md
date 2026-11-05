# Third-party licenses — markdown-wlx-linux

The Linux port reuses the vendored libraries from `../_mac_repo/markdown-wlx/assets/`
(the files are copied here so this plugin directory stays self-contained).
Each retains its own license; all are permissive.

| Library | File(s) | License | Project |
|---------|---------|---------|---------|
| marked | `assets/marked.min.js` | MIT | https://github.com/markedjs/marked |
| DOMPurify | `assets/dompurify.min.js` | Apache-2.0 OR MPL-2.0 | https://github.com/cure53/DOMPurify |
| highlight.js | `assets/highlight.min.js`, `assets/hl-github.css`, `assets/hl-github-dark.css` | BSD-3-Clause | https://github.com/highlightjs/highlight.js |
| github-markdown-css | `assets/github-markdown.css`, `assets/github-markdown-light.css`, `assets/github-markdown-dark.css` | MIT | https://github.com/sindresorhus/github-markdown-css |
| Mermaid | `assets/mermaid/mermaid.min.js` | MIT | https://github.com/mermaid-js/mermaid |
| KaTeX | `assets/katex/*` (js, css, fonts) | MIT | https://github.com/KaTeX/KaTeX |

The plugin's own source is MIT-licensed — see the repository [LICENSE](../LICENSE).
