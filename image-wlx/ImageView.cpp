/*
 * ImageView.wlx — Double Commander Lister (WLX) plugin for Linux.
 *
 * Linux port of the upstream macOS plugin (NikolaiSachok/DC-plugins).
 * Shows images in the F3 viewer and Quick View with free zoom
 * (wheel/keyboard/toolbar), drag panning, and SVG that stays
 * vector-sharp at any magnification.
 *
 * The macOS plugin is AppKit + ImageIO + CoreSVG. On Unix, Double Commander
 * hands a WLX plugin a QWidget* and expects a QWidget* back, so this port is
 * C++/Qt:
 *   - raster decoding -> QImageReader (Qt image plugins; the detect string is
 *     built from the formats this runtime actually supports)
 *   - SVG/SVGZ        -> QSvgRenderer (re-rasterized through the QPainter
 *     transform on every paint, so it stays sharp; no scripts, no external
 *     fetches)
 *   - zoom/pan        -> QGraphicsView transform + this plugin's own drag
 *   - animation       -> QMovie (frame timing and loop count come from the
 *     file; APNG is the one macOS format Qt does not animate)
 *   - clipboard       -> QClipboard with image/png + the file's own bytes
 *
 * Double Commander enables floating-point exception traps (it is an FPC app);
 * every call into image decoding, SVG rendering or transform math runs with
 * the default masked FP environment (FpGuard), then restores the host's.
 */

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGraphicsItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QMovie>
#include <QPainter>
#include <QProgressBar>
#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <QtConcurrent/QtConcurrentRun>
#include <QtSvg/QSvgRenderer>

#include <dlfcn.h>
#include <fenv.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "listplug.h"

#define IV_VERSION "0.1.0"

static const double kMaxDisplaySide   = 16384;
static const double kMaxDisplayPixels = 64.0 * 1024 * 1024;
static const int    kToolbarHeight    = 30;
static const double kMaxMagnification = 64;

/* ---- FP guard ----------------------------------------------------------- */

struct FpGuard {
    fenv_t env;
    FpGuard() { fegetenv(&env); fesetenv(FE_DFL_ENV); }
    ~FpGuard() { fesetenv(&env); }
};

/* ---- Helpers ------------------------------------------------------------ */

