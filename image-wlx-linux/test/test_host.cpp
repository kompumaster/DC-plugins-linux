/*
 * Real-artifact harness for ImageView.wlx (Linux).
 *
 * dlopen()s the built plugin and drives its WLX ABI with a QWidget container,
 * exactly like Double Commander: ListLoadW, ListLoadNextW, ListSendCommand,
 * ListCloseWindow. It inspects the actual QGraphicsView/scene the plugin
 * creates and exercises formats, fall-through, opening zoom, keys, scrolling,
 * animation, SVG, size caps, copy and the FP-trap guard.
 *
 * Build:
 *   g++ -std=c++17 -fPIC -pthread -o build/test_host test/test_host.cpp \
 *       $(pkg-config --cflags --libs Qt6Widgets)
 * Run:
 *   ./build/test_host build/ImageView.wlx build/samples
 */
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QScrollBar>
#include <QThread>
#include <QToolButton>
#include <QWidget>

#include <dlfcn.h>
#include <fenv.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using HWND = std::uintptr_t;
#define lcp_fittowindow       2
#define lcp_fitlargeronly    32
#define lc_copy               1
#define lc_newparams          2
#define LISTPLUGIN_OK         0
#define LISTPLUGIN_ERROR      1

using LoadW_t = HWND (*)(HWND, char16_t *, int);
using LoadNextW_t = int (*)(HWND, HWND, char16_t *, int);
using Close_t = void (*)(HWND);
using Detect_t = void (*)(char *, int);
using Send_t = int (*)(HWND, int, int);

static LoadW_t LoadW;
static LoadNextW_t LoadNextW;
static Close_t Close;
static Detect_t Detect;
static Send_t Send;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string &msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg.c_str());
    if (ok) ++gPass; else ++gFail;
}

static QString utf16Path(const std::string &s) { return QString::fromStdString(s); }
static char16_t *wpath(const QString &s) {
    return reinterpret_cast<char16_t *>(const_cast<char16_t *>(
        reinterpret_cast<const char16_t *>(s.utf16())));
}

