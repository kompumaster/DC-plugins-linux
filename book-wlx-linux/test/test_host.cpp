/*
 * Real-artifact harness for BookView.wlx (Linux).
 *
 * dlopen()s the built plugin, drives the WLX ABI like Double Commander, and
 * inspects the live DOM of the QWebEngineView it creates: EPUB 2/3 and FB2/FBZ
 * books, TOC, encoding, sanitising, images, search, copy, keys and errors.
 *
 * Build:
 *   g++ -std=c++17 -fPIC -pthread -o build/test_host test/test_host.cpp \
 *       $(pkg-config --cflags --libs Qt6Widgets Qt6WebEngineWidgets)
 * Run:
 *   ./build/test_host build/BookView.wlx build/samples
 */
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QThread>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWidget>

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

using HWND = std::uintptr_t;
#define lcs_findfirst 1
#define lc_copy       1
#define lc_selectall  3
#define LISTPLUGIN_OK 0

using LoadW_t = HWND (*)(HWND, char16_t *, int);
using LoadNextW_t = int (*)(HWND, HWND, char16_t *, int);
using Close_t = void (*)(HWND);
using Detect_t = void (*)(char *, int);
using Send_t = int (*)(HWND, int, int);
using SearchW_t = int (*)(HWND, char16_t *, int);

static LoadW_t LoadW;
static LoadNextW_t LoadNextW;
static Close_t Close;
static Detect_t Detect;
static Send_t Send;
static SearchW_t SearchW;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string &msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg.c_str());
    if (ok) ++gPass; else ++gFail;
}

static char16_t *wide(const QString &s) {
    return reinterpret_cast<char16_t *>(const_cast<char16_t *>(
        reinterpret_cast<const char16_t *>(s.utf16())));
}

static QVariant js(QWebEnginePage *page, const QString &script, int timeout = 5000) {
    QVariant result;
    QEventLoop loop;
    page->runJavaScript(script, [&](const QVariant &v) { result = v; loop.quit(); });
    QTimer::singleShot(timeout, &loop, &QEventLoop::quit);
    loop.exec();
    return result;
}