static bool gunzip(const QByteArray &in, QByteArray &out) {
    if (in.size() < 18 || uchar(in[0]) != 0x1f || uchar(in[1]) != 0x8b) return false;
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return false;
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    QByteArray result;
    result.resize(int(qMin<qsizetype>(in.size() * 4, qsizetype(1 << 20))));
    int rc = Z_OK;
    while (rc == Z_OK) {
        if (zs.total_out >= quint64(result.size())) {
            if (result.size() > 256 * 1024 * 1024) break;   /* decompression bomb */
            result.resize(result.size() * 2);
        }
        zs.next_out = reinterpret_cast<Bytef *>(result.data()) + zs.total_out;
        zs.avail_out = uInt(result.size() - qsizetype(zs.total_out));
        rc = inflate(&zs, Z_NO_FLUSH);
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return false;
    result.resize(int(zs.total_out));
    out = result;
    return true;
}

static QString normalizedFormat(const QString &raw, const QString &path) {
    QString f = raw.toUpper();
    if (f == QLatin1String("JPG") || f == QLatin1String("JPE") || f == QLatin1String("JFIF"))
        return QStringLiteral("JPEG");
    if (f == QLatin1String("TIF")) return QStringLiteral("TIFF");
    if (f.isEmpty()) {
        f = QFileInfo(path).suffix().toUpper();
        if (f == QLatin1String("JPG") || f == QLatin1String("JPEG")) return QStringLiteral("JPEG");
        if (f == QLatin1String("TIF")) return QStringLiteral("TIFF");
    }
    return f;
}

static QString formatBytes(qint64 bytes) {
    if (bytes < 0) return QString();
    if (bytes < 1000) return QStringLiteral("%1 B").arg(bytes);
    const char *units[] = {"KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = -1;
    do { v /= 1000.0; ++u; } while (v >= 1000.0 && u < 3);
    return v >= 100 ? QStringLiteral("%1 %2").arg(qRound(v)).arg(units[u])
                    : QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(units[u]);
}

static QString formatDims(QSize s, bool vector) {
    return QStringLiteral("%1 × %2%3").arg(s.width()).arg(s.height())
        .arg(vector ? QStringLiteral(" pt") : QString());
}

/* The detect string is the set of formats this runtime can actually decode:
   one source of truth, and no column/plugin that is silently blank because a
   Qt image plugin is missing. PDF/EPS are not images this plugin shows. */
static QString buildDetectString() {
    QStringList exts;
    for (const QByteArray &f : QImageReader::supportedImageFormats()) {
        const QString s = QString::fromLatin1(f).toUpper();
        if (s == QLatin1String("PDF") || s.startsWith(QLatin1String("EPS"))) continue;
        if (!exts.contains(s)) exts << s;
    }
    if (exts.isEmpty()) exts << QStringLiteral("PNG") << QStringLiteral("JPG")
                            << QStringLiteral("JPEG") << QStringLiteral("SVG");
    exts.sort();
    QStringList parts;
    for (const QString &e : exts) parts << QStringLiteral("EXT=\"%1\"").arg(e);
    QString out = parts.join(QLatin1Char('|'));
    if (out.size() > 2000) out = out.left(out.lastIndexOf(QLatin1Char('|')));  /* DC's buffer */
    return out;
}

/* ---- Probe: what is this file? ------------------------------------------ */

enum class Kind { Raster, Vector };

struct Probe {
    bool ok = false;
    Kind kind = Kind::Raster;
    QString path;
    QString format;
    QSize pixelSize;      /* real pixels / vector points, for the toolbar */
    QSize displaySize;    /* what the scene holds (capped for huge rasters) */
    qint64 fileSize = 0;
    bool animated = false;
    int frameCount = 1;
    int imageIndex = 0;   /* which entry of a multi-image ICO/ICNS to show */
    QByteArray vectorData;
};

static Probe probeFile(const QString &path) {
    FpGuard fp;
    Probe p;
    p.path = path;
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) return p;
    p.fileSize = fi.size();
    const QString ext = fi.suffix().toLower();

    /* ---- SVG / SVGZ (vector) ---- */
    if (ext == QLatin1String("svg") || ext == QLatin1String("svgz")) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return p;
        if (f.size() <= 0 || f.size() > 64 * 1024 * 1024) return p;
        QByteArray data = f.readAll();
        f.close();
        if (ext == QLatin1String("svgz") || (data.size() > 2 && uchar(data[0]) == 0x1f &&
                                             uchar(data[1]) == 0x8b)) {
            QByteArray plain;
            if (!gunzip(data, plain)) return p;
            data = plain;
        }
        if (!data.contains("<svg") && !data.contains("<SVG")) return p;

        QSvgRenderer renderer;
        if (!renderer.load(data)) return p;
        QSizeF sz = renderer.defaultSize();
        if (sz.isEmpty()) sz = renderer.viewBoxF().size();
        if (sz.width() < 1 || sz.height() < 1 || sz.width() > 100000 || sz.height() > 100000)
            return p;

        p.kind = Kind::Vector;
        p.format = ext == QLatin1String("svgz") ? QStringLiteral("SVGZ") : QStringLiteral("SVG");
        p.pixelSize = QSize(qRound(sz.width()), qRound(sz.height()));
        p.displaySize = p.pixelSize;
        p.vectorData = data;
        p.ok = true;
        return p;
    }

    /* ---- raster ---- */
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return p;

    QSize sz = reader.size();
    if (!sz.isValid() || sz.width() < 1 || sz.height() < 1) return p;

    /* size() is pre-transform; report display-oriented dimensions. */
    const auto tr = reader.transformation();
    const bool rotated = tr & (QImageIOHandler::TransformationRotate90 |
                               QImageIOHandler::TransformationRotate270);
    if (rotated) sz.transpose();

    int count = reader.imageCount();
    if (count < 1) count = 1;

    /* Icons: Qt exposes their entries; show the largest, not the first. */
    int chosen = 0;
    if (count > 1 && (ext == QLatin1String("ico") || ext == QLatin1String("icns") ||
                      ext == QLatin1String("cur"))) {
        qint64 best = 0;
        for (int i = 0; i < count; ++i) {
            if (!reader.jumpToImage(i)) continue;
            const QSize s = reader.size();
            const qint64 area = qint64(s.width()) * s.height();
            if (area > best) { best = area; chosen = i; }
        }
        reader.jumpToImage(chosen);
        QSize s = reader.size();
        if (rotated) s.transpose();
        if (s.isValid() && !s.isEmpty()) sz = s;
    }

    double scale = 1.0;
    scale = qMin(scale, kMaxDisplaySide / double(qMax(sz.width(), sz.height())));
    scale = qMin(scale, std::sqrt(kMaxDisplayPixels / (double(sz.width()) * double(sz.height()))));

    p.kind = Kind::Raster;
    p.format = normalizedFormat(QString::fromLatin1(reader.format()), path);
    p.pixelSize = sz;
    p.displaySize = QSize(qMax(1, qRound(sz.width() * scale)),
                          qMax(1, qRound(sz.height() * scale)));
    p.animated = reader.supportsAnimation() && count > 1;
    p.frameCount = p.animated ? count : 1;
    p.imageIndex = chosen;
    p.ok = true;
    return p;
}

