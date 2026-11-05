/*
 * MarkdownView.wlx — Double Commander Lister (WLX) plugin for Linux.
 *
 * Linux port of ../_mac_repo/markdown-wlx. The rendering core is the same: the Markdown
 * source is base64-embedded in a generated page and rendered client-side by the
 * vendored marked.js / DOMPurify / highlight.js, with Mermaid and KaTeX added
 * only when the document needs them. The vendored assets are shared with the
 * macOS plugin (copied, so this directory stays self-contained).
 *
 * What differs is the host layer. On macOS Double Commander passes/expects
 * NSView* and the view is a WKWebView. On Unix it passes a QWidget* (the
 * viewer's container) and expects a QWidget* back; DC then moves, resizes and
 * focuses it (see src/uwlxmodule.pas in doublecmd). So this file is C++/Qt:
 * a QWidget owning a QWebEngineView.
 */

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineFindTextResult>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QWidget>

#include <dlfcn.h>

#include <cstring>

#include "listplug.h"

#define MDV_VERSION "0.1.0" /* single source of truth for the Linux port */

namespace {

/* Directory that contains this .wlx, so the assets folder beside it is found. */
QString pluginDirectory() {
    Dl_info info;
    if (dladdr(reinterpret_cast<const void *>(&pluginDirectory), &info) && info.dli_fname) {
        return QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath();
    }
    return QString();
}

QString assetDirectory() {
    const QString dir = pluginDirectory();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/assets");
}

QString assetUrl(const QString &name) {
    return QUrl::fromLocalFile(assetDirectory() + QLatin1Char('/') + name).toString();
}

/* Set by ListSetDefaultParams (DC's per-plugin ini path). */
QString gIniPath;

QString configIniPath() {
    if (!gIniPath.isEmpty()) return gIniPath;
    const QString dir = pluginDirectory();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/MarkdownView.ini");
}

bool cfgBool(const QSettings &cfg, const QString &key, bool defaultValue) {
    const QString v = cfg.value(QStringLiteral("MarkdownView/") + key,
                                defaultValue ? QStringLiteral("1") : QStringLiteral("0"))
                          .toString().toLower();
    return v == QLatin1String("1") || v == QLatin1String("true") ||
           v == QLatin1String("yes") || v == QLatin1String("on");
}

QString htmlEscape(QString s) {
    s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return s;
}

/* Content element children only — never the whole document, so the version
 * badge does not land in the clipboard. */
const char *const kSelectContentJS =
    "(function(){var el=document.getElementById('content');if(!el)return false;"
    "var s=window.getSelection();if(!s)return false;"
    "s.removeAllRanges();s.selectAllChildren(el);return true;})()";

} // namespace

static bool systemPrefersDark(const QWidget *w) {
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) return true;
    if (scheme == Qt::ColorScheme::Light) return false;
    return w->palette().color(QPalette::Window).lightness() < 128;
}

class MDView : public QWidget {
public:
    explicit MDView(QWidget *parent);
    ~MDView() override;

    bool loadMarkdown(const QString &path);
    void keyPressEvent(QKeyEvent *event) override;
    void findText(const QString &text, int flags);
    int sendCommand(int command);

private:
    void removeTempFile();

    QWebEngineView *web_ = nullptr;
    QString tempPath_;
    quint64 counter_ = 0;
    bool closed_ = false;
};

MDView::MDView(QWidget *parent) : QWidget(parent) {
    web_ = new QWebEngineView(this);
    web_->setContextMenuPolicy(Qt::DefaultContextMenu);
    web_->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    web_->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    web_->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(web_);

    /* Keep keyboard focus on this widget, never on the web page.
     *
     * A focused QWebEngineView accepts Escape itself (it uses it to leave HTML
     * fullscreen) and LCL never sees it, so F3's viewer cannot be closed — the
     * same failure WKWebView causes on macOS. With focus here instead,
     * keyPressEvent sees every key and hands the unused ones back to DC. */
    setFocusPolicy(Qt::StrongFocus);
    web_->setFocusPolicy(Qt::NoFocus);
    setFocus();
}