static bool waitFor(const std::function<bool()> &f, int ms = 15000) {
    QElapsedTimer t;
    t.start();
    while (!f() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return f();
}

static bool waitJs(QWebEnginePage *page, const QString &expr, int ms = 15000) {
    return waitFor([page, expr] { return js(page, expr, 1500).toBool(); }, ms);
}

struct Book {
    HWND handle = 0;
    QWidget *widget = nullptr;
    QWebEngineView *web = nullptr;
    QWebEnginePage *page = nullptr;
};

static Book openBook(QWidget *parent, const QString &path) {
    Book b;
    b.handle = LoadW(reinterpret_cast<HWND>(parent), wide(path), 0);
    if (!b.handle) return b;
    b.widget = reinterpret_cast<QWidget *>(b.handle);
    b.widget->setGeometry(parent->rect());
    b.web = b.widget->findChild<QWebEngineView *>();
    b.page = b.web ? b.web->page() : nullptr;
    QCoreApplication::processEvents();
    return b;
}

static void closeBook(Book &b) {
    if (b.handle) Close(b.handle);
    b = Book();
    QCoreApplication::processEvents();
}

static bool waitLoaded(Book &b, int ms = 20000) {
    return waitJs(b.page,
                  "window.__dcReady===true || "
                  "document.body.innerText.indexOf(\"Can't open\")>=0", ms);
}

static QString textOf(Book &b) {
    return js(b.page, QStringLiteral("document.getElementById('book') ? "
                                     "document.getElementById('book').innerText : ''"))
        .toString();
}

class KeyCounter : public QObject {
public:
    std::vector<int> keys;
    bool eventFilter(QObject *, QEvent *e) override {
        if (e->type() == QEvent::KeyPress)
            keys.push_back(static_cast<QKeyEvent *>(e)->key());
        return false;
    }
};

static void sendKey(QWidget *target, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent e(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(target, &e);
    QCoreApplication::processEvents();
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <BookView.wlx> <samples-dir>\n", argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    LoadW = reinterpret_cast<LoadW_t>(dlsym(h, "ListLoadW"));
    LoadNextW = reinterpret_cast<LoadNextW_t>(dlsym(h, "ListLoadNextW"));
    Close = reinterpret_cast<Close_t>(dlsym(h, "ListCloseWindow"));
    Detect = reinterpret_cast<Detect_t>(dlsym(h, "ListGetDetectString"));
    Send = reinterpret_cast<Send_t>(dlsym(h, "ListSendCommand"));
    SearchW = reinterpret_cast<SearchW_t>(dlsym(h, "ListSearchTextW"));
    check(LoadW && LoadNextW && Close && Detect && Send && SearchW,
          "all WLX entry points exported");
    if (!LoadW || !LoadNextW || !Close || !Detect || !Send || !SearchW) return 2;

    const QString dir = QString::fromLocal8Bit(argv[2]);
    auto path = [&](const char *n) { return dir + QLatin1Char('/') + QLatin1String(n); };

    char det[2048] = {0};
    Detect(det, sizeof det);
    check(std::strstr(det, "EXT=\"EPUB\"") && std::strstr(det, "EXT=\"FB2\"") &&
              std::strstr(det, "EXT=\"FBZ\""),
          "detect string covers EPUB/FB2/FBZ");

    QWidget parent;
    parent.resize(1100, 850);
    parent.show();
    QCoreApplication::processEvents();

    struct Sample {
        const char *name;
        const char *needle;
        const char *title;
    };
    const Sample samples[] = {
        {"sample3.epub", "Chapter Three: The Lamp", "The Wandering Lamp"},
        {"sample2.epub", "кодировке windows-1251", "A Ledger of Small Weights"},
        {"sample.fb2", "Блуждающая лампа", "Блуждающая лампа"},
        {"sample.fbz", "Глава первая. Гавань", "Блуждающая лампа"},
    };

    for (const Sample &s : samples) {
        Book b = openBook(&parent, path(s.name));
        check(b.handle != 0, std::string(s.name) + " opens");
        if (!b.handle) continue;
        check(waitLoaded(b), std::string(s.name) + " renders");
        check(textOf(b).contains(QString::fromUtf8(s.needle)),
              std::string(s.name) + " contains its text");
        check(js(b.page, QStringLiteral("document.querySelectorAll('#book .chapter').length"))
                      .toInt() >= 2,
              std::string(s.name) + " has chapters");
        check(js(b.page, QStringLiteral("document.querySelectorAll('#toc-list a').length"))
                      .toInt() >= 2,
              std::string(s.name) + " has a contents list");
        const QString title = js(b.page, QStringLiteral(
            "(document.getElementById('book-title')||{}).getAttribute ? "
            "document.getElementById('book-title').getAttribute('data-label') : ''"))
            .toString();
        check(title.contains(QString::fromUtf8(s.title)),
              std::string(s.name) + " title '" + title.toStdString() + "'");
        check(js(b.page, QStringLiteral(
                       "Array.prototype.some.call(document.querySelectorAll('#book img'),"
                       "function(i){return i.complete && i.naturalWidth>0;})"))
                      .toBool(),
              std::string(s.name) + " loaded at least one image");

        if (std::strcmp(s.name, "sample3.epub") == 0 || std::strstr(s.name, ".fb")) {
            check(js(b.page, QStringLiteral("window.PWNED===undefined")).toBool(),
                  std::string(s.name) + " did not run hostile script");
            check(js(b.page, QStringLiteral("document.querySelector('#book script')===null"))
                      .toBool(),
                  std::string(s.name) + " stripped <script>");
            check(js(b.page, QStringLiteral("document.querySelector('#book iframe')===null"))
                      .toBool(),
                  std::string(s.name) + " stripped <iframe>");
            check(js(b.page, QStringLiteral(
                           "document.querySelector('#book img[src*=\"example.invalid\"]')===null"))
                      .toBool(),
                  std::string(s.name) + " stripped remote images");
        }
        if (std::strstr(s.name, ".fb") != nullptr) {
            check(js(b.page, QStringLiteral("document.querySelector('#book .epigraph')!==null"))
                      .toBool(),
                  std::string(s.name) + " maps FB2 epigraphs");
            check(js(b.page, QStringLiteral("document.querySelector('#book table')!==null"))
                      .toBool(),
                  std::string(s.name) + " maps FB2 tables");
        }
        closeBook(b);
    }

    /* ---- TOC, text size and scrolling keys stay with the reader ---- */
    {
        Book b = openBook(&parent, path("sample3.epub"));
        check(waitLoaded(b), "key test book loaded");
        const bool tocBefore = js(b.page, QStringLiteral(
            "document.documentElement.classList.contains('toc-open')")).toBool();
        sendKey(b.widget, Qt::Key_T);
        QThread::msleep(200);
        QCoreApplication::processEvents();
        const bool tocAfter = js(b.page, QStringLiteral(
            "document.documentElement.classList.contains('toc-open')")).toBool();
        check(tocAfter != tocBefore, "t toggles the contents sidebar");

        const QString sizeBefore = js(b.page, QStringLiteral(
            "document.documentElement.style.getPropertyValue('--font-size')")).toString();
        sendKey(b.widget, Qt::Key_Plus);
        QThread::msleep(150);
        QCoreApplication::processEvents();
        const QString sizeAfter = js(b.page, QStringLiteral(
            "document.documentElement.style.getPropertyValue('--font-size')")).toString();
        check(!sizeBefore.isEmpty() && sizeBefore != sizeAfter, "+ increases the text size");

        const double y0 = js(b.page, QStringLiteral("window.scrollY")).toDouble();
        sendKey(b.widget, Qt::Key_PageDown);
        QThread::msleep(300);
        QCoreApplication::processEvents();
        const double y1 = js(b.page, QStringLiteral("window.scrollY")).toDouble();
        check(y1 > y0, "PageDown scrolls the book");

        KeyCounter counter;
        parent.installEventFilter(&counter);
        counter.keys.clear();
        sendKey(b.widget, Qt::Key_Escape);
        check(!counter.keys.empty() && counter.keys.back() == Qt::Key_Escape,
              "Esc is handed to DC");
        counter.keys.clear();
        sendKey(b.widget, Qt::Key_1);
        check(!counter.keys.empty() && counter.keys.back() == Qt::Key_1,
              "plain digit is handed to DC (view mode)");
        parent.removeEventFilter(&counter);
        closeBook(b);
    }

    /* ---- search waits for the whole book ---- */
    {
        Book b = openBook(&parent, path("sample3.epub"));
        check(b.handle != 0, "search book opens");
        /* Issued before the reader has reported ready: it must be queued and
         * still find text in the last chapter. */
        SearchW(b.handle, wide(QStringLiteral("wandered")), lcs_findfirst);
        check(waitLoaded(b), "search book loaded");
        check(waitJs(b.page, QStringLiteral("window.__dcFindResult===1"), 15000),
              "queued search finds the last chapter");
        SearchW(b.handle, wide(QStringLiteral("zzzz-not-in-this-book")), lcs_findfirst);
        check(waitJs(b.page, QStringLiteral("window.__dcFindResult===0"), 15000),
              "a miss reports zero matches");
        closeBook(b);
    }

    /* ---- copy: only the book, never the chrome ---- */
    {
        Book b = openBook(&parent, path("sample3.epub"));
        check(waitLoaded(b), "copy book loaded");
        check(Send(b.handle, lc_selectall, 0) == LISTPLUGIN_OK, "lc_selectall accepted");
        check(waitFor([&] {
                  return js(b.page, QStringLiteral("window.getSelection().toString().length"))
                             .toInt() > 100;
              }, 5000),
              "Select All selected the book");
        check(Send(b.handle, lc_copy, 0) == LISTPLUGIN_OK, "lc_copy accepted");
        check(waitFor([] {
                  return QGuiApplication::clipboard()->text().contains(
                      QStringLiteral("Chapter One"));
              }, 5000),
              "copy reached the clipboard");
        const QString text = QGuiApplication::clipboard()->text();
        check(text.contains(QStringLiteral("Chapter One")), "clipboard has the book text");
        check(text.contains(QStringLiteral("Chapter Three")),
              "clipboard spans the whole book");
        check(!text.contains(QStringLiteral("Contents")),
              "clipboard excludes the sidebar chrome");
        const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
        check(mime && mime->hasHtml(), "clipboard also offers rich text");
        closeBook(b);
    }

    /* ---- ListLoadNext replaces the book ---- */
    {
        Book b = openBook(&parent, path("sample3.epub"));
        check(waitLoaded(b), "next: first book loaded");
        check(LoadNextW(reinterpret_cast<HWND>(&parent), b.handle, wide(path("sample2.epub")), 0) ==
                  LISTPLUGIN_OK,
              "ListLoadNextW accepted the next book");
        /* The readiness global belongs to the old document until the new one
         * replaces it, so wait for the new book's own text. */
        check(waitFor([&] { return textOf(b).contains(QStringLiteral("кодировке windows-1251")); }),
              "next: second book loaded");
        closeBook(b);
    }

    /* ---- a malformed book gets an explanation and still answers search ---- */
    {
        Book b = openBook(&parent, path("garbage.epub"));
        check(b.handle != 0, "malformed book still opens a view");
        check(waitJs(b.page, QStringLiteral("document.body.innerText.indexOf(\"Can't open\")>=0")),
              "malformed book shows the reason");
        SearchW(b.handle, wide(QStringLiteral("EPUB")), lcs_findfirst);
        check(waitJs(b.page, QStringLiteral("window.__dcFindResult>=1"), 10000),
              "search against the error page does not hang");
        closeBook(b);
    }

    parent.hide();
    dlclose(h);
    std::printf("\nRESULT: %s  (%d passed, %d failed)\n",
                gFail == 0 ? "PASS" : "FAIL", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