static bool waitFor(const std::function<bool()> &f, int ms = 8000) {
    QElapsedTimer t;
    t.start();
    while (!f() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return f();
}

static QGraphicsView *viewOf(QWidget *w) { return w ? w->findChild<QGraphicsView *>() : nullptr; }

static QGraphicsPixmapItem *pixmapOf(QWidget *w) {
    QGraphicsView *v = viewOf(w);
    if (!v || !v->scene()) return nullptr;
    for (QGraphicsItem *i : v->scene()->items())
        if (auto *p = dynamic_cast<QGraphicsPixmapItem *>(i)) return p;
    return nullptr;
}

static QSize sceneSize(QWidget *w) {
    QGraphicsView *v = viewOf(w);
    if (!v) return QSize();
    return v->scene()->sceneRect().size().toSize();
}

static qreal magOf(QWidget *w) {
    QGraphicsView *v = viewOf(w);
    return v ? v->transform().m11() : 0;
}

static QString labelContaining(QWidget *w, const QString &needle) {
    for (QLabel *l : w->findChildren<QLabel *>())
        if (l->text().contains(needle)) return l->text();
    return QString();
}

static QString infoText(QWidget *w) { return labelContaining(w, QStringLiteral(" × ")); }

struct Plugin {
    HWND handle = 0;
    QWidget *widget = nullptr;
};

static Plugin openPlugin(QWidget *parent, const std::string &path, int flags) {
    Plugin p;
    p.handle = LoadW(reinterpret_cast<HWND>(parent), wpath(utf16Path(path)), flags);
    if (!p.handle) return p;
    p.widget = reinterpret_cast<QWidget *>(p.handle);
    p.widget->setGeometry(parent->rect());   /* what DC's ResizeWindow does */
    QCoreApplication::processEvents();
    return p;
}

static void closePlugin(Plugin &p) {
    if (p.handle) Close(p.handle);
    p = Plugin();
    QCoreApplication::processEvents();
}

static void sendKey(QWidget *target, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent e(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(target, &e);
}

/* Counts the key events the plugin hands back to "DC" (the parent window). */
class KeyCounter : public QObject {
public:
    std::vector<int> keys;
    bool eventFilter(QObject *, QEvent *e) override {
        if (e->type() == QEvent::KeyPress)
            keys.push_back(static_cast<QKeyEvent *>(e)->key());
        return false;
    }
};

static bool haveSample(const QString &dir, const char *name) {
    return QFile::exists(dir + QLatin1Char('/') + QLatin1String(name));
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <ImageView.wlx> <samples-dir>\n", argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    LoadW = reinterpret_cast<LoadW_t>(dlsym(h, "ListLoadW"));
    LoadNextW = reinterpret_cast<LoadNextW_t>(dlsym(h, "ListLoadNextW"));
    Close = reinterpret_cast<Close_t>(dlsym(h, "ListCloseWindow"));
    Detect = reinterpret_cast<Detect_t>(dlsym(h, "ListGetDetectString"));
    Send = reinterpret_cast<Send_t>(dlsym(h, "ListSendCommand"));
    check(LoadW && LoadNextW && Close && Detect && Send, "all WLX entry points exported");
    if (!LoadW || !LoadNextW || !Close || !Detect || !Send) return 2;

    const QString dir = QString::fromLocal8Bit(argv[2]);
    const auto path = [&](const char *n) { return (dir + QLatin1Char('/') + QLatin1String(n)).toStdString(); };

    char det[2048] = {0};
    Detect(det, sizeof det);
    check(std::strlen(det) < sizeof det - 1, "detect string fits DC's buffer");
    check(std::strstr(det, "EXT=\"PNG\"") && std::strstr(det, "EXT=\"SVG\"") &&
              std::strstr(det, "EXT=\"GIF\""),
          "detect string covers PNG/SVG/GIF");
    check(!std::strstr(det, "EXT=\"PDF\""), "detect string does not claim PDF");

    QWidget parent;
    parent.resize(900, 700);
    parent.show();
    QCoreApplication::processEvents();

    /* ---- fall-through: not images -> NULL, DC tries the next lister ---- */
    const char *notImages[] = {"garbage.png", "disk.raw", "broken.svg", "missing.png"};
    for (const char *n : notImages) {
        Plugin p = openPlugin(&parent, path(n), 0);
        check(p.handle == 0, std::string("ListLoad returns NULL for ") + n);
        if (p.handle) closePlugin(p);
    }

    /* ---- formats ---- */
    struct Case { const char *name; int w, h; const char *info; };
    const Case cases[] = {
        {"photo.png", 320, 200, "320 × 200 · PNG"},
        {"photo.jpg", 320, 200, "320 × 200 · JPEG"},
        {"photo.bmp", 320, 200, "320 × 200 · BMP"},
        {"alpha.png", 200, 100, "200 × 100 · PNG"},
        {"pixel.png", 16, 16, "16 × 16 · PNG"},
        {"wide.png", 1600, 200, "1600 × 200 · PNG"},
        {"edge.svg", 40, 20, "40 × 20 pt · SVG"},
        {"edge.svgz", 40, 20, "40 × 20 pt · SVGZ"},
        {"pages.tiff", 320, 200, "320 × 200 · TIFF"},
        {"icon.ico", 32, 32, "32 × 32 · ICO"},
        {"anim.gif", 32, 32, "32 × 32 · GIF"},
        {"oriented.jpg", 200, 320, "200 × 320 · JPEG"},
    };
    for (const Case &c : cases) {
        if (!haveSample(dir, c.name)) continue;
        Plugin p = openPlugin(&parent, path(c.name), 0);
        if (!p.handle) { check(false, std::string(c.name) + " opens"); continue; }
        waitFor([&] { return pixmapOf(p.widget) || sceneSize(p.widget).width() > 1; }, 8000);
        const QSize sz = sceneSize(p.widget);
        check(sz.width() == c.w && sz.height() == c.h,
              std::string(c.name) + " scene " + std::to_string(sz.width()) + "x" +
                  std::to_string(sz.height()));
        const QString info = infoText(p.widget);
        check(info.contains(QString::fromUtf8(c.info)),
              std::string(c.name) + " info '" + info.toStdString() + "'");
        if (std::strcmp(c.name, "edge.svg") == 0 || std::strcmp(c.name, "edge.svgz") == 0) {
            check(pixmapOf(p.widget) == nullptr, std::string(c.name) + " is drawn as vector");
        }
        if (std::strcmp(c.name, "pages.tiff") == 0)
            check(!info.contains("frames"), "multi-page TIFF is not treated as animation");
        closePlugin(p);
    }

    /* ---- huge raster is capped for display, both sizes reported ---- */
    if (haveSample(dir, "huge.png")) {
        Plugin p = openPlugin(&parent, path("huge.png"), 0);
        check(p.handle != 0, "huge.png opens");
        waitFor([&] { return pixmapOf(p.widget) != nullptr; }, 15000);
        QGraphicsPixmapItem *it = pixmapOf(p.widget);
        check(it && it->pixmap().width() <= 16384, "display bitmap capped at 16384 px");
        check(infoText(p.widget).startsWith(QStringLiteral("20000 × 100")),
              "toolbar reports the real pixel size");
        closePlugin(p);
    }

    /* ---- opening zoom follows DC's ShowFlags ---- */
    {
        Plugin p = openPlugin(&parent, path("photo.png"), 0);
        check(std::fabs(magOf(p.widget) - 1.0) < 1e-6, "no Stretch -> 100%");
        closePlugin(p);

        p = openPlugin(&parent, path("photo.png"), lcp_fittowindow);
        const qreal fit = qMin(900.0 / 320.0, 670.0 / 200.0);
        check(std::fabs(magOf(p.widget) - fit) < 0.15,
              "Stretch -> fit (" + std::to_string(magOf(p.widget)) + ")");
        closePlugin(p);

        p = openPlugin(&parent, path("photo.png"), lcp_fittowindow | lcp_fitlargeronly);
        check(std::fabs(magOf(p.widget) - 1.0) < 1e-6, "Stretch only large, small image -> 100%");
        closePlugin(p);

        p = openPlugin(&parent, path("wide.png"), lcp_fittowindow | lcp_fitlargeronly);
        check(magOf(p.widget) < 1.0, "Stretch only large, wide image -> fit");
        closePlugin(p);
    }

    /* ---- double-click toggles Fit <-> 100% ---- */
    {
        Plugin p = openPlugin(&parent, path("photo.png"), 0);
        QGraphicsView *v = viewOf(p.widget);
        const qreal fit = qMin(900.0 / 320.0, 670.0 / 200.0);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(200, 200),
                        QPointF(200, 200), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v->viewport(), &dbl);
        QCoreApplication::processEvents();
        check(std::fabs(magOf(p.widget) - fit) < 0.15, "double-click fits the image");
        QCoreApplication::sendEvent(v->viewport(), &dbl);
        QCoreApplication::processEvents();
        check(std::fabs(magOf(p.widget) - 1.0) < 1e-6, "double-click again returns to 100%");
        closePlugin(p);
    }

    /* ---- lc_newparams re-fits at once ---- */
    {
        Plugin p = openPlugin(&parent, path("wide.png"), 0);
        const qreal before = magOf(p.widget);
        Send(p.handle, lc_newparams, lcp_fittowindow);
        check(before != magOf(p.widget) && magOf(p.widget) < 1.0,
              "lc_newparams re-fits the open image");
        closePlugin(p);
    }

    /* ---- keys: zoom stays with the plugin, the rest goes to DC ---- */
    {
        Plugin p = openPlugin(&parent, path("photo.png"), 0);
        QGraphicsView *v = viewOf(p.widget);
        check(v != nullptr, "plugin has a QGraphicsView");
        KeyCounter counter;
        parent.installEventFilter(&counter);

        sendKey(v, Qt::Key_Equal, Qt::ControlModifier);
        check(std::fabs(magOf(p.widget) - 1.25) < 0.01, "Ctrl+= zooms in one step");
        sendKey(v, Qt::Key_Minus, Qt::ControlModifier);
        sendKey(v, Qt::Key_Minus, Qt::ControlModifier);
        check(std::fabs(magOf(p.widget) - 0.75) < 0.01, "Ctrl+- twice zooms out to 75%");
        sendKey(v, Qt::Key_0, Qt::ControlModifier);
        check(std::fabs(magOf(p.widget) - 1.0) < 1e-6, "Ctrl+0 -> actual size");
        sendKey(v, Qt::Key_9, Qt::ControlModifier);
        const qreal fit = qMin(900.0 / 320.0, 670.0 / 200.0);
        check(std::fabs(magOf(p.widget) - fit) < 0.15, "Ctrl+9 -> fit");

        counter.keys.clear();
        sendKey(v, Qt::Key_1);
        check(counter.keys.size() == 1 && counter.keys[0] == Qt::Key_1,
              "plain digit 1 is handed to DC (view mode)");
        sendKey(v, Qt::Key_Escape);
        check(counter.keys.size() == 2 && counter.keys[1] == Qt::Key_Escape,
              "Esc is handed to DC");
        check(std::fabs(magOf(p.widget) - fit) < 0.15, "handed keys do not change the zoom");

        parent.removeEventFilter(&counter);
        closePlugin(p);
    }

    /* ---- scroll keys ---- */
    if (haveSample(dir, "tall.png")) {
        Plugin p = openPlugin(&parent, path("tall.png"), 0);
        QGraphicsView *v = viewOf(p.widget);
        v->verticalScrollBar()->setValue(0);
        const int maxV = v->verticalScrollBar()->maximum();
        check(maxV > 0, "tall image is pannable");
        sendKey(v, Qt::Key_PageDown);
        check(v->verticalScrollBar()->value() > 0, "PageDown scrolls down");
        sendKey(v, Qt::Key_End);
        check(v->verticalScrollBar()->value() == maxV, "End goes to the bottom");
        sendKey(v, Qt::Key_Home);
        check(v->verticalScrollBar()->value() == 0, "Home goes to the top");
        closePlugin(p);
    }

    /* ---- animation ---- */
    if (haveSample(dir, "anim.gif")) {
        Plugin p = openPlugin(&parent, path("anim.gif"), 0);
        QMovie *movie = p.widget ? p.widget->findChild<QMovie *>() : nullptr;
        check(movie != nullptr && movie->isValid(), "animated GIF creates a QMovie");
        if (movie) {
            check(movie->frameCount() == 3, "GIF frame count = 3");
            check(movie->state() == QMovie::Running, "GIF is playing");
            const int before = movie->currentFrameNumber();
            waitFor([&] { return movie->currentFrameNumber() != before; }, 3000);
            check(movie->currentFrameNumber() != before, "animation advances frames");
        }
        closePlugin(p);
    }

    /* ---- zoom limits and pixelated rendering ---- */
    {
        Plugin p = openPlugin(&parent, path("pixel.png"), 0);
        QGraphicsView *v = viewOf(p.widget);
        const qreal fit = qMin(900.0 / 16.0, 670.0 / 16.0);
        const qreal maxMag = qMax(64.0, fit * 2);
        for (int i = 0; i < 40; ++i) sendKey(v, Qt::Key_Equal, Qt::ControlModifier);
        check(magOf(p.widget) <= maxMag + 0.01, "zoom-in stops at the plugin's limit");
        check(!(v->renderHints() & QPainter::SmoothPixmapTransform),
              "past 200% raster pixels are drawn unsmoothed");
        closePlugin(p);
    }

    /* ---- copy ---- */
    {
        Plugin p = openPlugin(&parent, path("photo.png"), 0);
        waitFor([&] { return pixmapOf(p.widget) != nullptr; });
        check(Send(p.handle, lc_copy, 0) == LISTPLUGIN_OK, "lc_copy accepted");
        const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
        check(mime && mime->hasImage(), "clipboard carries an image");
        if (mime && mime->hasImage()) {
            const QImage img = qvariant_cast<QImage>(mime->imageData());
            check(img.width() == 320 && img.height() == 200, "clipboard image is the full size");
        }
        check(mime && mime->hasFormat(QStringLiteral("image/png")), "clipboard offers image/png");
        closePlugin(p);

        if (haveSample(dir, "edge.svg")) {
            p = openPlugin(&parent, path("edge.svg"), 0);
            check(Send(p.handle, lc_copy, 0) == LISTPLUGIN_OK, "SVG copy accepted");
            mime = QGuiApplication::clipboard()->mimeData();
            check(mime && mime->hasFormat(QStringLiteral("image/svg+xml")),
                  "SVG copy keeps the vector source");
            check(mime && mime->hasImage(), "SVG copy also offers a rendered image");
            closePlugin(p);
        }
        if (haveSample(dir, "anim.gif")) {
            p = openPlugin(&parent, path("anim.gif"), 0);
            check(Send(p.handle, lc_copy, 0) == LISTPLUGIN_OK, "GIF copy accepted");
            mime = QGuiApplication::clipboard()->mimeData();
            check(mime && mime->hasFormat(QStringLiteral("image/gif")),
                  "GIF copy keeps the original animated bytes");
            closePlugin(p);
        }
    }

    /* ---- resize refits in Fit mode ---- */
    {
        Plugin p = openPlugin(&parent, path("wide.png"), lcp_fittowindow);
        const qreal fit1 = magOf(p.widget);
        p.widget->resize(450, 400);
        QCoreApplication::processEvents();
        const qreal fit2 = magOf(p.widget);
        check(fit2 < fit1 && std::fabs(fit2 - qMin(450.0 / 1600.0, 370.0 / 200.0)) < 0.05,
              "resize re-fits in Fit mode");
        closePlugin(p);
    }

    /* ---- ListLoadNext reuses the window ---- */
    {
        Plugin p = openPlugin(&parent, path("photo.png"), 0);
        const int r = LoadNextW(reinterpret_cast<HWND>(&parent), p.handle, wpath(utf16Path(path("wide.png"))), 0);
        check(r == LISTPLUGIN_OK, "ListLoadNextW accepted the next image");
        check(sceneSize(p.widget).width() == 1600, "next image replaced the scene");
        closePlugin(p);
    }

    /* ---- FP-exception traps, as FPC (Double Commander) enables them ---- */
    {
        fesetenv(FE_DFL_ENV);
        feenableexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW);
        for (int i = 0; i < 2; ++i) {
            const QString copy = dir + QStringLiteral("/trap%1.png").arg(i);
            QFile::copy(dir + QStringLiteral("/photo.png"), copy);
            Plugin p = openPlugin(&parent, copy.toStdString(), 0);
            if (p.handle) {
                waitFor([&] { return pixmapOf(p.widget) != nullptr; });
                closePlugin(p);
            }
        }
        check(true, "survived opening images with FP-exception traps enabled");
        check((fegetexcept() & (FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW)) ==
                  (FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW),
              "plugin restored the host FP-exception environment");
        fedisableexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW);
    }

    /* ---- version surface ---- */
    {
        Plugin p = openPlugin(&parent, path("photo.png"), 0);
        check(!labelContaining(p.widget, QStringLiteral("v0.1.0")).isEmpty(),
              "version surface present");
        closePlugin(p);
    }

    parent.hide();
    dlclose(h);
    std::printf("\nRESULT: %s  (%d passed, %d failed)\n",
                gFail == 0 ? "PASS" : "FAIL", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