void MDView::keyPressEvent(QKeyEvent *event) {
    /* Scrolling is ours; every other key belongs to DC.
     *
     * The page has no keyboard focus (see the constructor), so DC's hotkeys —
     * Esc/Q to close the viewer, N/P, F3/F7 search, Ctrl+A/C through
     * ListSendCommand, the view-mode digits — would otherwise never fire while
     * the plugin is open. LCL's Qt widgetset ignores non-spontaneous events, so
     * re-send a fresh copy through the private (but exported)
     * QCoreApplication::sendSpontaneousEvent and keep focus where it is. */
    int dy = 0;
    switch (event->key()) {
    case Qt::Key_Down: dy = 40; break;
    case Qt::Key_Up: dy = -40; break;
    case Qt::Key_PageDown: dy = qMax(80, web_->height() * 9 / 10); break;
    case Qt::Key_PageUp: dy = -qMax(80, web_->height() * 9 / 10); break;
    case Qt::Key_Space:
        dy = (event->modifiers() & Qt::ShiftModifier) ? -qMax(80, web_->height() * 9 / 10)
                                                      : qMax(80, web_->height() * 9 / 10);
        break;
    default: break;
    }
    if (dy != 0) {
        web_->page()->runJavaScript(QStringLiteral("window.scrollBy(0,%1)").arg(dy));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Home || event->key() == Qt::Key_End) {
        web_->page()->runJavaScript(event->key() == Qt::Key_Home
                                        ? QStringLiteral("window.scrollTo(0,0)")
                                        : QStringLiteral("window.scrollTo(0,document.body.scrollHeight)"));
        event->accept();
        return;
    }

    QWidget *target = window();
    if (target) {
        QKeyEvent forwarded(QEvent::KeyPress, event->key(), event->modifiers(), event->text(),
                            event->isAutoRepeat(), event->count());
        using SendSpontaneous = bool (*)(QObject *, QEvent *);
        static SendSpontaneous sendSpontaneous = reinterpret_cast<SendSpontaneous>(
            dlsym(RTLD_DEFAULT, "_ZN16QCoreApplication20sendSpontaneousEventEP7QObjectP6QEvent"));
        if (sendSpontaneous) sendSpontaneous(target, &forwarded);
        else QCoreApplication::sendEvent(target, &forwarded);
        if (forwarded.isAccepted()) {
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event); /* ignore -> Qt's normal propagation */
}

MDView::~MDView() {
    closed_ = true;
    removeTempFile();
}

void MDView::removeTempFile() {
    if (!tempPath_.isEmpty()) {
        QFile::remove(tempPath_);
        tempPath_.clear();
    }
}

bool MDView::loadMarkdown(const QString &path) {
    if (closed_ || !web_) return false;

    QFile file(path);
    QByteArray raw;
    if (file.open(QIODevice::ReadOnly)) raw = file.readAll();
    const QString mdText = QString::fromUtf8(raw);

    const QSettings cfg(configIniPath(), QSettings::IniFormat);
    QString theme = cfg.value(QStringLiteral("MarkdownView/theme"), QStringLiteral("auto"))
                        .toString().toLower();
    if (theme != QLatin1String("light") && theme != QLatin1String("dark"))
        theme = QStringLiteral("auto");

    /* The page is blank between navigation and its first paint; WebEngine's
     * default blank page is white, which shows as a flash before a dark theme
     * arrives. Set the page/widget background to the colour the document will
     * use, before load(), and paint the same colour from the first HTML byte. */
    const bool darkPage = theme == QLatin1String("dark") ||
                          (theme == QLatin1String("auto") && systemPrefersDark(this));
    const QColor pageBg = darkPage ? QColor(0x0d, 0x11, 0x17) : QColor(Qt::white);
    web_->page()->setBackgroundColor(pageBg);
    setAutoFillBackground(true);
    QPalette hostPalette = palette();
    hostPalette.setColor(QPalette::Window, pageBg);
    setPalette(hostPalette);
    web_->setAutoFillBackground(true);
    QPalette webPalette = web_->palette();
    webPalette.setColor(QPalette::Base, pageBg);
    webPalette.setColor(QPalette::Window, pageBg);
    web_->setPalette(webPalette);
    web_->setStyleSheet(QStringLiteral("background-color:%1;").arg(pageBg.name()));
    const int maxWidth = qMax(320, cfg.value(QStringLiteral("MarkdownView/maxwidth"), 980).toInt());
    const int fontSize = qMax(8, cfg.value(QStringLiteral("MarkdownView/fontsize"), 16).toInt());
    const bool wantMermaid =
        cfgBool(cfg, QStringLiteral("mermaid"), true) &&
        mdText.contains(QLatin1String("```mermaid"));
    const bool allowDollar = cfgBool(cfg, QStringLiteral("mathdollar"), false);
    const bool hasMathDelim =
        mdText.contains(QLatin1String("$$")) || mdText.contains(QLatin1String("\\(")) ||
        mdText.contains(QLatin1String("\\[")) || (allowDollar && mdText.contains(QLatin1Char('$')));
    const bool wantMath = cfgBool(cfg, QStringLiteral("math"), true) && hasMathDelim;
    const bool showVersion = cfgBool(cfg, QStringLiteral("showversion"), true);

    const QString mdDir = QFileInfo(path).absolutePath();
    const QString baseHref = QUrl::fromLocalFile(mdDir + QLatin1Char('/')).toString();
    const QString title = htmlEscape(QFileInfo(path).fileName());
    const QString b64 = QString::fromLatin1(raw.toBase64());

    QString head = QStringLiteral("<style>html,body{background:%1;}</style>").arg(pageBg.name());
    if (theme == QLatin1String("light")) {
        head += QStringLiteral("<link rel=\"stylesheet\" href=\"%1\">").arg(assetUrl(QStringLiteral("github-markdown-light.css")));
        head += QStringLiteral("<link rel=\"stylesheet\" href=\"%1\">").arg(assetUrl(QStringLiteral("hl-github.css")));
        head += QStringLiteral("<style>:root{color-scheme:light;}html,body{background:#fff;}</style>");
    } else if (theme == QLatin1String("dark")) {
        head += QStringLiteral("<link rel=\"stylesheet\" href=\"%1\">").arg(assetUrl(QStringLiteral("github-markdown-dark.css")));
        head += QStringLiteral("<link rel=\"stylesheet\" href=\"%1\">").arg(assetUrl(QStringLiteral("hl-github-dark.css")));
        head += QStringLiteral("<style>:root{color-scheme:dark;}html,body{background:#0d1117;}</style>");
    } else {
        head += QStringLiteral("<link rel=\"stylesheet\" href=\"%1\">").arg(assetUrl(QStringLiteral("github-markdown.css")));
        head += QStringLiteral("<link rel=\"stylesheet\" href=\"%1\" media=\"(prefers-color-scheme: light)\">").arg(assetUrl(QStringLiteral("hl-github.css")));
        head += QStringLiteral("<link rel=\"stylesheet\" href=\"%1\" media=\"(prefers-color-scheme: dark)\">").arg(assetUrl(QStringLiteral("hl-github-dark.css")));
        head += QStringLiteral("<style>:root{color-scheme:light dark;}html,body{background:#fff;}"
                               "@media(prefers-color-scheme:dark){html,body{background:#0d1117;}}</style>");
    }
    head += QStringLiteral("<style>html,body{margin:0;padding:0;}"
                           ".markdown-body{box-sizing:border-box;max-width:%1px;margin:0 auto;"
                           "padding:32px 44px;font-size:%2px;}"
                           "@media(max-width:767px){.markdown-body{padding:18px;}}"
                           ".mermaid{display:flex;justify-content:center;margin:16px 0;}</style>")
                .arg(maxWidth)
                .arg(fontSize);
    head += QStringLiteral(
        "<style>#mdv-ver{position:fixed;right:8px;bottom:6px;z-index:99;"
        "font:11px/1.4 sans-serif;color:#8b949e;background:rgba(127,127,127,.12);"
        "padding:2px 8px;border-radius:10px;opacity:.35;user-select:none;"
        "transition:opacity .15s;}"
        "#mdv-ver::after{content:attr(data-label);}"
        "#mdv-ver:hover{opacity:.95;}</style>");
    head += QStringLiteral("<link rel=\"stylesheet\" media=\"print\" href=\"%1\">").arg(assetUrl(QStringLiteral("github-markdown-light.css")));
    head += QStringLiteral("<link rel=\"stylesheet\" media=\"print\" href=\"%1\">").arg(assetUrl(QStringLiteral("hl-github.css")));
    head += QStringLiteral("<link rel=\"stylesheet\" media=\"print\" href=\"%1\">").arg(assetUrl(QStringLiteral("print.css")));

    head += QStringLiteral("<script src=\"%1\"></script>").arg(assetUrl(QStringLiteral("marked.min.js")));
    head += QStringLiteral("<script src=\"%1\"></script>").arg(assetUrl(QStringLiteral("dompurify.min.js")));
    head += QStringLiteral("<script src=\"%1\"></script>").arg(assetUrl(QStringLiteral("highlight.min.js")));
    if (wantMermaid)
        head += QStringLiteral("<script src=\"%1\"></script>").arg(assetUrl(QStringLiteral("mermaid/mermaid.min.js")));
    if (wantMath) {
        head += QStringLiteral("<link rel=\"stylesheet\" href=\"%1\">").arg(assetUrl(QStringLiteral("katex/katex.min.css")));
        head += QStringLiteral("<script src=\"%1\"></script>").arg(assetUrl(QStringLiteral("katex/katex.min.js")));
        head += QStringLiteral("<script src=\"%1\"></script>").arg(assetUrl(QStringLiteral("mathext.js")));
    }

    /* Same bootstrap as the macOS plugin. It has to run after the vendored
     * libraries (they are classic <script> tags above) and before the first
     * paint the harness observes. window.__mdvReady is the readiness probe the
     * test harness polls; it is also where a future scroll-restore hook goes. */
    const QString bootstrap = QStringLiteral(
        "var __theme=\"%1\";var __mathDollar=%2;var __wantMath=%3;"
        "window.__mdvReady=false;"
        "(async function(){"
        "var b64=document.getElementById('md-data').textContent.trim();"
        "var md=new TextDecoder('utf-8').decode(Uint8Array.from(atob(b64),function(c){return c.charCodeAt(0);}));"
        "try{marked.setOptions({gfm:true,breaks:false});}catch(e){}"
        "if(__wantMath&&window.__dcMathSetup)try{__dcMathSetup(marked,{dollar:!!__mathDollar});}catch(e){}"
        "var out;try{out=marked.parse(md);}catch(e){out='<pre>'+String(e)+'</pre>';}"
        "if(window.DOMPurify){out=DOMPurify.sanitize(out,{USE_PROFILES:{html:true}});}"
        "var content=document.getElementById('content');content.innerHTML=out;"
        "var diagramReady=Promise.resolve();if(window.mermaid){"
        "content.querySelectorAll('code.language-mermaid').forEach(function(code){"
        "var d=document.createElement('div');d.className='mermaid';d.textContent=code.textContent;"
        "var pre=code.parentNode;pre.parentNode.replaceChild(d,pre);});"
        "try{var dark=(__theme==='dark')||(__theme==='auto'&&matchMedia('(prefers-color-scheme: dark)').matches);"
        "mermaid.initialize({startOnLoad:false,theme:dark?'dark':'default',securityLevel:'strict'});"
        "diagramReady=mermaid.run().catch(function(){});}catch(e){}}"
        "try{content.querySelectorAll('pre code:not(.language-mermaid)').forEach(function(el){hljs.highlightElement(el);});}catch(e){}"
        "if(__wantMath&&window.__dcMathRender)try{__dcMathRender(content);}catch(e){}"
        "await diagramReady;"
        "await Promise.all([document.fonts.ready].concat(Array.from(content.querySelectorAll('img'),function(img){"
        "return img.complete?Promise.resolve():new Promise(function(resolve){"
        "img.addEventListener('load',resolve,{once:true});img.addEventListener('error',resolve,{once:true});});})));"
        "window.__mdvReady=true;"
        "})();")
        .arg(theme)
        .arg(allowDollar ? 1 : 0)
        .arg(wantMath ? 1 : 0);

    QString html = QStringLiteral(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<base href=\"%1\"><title>%2</title>%3"
        "</head><body><article class=\"markdown-body\" id=\"content\"></article>%4"
        "<script id=\"md-data\" type=\"application/x-markdown-base64\">%5</script>"
        "<script>%6</script></body></html>")
        .arg(baseHref, title, head,
             showVersion ? QStringLiteral("<div id=\"mdv-ver\" title=\"Double Commander MarkdownView plugin\" "
                                         "data-label=\"MarkdownView v" MDV_VERSION "\"></div>")
                         : QString(),
             b64, bootstrap);

    /* A file:// page (not setHtml/data:) so the document's relative images and
     * the vendored scripts/CSS share an origin. Same shape as the macOS plugin,
     * which writes a temp page and loads it from disk. */
    removeTempFile();
    const QString name = QStringLiteral("dc-mdview-%1-%2.html")
                             .arg(QCoreApplication::applicationPid())
                             .arg(++counter_);
    tempPath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(name);
    QSaveFile out(tempPath_);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray bytes = html.toUtf8();
    if (out.write(bytes) != bytes.size() || !out.commit()) {
        tempPath_.clear();
        return false;
    }

    web_->load(QUrl::fromLocalFile(tempPath_));
    return true;
}

void MDView::findText(const QString &text, int flags) {
    if (!web_ || text.isEmpty()) return;
    QWebEnginePage *page = web_->page();
    QWebEnginePage::FindFlags f;
    if (flags & lcs_backwards) f |= QWebEnginePage::FindBackward;
    if (flags & lcs_matchcase) f |= QWebEnginePage::FindCaseSensitively;

    auto runFind = [page, text, f]() {
        page->findText(text, f, [page](const QWebEngineFindTextResult &result) {
            /* Chromium's find-in-page highlights are not a DOM selection, so
             * tests and diagnostics read the match count from this global. */
            page->runJavaScript(QStringLiteral("window.__mdvFindResult=%1")
                                    .arg(result.numberOfMatches()));
            if (result.numberOfMatches() == 0) QApplication::beep();
        });
    };
    if (flags & lcs_findfirst)
        page->runJavaScript(QStringLiteral("window.getSelection().removeAllRanges();"),
                            [runFind](const QVariant &) { runFind(); });
    else
        runFind();
}

int MDView::sendCommand(int command) {
    if (!web_ || !web_->page()) return LISTPLUGIN_ERROR;
    if (command == lc_copy) {
        web_->page()->triggerAction(QWebEnginePage::Copy);
        return LISTPLUGIN_OK;
    }
    if (command == lc_selectall) {
        web_->page()->runJavaScript(QString::fromLatin1(kSelectContentJS));
        return LISTPLUGIN_OK;
    }
    return LISTPLUGIN_ERROR;
}

/* ------------------------------------------------------------------ */
/* WLX exported API                                                    */
/* ------------------------------------------------------------------ */

static MDView *makeAndLoad(HWND parentWin, const QString &path) {
    auto *parent = reinterpret_cast<QWidget *>(parentWin);
    auto *view = new MDView(parent);
    if (!view->loadMarkdown(path)) {
        delete view;
        return nullptr;
    }
    view->show();
    return view;
}

static QString wildcardToQString(const char16_t *s) {
    size_t len = 0;
    while (s && s[len]) ++len;
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(s), int(len));
}