/* Decode a raster for display, capped. Runs on a worker thread. */
static QImage decodeRaster(const Probe &p) {
    FpGuard fp;
    QImageReader reader(p.path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    if (p.imageIndex > 0) reader.jumpToImage(p.imageIndex);
    if (p.displaySize != p.pixelSize) reader.setScaledSize(p.displaySize);
    QImage img = reader.read();
    if (img.isNull()) return img;
    if (img.size() != p.displaySize)
        img = img.scaled(p.displaySize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return img;
}

static QImage decodeFull(const Probe &p) {
    FpGuard fp;
    QImageReader reader(p.path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    if (p.imageIndex > 0) reader.jumpToImage(p.imageIndex);
    return reader.read();
}

/* ---- SVG item ----------------------------------------------------------- */

class SvgItem : public QGraphicsItem {
public:
    SvgItem(const QByteArray &data, QSizeF size) : size_(size) {
        renderer_ = new QSvgRenderer(data);
        setFlag(ItemUsesExtendedStyleOption, true);
    }
    ~SvgItem() override { delete renderer_; }

    QRectF boundingRect() const override { return QRectF(QPointF(0, 0), size_); }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        FpGuard fp;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        renderer_->render(painter, QRectF(QPointF(0, 0), size_));
        painter->restore();
    }

    bool valid() const { return renderer_ && renderer_->isValid(); }

private:
    QSvgRenderer *renderer_ = nullptr;
    QSizeF size_;
};

/* ---- View --------------------------------------------------------------- */

class IVView;

class IVCanvas : public QGraphicsView {
public:
    explicit IVCanvas(IVView *owner);

    QGraphicsPixmapItem *pixmapItem() const { return pixmap_; }
    SvgItem *svgItem() const { return svg_; }
    void setPixmap(const QPixmap &pm);
    void setSvg(const QByteArray &data, QSizeF size);
    void clearImage();
    void setCheckerboard(bool on);
    void setImageSize(QSizeF size);

    qreal magnification() const { return transform().m11(); }
    void setMagnificationAt(qreal mag, const QPoint &viewportPos);
    void applyMagnification(qreal mag);
    bool pannable() const;
    void updateCursor();
    void scrollLines(int dx, int dy);
    void scrollPages(int pages);
    void scrollToEdge(bool bottom);
    QPoint viewportCenter() const { return viewport()->rect().center(); }

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool event(QEvent *e) override;

private:
    void forwardKeyToHost(QKeyEvent *e);

    IVView *owner_;
    QGraphicsScene scene_;
    QGraphicsPixmapItem *pixmap_ = nullptr;
    SvgItem *svg_ = nullptr;
    bool checkerboard_ = false;
    bool dragging_ = false;
    QPoint lastDrag_;
};

class IVView : public QWidget {
public:
    explicit IVView(QWidget *parent);
    ~IVView() override;

    bool showFile(const Probe &p, int flags);
    int copyImage();
    void applyNewParams(int flags);
    void onCanvasResized();

    /* called by IVCanvas */
    void zoomStep(int direction);
    void zoomToFit();
    void zoomToActual();
    void zoomToPreset(int percent);
    void toggleFitAt(const QPointF &scenePos);
    void onUserZoom();
    qreal fitMagnification() const;
    QSize canvasViewportSize() const;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    enum Mode { Fit, FitLarge, Actual, Manual };

    static Mode modeForFlags(int flags);
    void buildToolbar();
    void layoutOverlay();
    void applyMode();
    void setMagnification(qreal mag, const QPoint &anchor);
    void updateZoomLabel();
    void updateInfo();
    void startDecode(quint64 token);
    void startAnimation();
    void stopAnimation();
    void showBusy(bool on);
    void showMessage(const QString &text);
    QImage renderVector(double scale);
    QString infoText() const;
    int currentFrameCount() const;

    IVCanvas *canvas_ = nullptr;
    QWidget *toolbar_ = nullptr;
    QLabel *info_ = nullptr;
    QLabel *version_ = nullptr;
    QLabel *message_ = nullptr;
    QProgressBar *busy_ = nullptr;
    QToolButton *zoomButton_ = nullptr;

    Probe probe_;
    bool hasImage_ = false;
    Mode mode_ = Actual;
    quint64 generation_ = 0;
    QImage displayImage_;
    QMovie *movie_ = nullptr;
    QFutureWatcher<QImage> *watcher_ = nullptr;
    QTimer *busyTimer_ = nullptr;
};

/* ---- IVCanvas ----------------------------------------------------------- */

IVCanvas::IVCanvas(IVView *owner) : owner_(owner) {
    setScene(&scene_);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::TextAntialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::NoAnchor);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setAlignment(Qt::AlignCenter);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setDragMode(QGraphicsView::NoDrag);
    setBackgroundBrush(palette().color(QPalette::Window));
    viewport()->setCursor(Qt::ArrowCursor);
}

void IVCanvas::setPixmap(const QPixmap &pm) {
    if (!pixmap_) {
        pixmap_ = new QGraphicsPixmapItem();
        pixmap_->setTransformationMode(Qt::FastTransformation);
        scene_.addItem(pixmap_);
    }
    pixmap_->setPixmap(pm);
    viewport()->update();
}

void IVCanvas::setSvg(const QByteArray &data, QSizeF size) {
    clearImage();
    svg_ = new SvgItem(data, size);
    scene_.addItem(svg_);
    setSceneRect(QRectF(QPointF(0, 0), size));
    viewport()->update();
}

void IVCanvas::clearImage() {
    if (pixmap_) { scene_.removeItem(pixmap_); delete pixmap_; pixmap_ = nullptr; }
    if (svg_) { scene_.removeItem(svg_); delete svg_; svg_ = nullptr; }
    viewport()->update();
}

void IVCanvas::setCheckerboard(bool on) {
    checkerboard_ = on;
    viewport()->update();
}

void IVCanvas::setImageSize(QSizeF size) {
    setSceneRect(QRectF(QPointF(0, 0), size));
    scene_.setSceneRect(QRectF(QPointF(0, 0), size));
}

void IVCanvas::drawBackground(QPainter *painter, const QRectF &rect) {
    QColor base = palette().color(QPalette::Window);
    const bool dark = base.lightness() < 128;
    painter->fillRect(rect, dark ? QColor(48, 48, 48) : QColor(250, 250, 250));
    if (!checkerboard_) return;

    const qreal mag = qMax(magnification(), 0.001);
    const qreal sq = 8.0 / mag;
    const QRectF r = rect.intersected(sceneRect());
    if (r.isEmpty()) return;

    const qreal x0 = std::floor(r.left() / sq) * sq;
    const qreal y0 = std::floor(r.top() / sq) * sq;
    const int nx = qMin(2048, int(std::ceil(r.width() / sq)) + 1);
    const int ny = qMin(2048, int(std::ceil(r.height() / sq)) + 1);
    const QColor light = dark ? QColor(64, 64, 64) : QColor(228, 228, 228);
    for (int y = 0; y < ny; ++y) {
        for (int x = (int(std::floor(x0 / sq)) + y) & 1; x < nx; x += 2) {
            painter->fillRect(QRectF(x0 + x * sq, y0 + y * sq, sq, sq).intersected(r), light);
        }
    }
}

bool IVCanvas::pannable() const {
    const QRectF s = sceneRect();
    if (s.isEmpty()) return false;
    return s.width() * magnification() > viewport()->width() + 1 ||
           s.height() * magnification() > viewport()->height() + 1;
}

void IVCanvas::updateCursor() {
    if (dragging_) return;
    viewport()->setCursor(pannable() ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void IVCanvas::applyMagnification(qreal mag) {
    QTransform t;
    t.scale(mag, mag);
    setTransform(t);
    updateCursor();
    owner_->onUserZoom();
}

void IVCanvas::setMagnificationAt(qreal mag, const QPoint &viewportPos) {
    const QPointF before = mapToScene(viewportPos);
    QTransform t;
    t.scale(mag, mag);
    setTransform(t);
    const QPointF after = mapToScene(viewportPos);
    translate(after.x() - before.x(), after.y() - before.y());
    updateCursor();
    owner_->onUserZoom();
}

void IVCanvas::scrollLines(int dx, int dy) {
    const qreal mag = qMax(magnification(), 0.001);
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(dx / mag));
    verticalScrollBar()->setValue(verticalScrollBar()->value() - qRound(dy / mag));
}

void IVCanvas::scrollPages(int pages) {
    scrollLines(0, -qRound(viewport()->height() * 0.9) * pages);
}

void IVCanvas::scrollToEdge(bool bottom) {
    verticalScrollBar()->setValue(bottom ? verticalScrollBar()->maximum()
                                         : verticalScrollBar()->minimum());
}

void IVCanvas::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && pannable()) {
        dragging_ = true;
        lastDrag_ = e->position().toPoint();
        viewport()->setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QGraphicsView::mousePressEvent(e);
}

void IVCanvas::mouseMoveEvent(QMouseEvent *e) {
    if (dragging_) {
        const QPoint now = e->position().toPoint();
        const QPoint d = now - lastDrag_;
        lastDrag_ = now;
        const qreal mag = qMax(magnification(), 0.001);
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(d.x() / mag));
        verticalScrollBar()->setValue(verticalScrollBar()->value() - qRound(d.y() / mag));
        e->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
}

void IVCanvas::mouseReleaseEvent(QMouseEvent *e) {
    if (dragging_ && e->button() == Qt::LeftButton) {
        dragging_ = false;
        updateCursor();
        e->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
}

void IVCanvas::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        owner_->toggleFitAt(mapToScene(e->position().toPoint()));
        e->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(e);
}

void IVCanvas::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier) {
        const int dy = e->angleDelta().y();
        if (dy == 0) return;
        const qreal factor = std::pow(1.0015, dy);
        const QPoint pos = e->position().toPoint();
        const QPointF before = mapToScene(pos);
        QTransform t = transform();
        t.scale(factor, factor);
        setTransform(t);
        const QPointF after = mapToScene(pos);
        translate(after.x() - before.x(), after.y() - before.y());
        owner_->onUserZoom();
        e->accept();
        return;
    }
    QGraphicsView::wheelEvent(e);
}

