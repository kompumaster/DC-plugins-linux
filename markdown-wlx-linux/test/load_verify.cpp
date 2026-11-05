/*
 * load_verify — drives the REAL built MarkdownView.wlx the way Double Commander
 * does on Linux: dlopen the .wlx, call ListSetDefaultParams / ListLoadW with a
 * QWidget* container, then observe the QWebEngineView DC would have embedded.
 *
 * Build:
 *   g++ -std=c++17 -fPIC -o build/load_verify test/load_verify.cpp \
 *       $(pkg-config --cflags --libs Qt6Widgets Qt6WebEngineWidgets)
 * Run:
 *   ./build/load_verify build/MarkdownView.wlx
 */

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWidget>

#include <dlfcn.h>

#include <cstdio>
#include <cstring>

using HWND = std::uintptr_t;

struct ListDefaultParamStruct {
    int size;
    std::uint32_t low, high;
    char ini[32000];
};

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static QVariant js(QWebEnginePage *page, const QString &script, int timeoutMs = 4000) {
    QVariant result;
    QEventLoop loop;
    page->runJavaScript(script, [&](const QVariant &v) { result = v; loop.quit(); });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return result;
}

static bool waitReady(QWebEnginePage *page, int timeoutMs) {
    const int step = 100;
    for (int waited = 0; waited < timeoutMs; waited += step) {
        if (js(page, QStringLiteral("window.__mdvReady===true")).toBool()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(step);
    }
    return false;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path/to/MarkdownView.wlx>\n", argv[0]);
        return 2;
    }
    const QByteArray artifact = QFile::encodeName(QString::fromLocal8Bit(argv[1]));

    void *mod = dlopen(artifact.constData(), RTLD_NOW | RTLD_LOCAL);
    if (!mod) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto ListLoadW = reinterpret_cast<HWND (*)(HWND, char16_t *, int)>(dlsym(mod, "ListLoadW"));
    auto ListLoadNextW = reinterpret_cast<int (*)(HWND, HWND, char16_t *, int)>(dlsym(mod, "ListLoadNextW"));
    auto ListCloseWindow = reinterpret_cast<void (*)(HWND)>(dlsym(mod, "ListCloseWindow"));
    auto ListGetDetectString = reinterpret_cast<void (*)(char *, int)>(dlsym(mod, "ListGetDetectString"));
    auto ListSearchTextW = reinterpret_cast<int (*)(HWND, char16_t *, int)>(dlsym(mod, "ListSearchTextW"));
    auto ListSendCommand = reinterpret_cast<int (*)(HWND, int, int)>(dlsym(mod, "ListSendCommand"));
    auto ListSetDefaultParams = reinterpret_cast<void (*)(ListDefaultParamStruct *)>(dlsym(mod, "ListSetDefaultParams"));

    check(ListLoadW && ListLoadNextW && ListCloseWindow && ListGetDetectString &&
              ListSearchTextW && ListSendCommand && ListSetDefaultParams,
          "all WLX entry points exported");

    char detect[2048] = {0};
    ListGetDetectString(detect, sizeof(detect));
    check(std::strstr(detect, "EXT=\"MD\"") != nullptr, "detect string claims .md");

    QTemporaryDir tmp;
    check(tmp.isValid(), "temp dir");
    const QString dir = tmp.path();
    QFile svg(dir + "/pic.svg");
    check(svg.open(QIODevice::WriteOnly), "write sample SVG");
    svg.write("<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'>"
              "<rect width='40' height='20' fill='#c00'/></svg>");
    svg.close();

    const QString mdPath = dir + "/sample.md";
    QFile md(mdPath);
    check(md.open(QIODevice::WriteOnly), "write sample Markdown");
    md.write("# Hello Linux\n\n"
             "Markdown **works** here, and *so* does `inline code`.\n\n"
             "```js\nconst x = 1;\n```\n\n"
             "![pic](pic.svg)\n");
    md.close();

    QWidget parent;
    parent.resize(900, 700);
    parent.show();

    ListDefaultParamStruct dps{};
    dps.size = sizeof(dps);
    std::snprintf(dps.ini, sizeof(dps.ini), "%s/MarkdownView.ini", QFile::encodeName(dir).constData());
    ListSetDefaultParams(&dps);

    HWND win = ListLoadW(reinterpret_cast<HWND>(&parent),
                         reinterpret_cast<char16_t *>(const_cast<char16_t *>(
                             reinterpret_cast<const char16_t *>(mdPath.utf16()))),
                         0);
    check(win != 0, "ListLoadW returned a plugin window");
    if (!win) return 1;

    auto *widget = reinterpret_cast<QWidget *>(win);
    check(widget->parentWidget() == &parent, "plugin widget is parented into DC's container");

    auto *web = widget->findChild<QWebEngineView *>();
    check(web != nullptr, "plugin window contains a QWebEngineView");
    if (!web) return 1;

    bool loaded = false;
    QEventLoop loadLoop;
    QObject::connect(web->page(), &QWebEnginePage::loadFinished, [&](bool ok) { loaded = ok; loadLoop.quit(); });
    QTimer::singleShot(20000, &loadLoop, &QEventLoop::quit);
    if (web->page()->isLoading()) loadLoop.exec();
    else loaded = true;
    check(loaded, "page loaded");

    check(waitReady(web->page(), 20000), "markdown render finished (__mdvReady)");

    const QString text = js(web->page(), QStringLiteral("document.getElementById('content').innerText")).toString();
    check(text.contains("Hello Linux"), "rendered heading text");
    check(text.contains("Markdown works here"), "rendered emphasis text");
    check(text.contains("inline code"), "rendered inline code");
    check(js(web->page(), QStringLiteral("document.querySelectorAll('h1').length")).toInt() == 1,
          "markdown parsed into an <h1>");
    check(js(web->page(), QStringLiteral("document.querySelectorAll('pre code.hljs').length")).toInt() >= 1,
          "highlight.js applied to the fenced block");
    check(js(web->page(), QStringLiteral(
                              "!!document.querySelector('#content img') && "
                              "document.querySelector('#content img').naturalWidth > 0"))
              .toBool(),
          "relative local image loaded through the document's base URL");
    check(js(web->page(), QStringLiteral(
                              "document.getElementById('mdv-ver') && "
                              "document.getElementById('mdv-ver').textContent === ''"))
              .toBool(),
          "version badge is CSS-generated, not DOM text (Find/Copy cannot hit it)");

    /* Copy / Select All must go through ListSendCommand on DC. Select All may
     * only select the content element's children. */
    check(ListSendCommand(win, 3 /* lc_selectall */, 0) == 0, "ListSendCommand(lc_selectall) accepted");
    js(web->page(), QStringLiteral("window.__sel=window.getSelection().toString();"));
    QThread::msleep(200);
    const QString sel = js(web->page(), QStringLiteral("window.__sel")).toString();
    check(sel.contains("Hello Linux"), "Select All reaches the rendered content");
    check(!sel.contains("MarkdownView v"), "Select All excludes the version badge");

    /* Search: the W variant is what DC prefers, and it must find rendered
     * text, not Markdown source. */
    const QString needle = QStringLiteral("works");
    check(ListSearchTextW(win, reinterpret_cast<char16_t *>(const_cast<char16_t *>(
                                     reinterpret_cast<const char16_t *>(needle.utf16()))),
                          1 /* lcs_findfirst */) == 0,
          "ListSearchTextW accepted");
    int matches = -1;
    for (int i = 0; i < 50 && matches < 0; ++i) {
        QVariant v = js(web->page(), QStringLiteral("window.__mdvFindResult"));
        if (v.isValid() && !v.isNull()) matches = v.toInt();
        else {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(100);
        }
    }
    check(matches == 1, "search matched the rendered occurrence exactly once");

    /* Same file through ListLoadNextW: DC uses it when the user presses N/P. */
    check(ListLoadNextW(reinterpret_cast<HWND>(&parent), win,
                        reinterpret_cast<char16_t *>(const_cast<char16_t *>(
                            reinterpret_cast<const char16_t *>(mdPath.utf16()))),
                        0) == 0,
          "ListLoadNextW reuses the plugin window");

    ListCloseWindow(win);
    check(parent.findChild<QWebEngineView *>() == nullptr, "ListCloseWindow tore the view down");

    dlclose(mod);
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