extern "C" {

__attribute__((visibility("default")))
HWND DCPCALL ListLoadW(HWND ParentWin, char16_t *FileToLoad, int ShowFlags) {
    (void)ShowFlags;
    MDView *view = makeAndLoad(ParentWin, wildcardToQString(FileToLoad));
    return view ? reinterpret_cast<HWND>(view) : 0;
}

__attribute__((visibility("default")))
HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags) {
    (void)ShowFlags;
    MDView *view = makeAndLoad(ParentWin, QString::fromLocal8Bit(FileToLoad ? FileToLoad : ""));
    return view ? reinterpret_cast<HWND>(view) : 0;
}

__attribute__((visibility("default")))
int DCPCALL ListLoadNextW(HWND ParentWin, HWND PluginWin, char16_t *FileToLoad, int ShowFlags) {
    (void)ParentWin; (void)ShowFlags;
    auto *view = reinterpret_cast<MDView *>(PluginWin);
    if (!view) return LISTPLUGIN_ERROR;
    return view->loadMarkdown(wildcardToQString(FileToLoad)) ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
int DCPCALL ListLoadNext(HWND ParentWin, HWND PluginWin, char *FileToLoad, int ShowFlags) {
    (void)ParentWin; (void)ShowFlags;
    auto *view = reinterpret_cast<MDView *>(PluginWin);
    if (!view) return LISTPLUGIN_ERROR;
    return view->loadMarkdown(QString::fromLocal8Bit(FileToLoad ? FileToLoad : ""))
               ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
void DCPCALL ListCloseWindow(HWND ListWin) {
    if (!ListWin) return;
    delete reinterpret_cast<MDView *>(ListWin);
}

__attribute__((visibility("default")))
int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
    (void)Parameter;
    auto *view = reinterpret_cast<MDView *>(ListWin);
    return view ? view->sendCommand(Command) : LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
int DCPCALL ListSearchTextW(HWND ListWin, char16_t *SearchString, int SearchParameter) {
    auto *view = reinterpret_cast<MDView *>(ListWin);
    if (!view || !SearchString || !SearchString[0]) return LISTPLUGIN_ERROR;
    view->findText(wildcardToQString(SearchString), SearchParameter);
    return LISTPLUGIN_OK;
}

__attribute__((visibility("default")))
int DCPCALL ListSearchText(HWND ListWin, char *SearchString, int SearchParameter) {
    auto *view = reinterpret_cast<MDView *>(ListWin);
    if (!view || !SearchString || !SearchString[0]) return LISTPLUGIN_ERROR;
    view->findText(QString::fromLocal8Bit(SearchString), SearchParameter);
    return LISTPLUGIN_OK;
}

__attribute__((visibility("default")))
void DCPCALL ListGetDetectString(char *DetectString, int maxlen) {
    if (!DetectString || maxlen <= 0) return;
    const char *s =
        "EXT=\"MD\"|EXT=\"MARKDOWN\"|EXT=\"MDOWN\"|EXT=\"MKD\"|EXT=\"MKDN\"|"
        "EXT=\"MDWN\"|EXT=\"MDTXT\"|EXT=\"MDTEXT\"|EXT=\"MARKDN\"|EXT=\"RMD\"|EXT=\"QMD\"";
    std::strncpy(DetectString, s, size_t(maxlen - 1));
    DetectString[maxlen - 1] = '\0';
}

__attribute__((visibility("default")))
void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *dps) {
    if (dps && dps->DefaultIniName[0])
        gIniPath = QString::fromLocal8Bit(dps->DefaultIniName);
}

} // extern "C"