void IVCanvas::keyPressEvent(QKeyEvent *e) {
    const Qt::KeyboardModifiers mods = e->modifiers() &
        (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
    const bool keypad = e->modifiers() & Qt::KeypadModifier;
    const bool cmd = (mods & ~Qt::ShiftModifier) == Qt::ControlModifier;
    const bool plain = (mods & ~Qt::ShiftModifier) == Qt::NoModifier;

    if (cmd || plain) {
        if (e->key() == Qt::Key_Equal || e->key() == Qt::Key_Plus) {
            owner_->zoomStep(+1); e->accept(); return;
        }
        if (e->key() == Qt::Key_Minus || e->key() == Qt::Key_Underscore) {
            owner_->zoomStep(-1); e->accept(); return;
        }
        if (cmd && !keypad && e->key() == Qt::Key_0) {
            owner_->zoomToActual(); e->accept(); return;
        }
        if (cmd && !keypad && e->key() == Qt::Key_9) {
            owner_->zoomToFit(); e->accept(); return;
        }
    }

    if (cmd && e->key() == Qt::Key_Up)   { scrollToEdge(false); e->accept(); return; }
    if (cmd && e->key() == Qt::Key_Down) { scrollToEdge(true);  e->accept(); return; }

    if (mods == Qt::ShiftModifier && e->key() == Qt::Key_Space) {
        scrollPages(-1); e->accept(); return;
    }
    if (mods == Qt::NoModifier) {
        switch (e->key()) {
        case Qt::Key_Up:    scrollLines(0, -40); e->accept(); return;
        case Qt::Key_Down:  scrollLines(0, 40);  e->accept(); return;
        case Qt::Key_Left:  scrollLines(-40, 0); e->accept(); return;
        case Qt::Key_Right: scrollLines(40, 0);  e->accept(); return;
        case Qt::Key_PageUp:   scrollPages(-1); e->accept(); return;
        case Qt::Key_PageDown:
        case Qt::Key_Space:    scrollPages(1);  e->accept(); return;
        case Qt::Key_Home:  scrollToEdge(false); e->accept(); return;
        case Qt::Key_End:   scrollToEdge(true);  e->accept(); return;
        default: break;
        }
    }

    forwardKeyToHost(e);
    e->accept();
}

void IVCanvas::forwardKeyToHost(QKeyEvent *e) {
    QWidget *target = window();
    if (!target) return;
    QKeyEvent forwarded(QEvent::KeyPress, e->key(), e->modifiers(), e->text(),
                        e->isAutoRepeat(), e->count());
    using SendSpontaneous = bool (*)(QObject *, QEvent *);
    static SendSpontaneous sendSpontaneous = reinterpret_cast<SendSpontaneous>(
        dlsym(RTLD_DEFAULT, "_ZN16QCoreApplication20sendSpontaneousEventEP7QObjectP6QEvent"));
    if (sendSpontaneous) sendSpontaneous(target, &forwarded);
    else QCoreApplication::sendEvent(target, &forwarded);
}

void IVCanvas::resizeEvent(QResizeEvent *e) {
    QGraphicsView::resizeEvent(e);
    owner_->onCanvasResized();
    updateCursor();
}

bool IVCanvas::event(QEvent *e) {
    /* Wayland/X11 touchpad pinch arrives as a native gesture. Ctrl+wheel is
       the reliable fallback and is handled in wheelEvent. */
    if (e->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            const qreal factor = 1.0 + g->value();
            if (factor > 0.01 && factor < 100) {
                const QPoint pos = g->position().toPoint();
                const QPointF before = mapToScene(pos);
                QTransform t = transform();
                t.scale(factor, factor);
                setTransform(t);
                const QPointF after = mapToScene(pos);
                translate(after.x() - before.x(), after.y() - before.y());
                owner_->onUserZoom();
            }
            return true;
        }
    }
    return QGraphicsView::event(e);
}

/* ---- IVView ------------------------------------------------------------- */

IVView::IVView(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);

    canvas_ = new IVCanvas(this);
    toolbar_ = new QWidget(this);
    toolbar_->setFixedHeight(kToolbarHeight);
    toolbar_->setAutoFillBackground(true);

    busy_ = new QProgressBar(this);
    busy_->setRange(0, 0);
    busy_->setTextVisible(false);
    busy_->setFixedSize(140, 14);
    busy_->hide();

    message_ = new QLabel(this);
    message_->setAlignment(Qt::AlignCenter);
    message_->setWordWrap(true);
    message_->setAttribute(Qt::WA_TransparentForMouseEvents);
    message_->hide();

    busyTimer_ = new QTimer(this);
    busyTimer_->setSingleShot(true);

    buildToolbar();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar_);
    layout->addWidget(canvas_, 1);

    /* DC focuses the widget it was handed (Qt branch of TWlxModule.SetFocus);
       hand that focus to the canvas, which owns the keys. */
    setFocusProxy(canvas_);
}

IVView::~IVView() {
    ++generation_;
    stopAnimation();
    if (watcher_) { watcher_->disconnect(); watcher_->deleteLater(); watcher_ = nullptr; }
}

void IVView::buildToolbar() {
    auto *line = new QWidget(toolbar_);
    line->setFixedHeight(1);
    line->setAutoFillBackground(true);
    QPalette lp = line->palette();
    lp.setColor(QPalette::Window, palette().color(QPalette::Mid));
    line->setPalette(lp);

    auto *row = new QWidget(toolbar_);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(8, 0, 10, 0);
    h->setSpacing(2);

    auto makeButton = [this, row](const QString &text, const QString &tip) {
        auto *b = new QToolButton(row);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return b;
    };

    auto *fit = makeButton(QStringLiteral("Fit"), tr("Zoom to Fit (Ctrl+9)"));
    auto *actual = makeButton(QStringLiteral("1:1"), tr("Actual Size (Ctrl+0)"));
    auto *out = makeButton(QStringLiteral("−"), tr("Zoom Out (Ctrl+-)"));
    auto *in = makeButton(QStringLiteral("+"), tr("Zoom In (Ctrl+=)"));

    zoomButton_ = new QToolButton(row);
    zoomButton_->setPopupMode(QToolButton::InstantPopup);
    zoomButton_->setFocusPolicy(Qt::NoFocus);
    zoomButton_->setToolTip(tr("Zoom"));
    zoomButton_->setMinimumWidth(64);
    auto *menu = new QMenu(zoomButton_);
    auto *miFit = menu->addAction(tr("Zoom to Fit"));
    auto *miActual = menu->addAction(tr("Actual Size"));
    menu->addSeparator();
    QList<QAction *> presets;
    for (int pct : {25, 50, 200, 400, 800, 1600})
        presets << menu->addAction(QStringLiteral("%1%").arg(pct));
    zoomButton_->setMenu(menu);

    info_ = new QLabel(row);
    info_->setTextInteractionFlags(Qt::NoTextInteraction);
    info_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    info_->setFocusPolicy(Qt::NoFocus);
    info_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QPalette ip = info_->palette();
    ip.setColor(QPalette::WindowText, ip.color(QPalette::Disabled, QPalette::WindowText));
    info_->setPalette(ip);

    version_ = new QLabel(QStringLiteral("v" IV_VERSION), row);
    version_->setFocusPolicy(Qt::NoFocus);
    version_->setToolTip(QStringLiteral("ImageView " IV_VERSION));
    QPalette vp = version_->palette();
    vp.setColor(QPalette::WindowText, vp.color(QPalette::Disabled, QPalette::WindowText));
    version_->setPalette(vp);

    h->addWidget(fit);
    h->addWidget(actual);
    h->addWidget(out);
    h->addWidget(zoomButton_);
    h->addWidget(in);
    h->addStretch(1);
    h->addWidget(info_, 1);
    h->addSpacing(10);
    h->addWidget(version_);

    auto *outer = new QVBoxLayout(toolbar_);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(row, 1);
    outer->addWidget(line);

    connect(fit, &QToolButton::clicked, this, [this] { zoomToFit(); });
    connect(actual, &QToolButton::clicked, this, [this] { zoomToActual(); });
    connect(out, &QToolButton::clicked, this, [this] { zoomStep(-1); });
    connect(in, &QToolButton::clicked, this, [this] { zoomStep(+1); });
    connect(miFit, &QAction::triggered, this, [this] { zoomToFit(); });
    connect(miActual, &QAction::triggered, this, [this] { zoomToActual(); });
    int pct = 25;
    for (QAction *a : presets) {
        const int p = pct;
        connect(a, &QAction::triggered, this, [this, p] { zoomToPreset(p); });
        pct *= 2;
    }
}

IVView::Mode IVView::modeForFlags(int flags) {
    if (!(flags & lcp_fittowindow)) return Actual;
    return (flags & lcp_fitlargeronly) ? FitLarge : Fit;
}

QSize IVView::canvasViewportSize() const {
    return canvas_->viewport()->size();
}

qreal IVView::fitMagnification() const {
    if (!hasImage_ || probe_.displaySize.isEmpty()) return 1;
    const QSize vp = canvasViewportSize();
    if (vp.width() < 1 || vp.height() < 1) return 1;
    return qMin(double(vp.width()) / probe_.displaySize.width(),
                double(vp.height()) / probe_.displaySize.height());
}

void IVView::setMagnification(qreal mag, const QPoint &anchor) {
    const qreal fit = fitMagnification();
    const qreal minMag = qMax(0.005, qMin(fit, 1.0) / 2);
    const qreal maxMag = qMax(kMaxMagnification, fit * 2);
    mag = qBound(minMag, mag, maxMag);
    canvas_->setMagnificationAt(mag, anchor);
    canvas_->setRenderHint(QPainter::SmoothPixmapTransform, mag < 2.0);
    if (canvas_->pixmapItem())
        canvas_->pixmapItem()->setTransformationMode(mag >= 2.0 ? Qt::FastTransformation
                                                                : Qt::SmoothTransformation);
    canvas_->updateCursor();
    updateZoomLabel();
}

void IVView::applyMode() {
    if (!hasImage_) return;
    const QPoint center = canvas_->viewportCenter();
    switch (mode_) {
    case Fit:      setMagnification(fitMagnification(), center); break;
    case FitLarge: setMagnification(qMin(1.0, fitMagnification()), center); break;
    case Actual:   setMagnification(1.0, center); break;
    case Manual:   return;
    }
    /* Centered, whether or not the image fits. */
    canvas_->centerOn(canvas_->sceneRect().center());
    canvas_->updateCursor();
}

void IVView::zoomStep(int direction) {
    if (!hasImage_) return;
    static const double steps[] = {0.01, 0.02, 0.03, 0.05, 0.08, 0.1, 0.125, 0.167,
                                   0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.25, 1.5, 2, 3,
                                   4, 6, 8, 12, 16, 24, 32, 48, 64};
    const qreal cur = canvas_->magnification();
    qreal target = cur;
    const int n = int(sizeof steps / sizeof steps[0]);
    if (direction > 0) {
        target = cur * 2;
        for (int i = 0; i < n; ++i) if (steps[i] > cur * 1.01) { target = steps[i]; break; }
    } else {
        target = cur / 2;
        for (int i = n; i-- > 0;) if (steps[i] < cur / 1.01) { target = steps[i]; break; }
    }
    mode_ = Manual;
    setMagnification(target, canvas_->viewportCenter());
}

void IVView::zoomToFit() {
    if (!hasImage_) return;
    mode_ = Fit;
    applyMode();
}

void IVView::zoomToActual() {
    if (!hasImage_) return;
    mode_ = Actual;
    setMagnification(1.0, canvas_->viewportCenter());
}

void IVView::zoomToPreset(int percent) {
    if (!hasImage_) return;
    mode_ = Manual;
    setMagnification(percent / 100.0, canvas_->viewportCenter());
}

void IVView::toggleFitAt(const QPointF &scenePos) {
    if (!hasImage_) return;
    const qreal fit = fitMagnification();
    const qreal cur = canvas_->magnification();
    if (qAbs(cur - fit) < 0.005 && qAbs(fit - 1.0) > 0.005) {
        mode_ = Actual;
        const QPoint anchor = canvas_->mapFromScene(scenePos);
        setMagnification(1.0, anchor);
    } else {
        mode_ = Fit;
        applyMode();
    }
}

void IVView::onUserZoom() {
    updateZoomLabel();
}

void IVView::updateZoomLabel() {
    if (!zoomButton_) return;
    const qreal mag = hasImage_ ? canvas_->magnification() : 1.0;
    const QString text = (mag >= 0.995 || mag < 0.1)
        ? QStringLiteral("%1%").arg(qRound(mag * 100))
        : QStringLiteral("%1%").arg(qRound(mag * 1000) / 10.0, 0, 'f', 1);
    zoomButton_->setText(text);
}

void IVView::onCanvasResized() {
    layoutOverlay();
    if (!hasImage_) return;
    if (mode_ == Fit || mode_ == FitLarge) { applyMode(); return; }
    canvas_->centerOn(canvas_->sceneRect().center());
    canvas_->updateCursor();
    updateZoomLabel();
}

void IVView::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    layoutOverlay();
}

void IVView::layoutOverlay() {
    if (!busy_ || !canvas_) return;
    const QRect r = canvas_->geometry();
    busy_->move(r.center() - busy_->rect().center());
    message_->setGeometry(r.adjusted(24, 24, -24, -24));
}

void IVView::showBusy(bool on) {
    busy_->setVisible(on);
    if (on) busy_->raise();
}

void IVView::showMessage(const QString &text) {
    message_->setText(text);
    message_->setVisible(!text.isEmpty());
    if (!text.isEmpty()) message_->raise();
}

QString IVView::infoText() const {
    if (!probe_.ok) return QString();
    QStringList parts;
    parts << formatDims(probe_.pixelSize, probe_.kind == Kind::Vector);
    parts << probe_.format;
    const int frames = currentFrameCount();
    if (frames > 1) parts << tr("%1 frames").arg(frames);
    parts << formatBytes(probe_.fileSize);
    return parts.join(QStringLiteral(" · "));
}

int IVView::currentFrameCount() const {
    if (movie_ && movie_->isValid() && movie_->frameCount() > 0) return movie_->frameCount();
    return probe_.frameCount;
}

void IVView::updateInfo() {
    const QString text = infoText();
    info_->setText(text);
    info_->setToolTip(QFileInfo(probe_.path).fileName() + QLatin1Char('\n') + text);
}

void IVView::stopAnimation() {
    if (movie_) {
        movie_->stop();
        movie_->disconnect();
        movie_->deleteLater();
        movie_ = nullptr;
    }
}

void IVView::startAnimation() {
    stopAnimation();
    movie_ = new QMovie(probe_.path, QByteArray(), this);
    if (!movie_->isValid()) {
        delete movie_;
        movie_ = nullptr;
        return;
    }
    if (probe_.displaySize != probe_.pixelSize) movie_->setScaledSize(probe_.displaySize);
    movie_->setCacheMode(QMovie::CacheNone);
    connect(movie_, &QMovie::frameChanged, this, [this](int) {
        if (!movie_) return;
        const QPixmap pm = movie_->currentPixmap();
        if (pm.isNull()) return;
        canvas_->setPixmap(pm);
    });
    movie_->jumpToFrame(0);
    const QPixmap first = movie_->currentPixmap();
    if (!first.isNull()) {
        canvas_->setPixmap(first);
        canvas_->setCheckerboard(first.hasAlphaChannel());
        displayImage_ = first.toImage();
    }
    movie_->start();
}

void IVView::startDecode(quint64 token) {
    if (watcher_) { watcher_->disconnect(); watcher_->deleteLater(); watcher_ = nullptr; }
    auto *watch = new QFutureWatcher<QImage>(this);
    watcher_ = watch;
    connect(watch, &QFutureWatcher<QImage>::finished, this, [this, watch, token] {
        if (token != generation_) { watch->deleteLater(); return; }
        showBusy(false);
        const QImage img = watch->result();
        watch->deleteLater();
        if (watcher_ == watch) watcher_ = nullptr;
        if (img.isNull()) {
            showMessage(tr("This image could not be decoded."));
            return;
        }
        displayImage_ = img;
        canvas_->setPixmap(QPixmap::fromImage(img));
        canvas_->setCheckerboard(img.hasAlphaChannel());
        updateInfo();
    });

    const Probe p = probe_;
    busyTimer_->disconnect();
    connect(busyTimer_, &QTimer::timeout, this, [this, token] {
        if (token == generation_ && displayImage_.isNull() && !movie_) showBusy(true);
    });
    busyTimer_->start(250);

    watch->setFuture(QtConcurrent::run([p] { return decodeRaster(p); }));
}

bool IVView::showFile(const Probe &p, int flags) {
    if (!p.ok) return false;
    ++generation_;
    busyTimer_->stop();
    showBusy(false);
    showMessage(QString());
    stopAnimation();
    if (watcher_) { watcher_->disconnect(); watcher_->deleteLater(); watcher_ = nullptr; }

    probe_ = p;
    hasImage_ = true;
    displayImage_ = QImage();
    mode_ = modeForFlags(flags);

    canvas_->clearImage();
    canvas_->setImageSize(p.displaySize);

    if (p.kind == Kind::Vector) {
        canvas_->setSvg(p.vectorData, p.displaySize);
        canvas_->setCheckerboard(true);
        updateInfo();
    } else {
        updateInfo();
        if (p.animated) {
            startAnimation();
            if (!movie_) startDecode(generation_);   /* QMovie refused it: static frame */
        } else {
            startDecode(generation_);
        }
    }

    applyMode();
    updateZoomLabel();
    layoutOverlay();
    return true;
}

void IVView::applyNewParams(int flags) {
    mode_ = modeForFlags(flags);
    applyMode();
}

QImage IVView::renderVector(double scale) {
    FpGuard fp;
    QSvgRenderer renderer(probe_.vectorData);
    if (!renderer.isValid()) return QImage();
    const QSizeF base(probe_.pixelSize);
    const double cap = qMin(kMaxDisplaySide / qMax(base.width(), base.height()),
                            std::sqrt(kMaxDisplayPixels / (base.width() * base.height())));
    scale = qMax(0.01, qMin(scale, cap));
    const QSize px(qMax(1, qRound(base.width() * scale)),
                   qMax(1, qRound(base.height() * scale)));
    QImage img(px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(px)));
    painter.end();
    return img;
}

static QByteArray encodeImage(const QImage &img, const char *format) {
    QByteArray out;
    QBuffer buf(&out);
    if (!buf.open(QIODevice::WriteOnly)) return QByteArray();
    if (!img.save(&buf, format)) return QByteArray();
    return out;
}

int IVView::copyImage() {
    if (!hasImage_) return LISTPLUGIN_ERROR;

    QImage img = displayImage_;
    if (probe_.kind == Kind::Vector) img = renderVector(2.0);
    if (img.isNull() && movie_) img = movie_->currentPixmap().toImage();
    if (img.isNull() && probe_.kind == Kind::Raster) {
        /* The display may have been capped; copy the real thing when it is sane. */
        const double mp = double(probe_.pixelSize.width()) * probe_.pixelSize.height();
        img = (mp > 512.0 * 1024 * 1024) ? displayImage_ : decodeFull(probe_);
    }
    if (img.isNull()) return LISTPLUGIN_ERROR;

    auto *mime = new QMimeData;
    mime->setImageData(img);

    const QByteArray png = encodeImage(img, "PNG");
    if (!png.isEmpty()) mime->setData(QStringLiteral("image/png"), png);

    /* TIFF only while it stays a sane size (an uncompressed 64 MP bitmap is
       too big for a pasteboard). */
    if (qint64(img.width()) * img.height() <= 16 * 1024 * 1024 &&
        QImageWriter::supportedImageFormats().contains(QByteArray("tiff"))) {
        const QByteArray tiff = encodeImage(img, "TIFF");
        if (!tiff.isEmpty()) mime->setData(QStringLiteral("image/tiff"), tiff);
    }

    /* Where the file itself is what a paste wants — SVG source, an animation,
       a camera format's original bytes — offer it under its own type too. */
    QString mimeType;
    if (probe_.kind == Kind::Vector) {
        mimeType = QStringLiteral("image/svg+xml");
        mime->setData(mimeType, probe_.vectorData);
    } else {
        static const struct { const char *fmt; const char *mime; } kMimes[] = {
            {"JPEG", "image/jpeg"}, {"GIF", "image/gif"}, {"WEBP", "image/webp"},
            {"AVIF", "image/avif"}, {"HEIC", "image/heic"}, {"HEIF", "image/heic"},
            {"JXL", "image/jxl"}, {"PNG", "image/png"},
        };
        for (const auto &m : kMimes) {
            if (probe_.format == QLatin1String(m.fmt)) { mimeType = QString::fromLatin1(m.mime); break; }
        }
        if (!mimeType.isEmpty()) {
            QFile f(probe_.path);
            if (f.open(QIODevice::ReadOnly) && f.size() > 0 && f.size() <= 256 * 1024 * 1024)
                mime->setData(mimeType, f.readAll());
        }
    }

    QGuiApplication::clipboard()->setMimeData(mime);
    return LISTPLUGIN_OK;
}

/* ---- WLX exported API --------------------------------------------------- */

static QString fromNative(const char *s) {
    return s ? QString::fromUtf8(s) : QString();
}

static QString fromWide(const char16_t *s) {
    if (!s) return QString();
    size_t n = 0;
    while (s[n]) ++n;
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(s), int(n));
}

static IVView *makeAndLoad(HWND parentWin, const QString &path, int flags) {
    const Probe probe = probeFile(path);
    if (!probe.ok) return nullptr;
    auto *parent = reinterpret_cast<QWidget *>(parentWin);
    auto *view = new IVView(parent);
    if (!view->showFile(probe, flags)) {
        delete view;
        return nullptr;
    }
    view->show();
    return view;
}

static IVView *asView(HWND h) {
    return reinterpret_cast<IVView *>(h);
}

extern "C" {

__attribute__((visibility("default")))
HWND DCPCALL ListLoadW(HWND ParentWin, char16_t *FileToLoad, int ShowFlags) {
    IVView *v = makeAndLoad(ParentWin, fromWide(FileToLoad), ShowFlags);
    return v ? reinterpret_cast<HWND>(v) : 0;
}

__attribute__((visibility("default")))
HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags) {
    IVView *v = makeAndLoad(ParentWin, fromNative(FileToLoad), ShowFlags);
    return v ? reinterpret_cast<HWND>(v) : 0;
}

__attribute__((visibility("default")))
int DCPCALL ListLoadNextW(HWND ParentWin, HWND PluginWin, char16_t *FileToLoad, int ShowFlags) {
    (void)ParentWin;
    IVView *v = asView(PluginWin);
    if (!v) return LISTPLUGIN_ERROR;
    const Probe probe = probeFile(fromWide(FileToLoad));
    if (!probe.ok) return LISTPLUGIN_ERROR;
    return v->showFile(probe, ShowFlags) ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
int DCPCALL ListLoadNext(HWND ParentWin, HWND PluginWin, char *FileToLoad, int ShowFlags) {
    (void)ParentWin;
    IVView *v = asView(PluginWin);
    if (!v) return LISTPLUGIN_ERROR;
    const Probe probe = probeFile(fromNative(FileToLoad));
    if (!probe.ok) return LISTPLUGIN_ERROR;
    return v->showFile(probe, ShowFlags) ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
void DCPCALL ListCloseWindow(HWND ListWin) {
    if (!ListWin) return;
    delete asView(ListWin);
}

__attribute__((visibility("default")))
int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
    IVView *v = asView(ListWin);
    if (!v) return LISTPLUGIN_ERROR;
    if (Command == lc_copy) return v->copyImage();
    if (Command == lc_newparams) { v->applyNewParams(Parameter); return LISTPLUGIN_OK; }
    return LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
void DCPCALL ListGetDetectString(char *DetectString, int maxlen) {
    if (!DetectString || maxlen <= 0) return;
    static const QString detect = buildDetectString();
    std::snprintf(DetectString, size_t(maxlen), "%s", detect.toUtf8().constData());
}

__attribute__((visibility("default")))
void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *dps) {
    (void)dps;
}

} /* extern "C" */
