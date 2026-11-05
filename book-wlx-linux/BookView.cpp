/*
 * BookView.wlx — Double Commander Lister (WLX) plugin for Linux.
 *
 * Reads EPUB 2/3 and FictionBook (FB2/FBZ) e-books in DC's F3 viewer: contents
 * sidebar, one continuous reflowable column, cover and metadata, light/dark/
 * sepia themes, session reading position and text size.
 *
 * Linux port of ../_mac_repo/book-wlx. The book is served to a QWebEngineView from a
 * loopback HTTP server with a per-load token:
 *
 *   http://127.0.0.1:<port>/<token>/__dcreader__/index.html   reader shell
 *   http://127.0.0.1:<port>/<token>/__dcreader__/reader.js    reader + DOMPurify
 *   http://127.0.0.1:<port>/<token>/OEBPS/chapter1.xhtml      an EPUB entry
 *
 * A custom URL scheme would be the natural fit, but QtWebEngine requires
 * QWebEngineUrlScheme::registerScheme() before QApplication is created and a
 * plugin is loaded into an already-running host. A loopback-only server bound
 * to 127.0.0.1 with an unguessable token gives the page a normal same-origin
 * URL (so fetch/relative hrefs/fonts work unchanged) without exposing anything
 * beyond this process. The page's CSP forbids everything except that origin and
 * data: images, so a book still cannot reach the network.
 *
 * The document model (EPUB OPF/spine/nav/NCX and FB2 structures) lives in
 * assets/reader.js, where DOMParser handles real-world markup and DOMPurify
 * sanitises EPUB chapters. zipreader.c (shared with the macOS plugin) reads the
 * container and inflates entries with zlib.
 */

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QByteArray>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QSet>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWebEngineFindTextResult>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QWidget>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <functional>
#include <memory>

#include "listplug.h"

extern "C" {
#include "zipreader.h"
}

#define BV_VERSION "0.1.0"
#define READER_PREFIX "__dcreader__"

/* ---- Helpers ------------------------------------------------------------ */

static QString pluginDirectory() {
    Dl_info info;
    if (dladdr(reinterpret_cast<const void *>(&pluginDirectory), &info) && info.dli_fname)
        return QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath();
    return QString();
}

static QString assetDirectory() {
    const QString dir = pluginDirectory();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/assets");
}

static QString gIniPath;          /* set by ListSetDefaultParams */
static int gFontSizeOverride = 0; /* last A+ / A- choice, session-wide */

static QString configIniPath() {
    if (!gIniPath.isEmpty()) return gIniPath;
    const QString dir = pluginDirectory();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/BookView.ini");
}

static bool cfgBool(const QSettings &cfg, const QString &key, bool def) {
    const QString v = cfg.value(QStringLiteral("BookView/") + key,
                                def ? QStringLiteral("1") : QStringLiteral("0"))
                          .toString().toLower();
    return v == QLatin1String("1") || v == QLatin1String("true") ||
           v == QLatin1String("yes") || v == QLatin1String("on");
}

static QString htmlEscape(QString s) {
    s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return s;
}

static QByteArray mimeForPath(const QString &path) {
    static const QHash<QString, QByteArray> map = {
        {"xhtml", "application/xhtml+xml"}, {"html", "text/html"},
        {"htm", "text/html"}, {"xml", "application/xml"},
        {"opf", "application/oebps-package+xml"}, {"ncx", "application/x-dtbncx+xml"},
        {"css", "text/css"}, {"js", "text/javascript"},
        {"json", "application/json"}, {"txt", "text/plain"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"},
        {"gif", "image/gif"}, {"svg", "image/svg+xml"}, {"webp", "image/webp"},
        {"bmp", "image/bmp"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
        {"avif", "image/avif"}, {"otf", "font/otf"}, {"ttf", "font/ttf"},
        {"woff", "font/woff"}, {"woff2", "font/woff2"},
        {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"ogg", "audio/ogg"},
        {"mp4", "video/mp4"}, {"webm", "video/webm"}, {"smil", "application/smil+xml"},
    };
    return map.value(QFileInfo(path).suffix().toLower(),
                     QByteArrayLiteral("application/octet-stream"));
}

static bool looksLikeFB2(const QByteArray &data) {
    return data.left(4096).contains("FictionBook");
}

/* Entry path safety: only exact names from the ZIP are ever looked up, but a
 * path that tries to walk out is rejected before it reaches the reader. */
static bool safeEntryPath(const QString &p) {
    if (p.isEmpty() || p.startsWith(QLatin1Char('/')) || p.contains(QLatin1Char('\\')))
        return false;
    const QStringList parts = p.split(QLatin1Char('/'));
    for (const QString &part : parts)
        if (part == QLatin1String("..") || part == QLatin1String(".")) return false;
    return true;
}

struct ZipHolder {
    ZipArchive *zip = nullptr;
    ~ZipHolder() { if (zip) ZipClose(zip); }
};

/* ---- Loopback book server ----------------------------------------------- */

class BookServer {
public:
    using MessageFn = std::function<void(const QJsonObject &)>;

    BookServer() = default;
    ~BookServer() { stop(); }

    bool start() {
        if (server_) return true;
        server_ = new QTcpServer();
        QObject::connect(server_, &QTcpServer::newConnection, server_, [this] { accept(); });
        if (!server_->listen(QHostAddress::LocalHost, 0)) {
            delete server_;
            server_ = nullptr;
            return false;
        }
        port_ = server_->serverPort();
        return true;
    }

    void stop() {
        const auto socks = sockets_;
        for (QTcpSocket *s : socks) s->abort();
        sockets_.clear();
        buffers_.clear();
        if (server_) { server_->close(); delete server_; server_ = nullptr; }
        port_ = 0;
        zip = nullptr;
        hasFb2 = false;
    }

    quint16 port() const { return port_; }

    QString token;
    QString shell;
    QByteArray fb2;
    bool hasFb2 = false;
    ZipArchive *zip = nullptr;
    QString assetDir;
    MessageFn onMessage;

private:
    void accept() {
        while (server_->hasPendingConnections()) {
            QTcpSocket *s = server_->nextPendingConnection();
            sockets_.insert(s);
            QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] { read(s); });
            QObject::connect(s, &QTcpSocket::disconnected, s, [this, s] {
                buffers_.remove(s);
                sockets_.remove(s);
                s->deleteLater();
            });
            QTimer::singleShot(30000, s, [s] { s->disconnectFromHost(); });
        }
    }

    static QByteArray reasonPhrase(int code) {
        switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default:  return "Error";
        }
    }

    void reply(QTcpSocket *s, int code, const QByteArray &type, const QByteArray &body) {
        QByteArray h = "HTTP/1.1 " + QByteArray::number(code) + ' ' + reasonPhrase(code) + "\r\n";
        h += "Content-Type: " + type + "\r\n";
        h += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        h += "Cache-Control: no-store\r\n";
        h += "X-Content-Type-Options: nosniff\r\n";
        h += "Connection: close\r\n\r\n";
        s->write(h);
        if (!body.isEmpty()) s->write(body);
        s->disconnectFromHost();
    }

    void read(QTcpSocket *s) {
        QByteArray &buf = buffers_[s];
        buf += s->readAll();
        if (buf.size() > 2 * 1024 * 1024) {
            reply(s, 413, "text/plain", "request too large");
            return;
        }
        const int hdrEnd = buf.indexOf("\r\n\r\n");
        if (hdrEnd < 0) return;

        const QByteArray head = buf.left(hdrEnd);
        const int lineEnd = head.indexOf("\r\n");
        const QByteArray requestLine = lineEnd < 0 ? head : head.left(lineEnd);
        const QList<QByteArray> parts = requestLine.split(' ');
        if (parts.size() < 2) { reply(s, 400, "text/plain", "bad request"); return; }

        int contentLength = 0;
        const QList<QByteArray> headerLines = head.mid(lineEnd + 2).split('\n');
        for (QByteArray line : headerLines) {
            line = line.trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0) continue;
            if (line.left(colon).toLower() == "content-length")
                contentLength = line.mid(colon + 1).trimmed().toInt();
        }
        if (contentLength < 0 || contentLength > 1024 * 1024) {
            reply(s, 413, "text/plain", "request too large");
            return;
        }
        const qint64 total = hdrEnd + 4 + contentLength;
        if (buf.size() < total) return;       /* wait for the body */

        const QByteArray body = buf.mid(hdrEnd + 4, contentLength);
        buf.clear();
        handle(s, parts[0], parts[1], body);
    }

    void handle(QTcpSocket *s, const QByteArray &method, const QByteArray &rawTarget,
                const QByteArray &body) {
        QString target = QString::fromLatin1(rawTarget);
        const int query = target.indexOf(QLatin1Char('?'));
        if (query >= 0) target.truncate(query);
        if (!target.startsWith(QLatin1Char('/'))) { reply(s, 404, "text/plain", "not found"); return; }

        const QString prefix = token + QLatin1Char('/');
        if (!target.startsWith(QLatin1Char('/') + prefix)) {
            reply(s, 404, "text/plain", "not found");
            return;
        }
        const QString rest = target.mid(prefix.size() + 1);

        if (method == "POST" && rest == QLatin1String(READER_PREFIX "/message")) {
            const QJsonObject msg = QJsonDocument::fromJson(body).object();
            if (onMessage) onMessage(msg);
            reply(s, 204, "text/plain", QByteArray());
            return;
        }
        if (method != "GET") { reply(s, 405, "text/plain", "method not allowed"); return; }

        if (rest == QLatin1String(READER_PREFIX "/index.html")) {
            reply(s, 200, "text/html; charset=utf-8", shell.toUtf8());
            return;
        }
        if (rest == QLatin1String(READER_PREFIX "/document.fb2")) {
            if (!hasFb2) { reply(s, 404, "text/plain", "not found"); return; }
            reply(s, 200, "application/octet-stream", fb2);
            return;
        }
        if (rest.startsWith(QLatin1String(READER_PREFIX "/"))) {
            const QString name = rest.mid(int(strlen(READER_PREFIX)) + 1);
            static const QStringList allowed = {
                QStringLiteral("reader.css"), QStringLiteral("reader.js"),
                QStringLiteral("dompurify.min.js")
            };
            if (!allowed.contains(name)) { reply(s, 404, "text/plain", "not found"); return; }
            QFile f(assetDir + QLatin1Char('/') + name);
            if (!f.open(QIODevice::ReadOnly)) { reply(s, 404, "text/plain", "not found"); return; }
            reply(s, 200, mimeForPath(name), f.readAll());
            return;
        }

        if (!zip || !safeEntryPath(rest)) { reply(s, 404, "text/plain", "not found"); return; }
        const QString entry = QUrl::fromPercentEncoding(rest.toUtf8());
        if (!safeEntryPath(entry)) {
            reply(s, 404, "text/plain", "not found");
            return;
        }
        const QByteArray entryBytes = entry.toUtf8();
        size_t len = 0;
        unsigned char *bytes = ZipCopyEntry(zip, entryBytes.constData(), &len);
        if (!bytes) { reply(s, 404, "text/plain", "not found"); return; }
        if (len > 256u * 1024 * 1024) {
            free(bytes);
            reply(s, 413, "text/plain", "entry too large");
            return;
        }
        const QByteArray data(reinterpret_cast<const char *>(bytes), int(len));
        free(bytes);
        reply(s, 200, mimeForPath(entry), data);
    }

    QTcpServer *server_ = nullptr;
    quint16 port_ = 0;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QSet<QTcpSocket *> sockets_;
};

/* ---- BookView ----------------------------------------------------------- */

class BookView : public QWidget {
public:
    explicit BookView(QWidget *parent);
    ~BookView() override;

    bool loadBook(const QString &path);
    int sendCommand(int command);
    void findText(const QString &text, int flags);
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct SearchJob { QString text; int flags; };

    QString buildShell(const QString &base, const QString &format, const QString &fileName,
                       const QString &errorReason) const;
    void serverMessage(const QJsonObject &msg);
    void runSearches();
    void dispatchReaderKey(const QString &key);
    void scrollBy(int dy);
    void forwardKey(QKeyEvent *event);
    int currentFontSize(const QSettings &cfg) const;

    QWebEngineView *web_ = nullptr;
    BookServer server_;
    std::unique_ptr<ZipHolder> zip_;
    QByteArray fb2_;
    QString currentPath_;
    QString token_;
    QString readyToken_;
    QList<SearchJob> searches_;
    bool searching_ = false;
    int loadCounter_ = 0;
    QHash<QString, QPair<int, double>> positions_;
};

BookView::BookView(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);

    web_ = new QWebEngineView(this);
    web_->setFocusPolicy(Qt::NoFocus);
    web_->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    web_->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(web_);

    /* Keep keyboard focus on this widget, never on the page: Double Commander's
     * viewer hotkeys (Esc/Q, N/P, Alt+arrows, the mode digits) are LCL-level and
     * a focused QWebEngineView would swallow them. The keys the reader needs are
     * driven from here (scroll through JS, t/+/- as synthetic events) and
     * everything else is forwarded to DC. */
    server_.assetDir = assetDirectory();
    server_.onMessage = [this](const QJsonObject &m) { serverMessage(m); };
}

BookView::~BookView() {
    server_.stop();
}

int BookView::currentFontSize(const QSettings &cfg) const {
    if (gFontSizeOverride > 0) return gFontSizeOverride;
    return qMax(10, cfg.value(QStringLiteral("BookView/fontsize"), 18).toInt());
}

QString BookView::buildShell(const QString &base, const QString &format,
                             const QString &fileName, const QString &errorReason) const {
    const QSettings cfg(configIniPath(), QSettings::IniFormat);
    QString theme = cfg.value(QStringLiteral("BookView/theme"), QStringLiteral("auto"))
                        .toString().toLower();
    if (theme != QLatin1String("light") && theme != QLatin1String("dark") &&
        theme != QLatin1String("sepia"))
        theme = QStringLiteral("auto");
    const int fontSize = currentFontSize(cfg);
    const int maxWidth = qMax(320, cfg.value(QStringLiteral("BookView/maxwidth"), 680).toInt());
    double lineHeight = cfg.value(QStringLiteral("BookView/lineheight"), 1.7).toDouble();
    if (lineHeight < 1.1 || lineHeight > 2.6) lineHeight = 1.7;
    const bool justify = cfgBool(cfg, QStringLiteral("justify"), false);
    const bool publisherCss = cfgBool(cfg, QStringLiteral("publishercss"), false);
    const bool toc = cfgBool(cfg, QStringLiteral("toc"), true);
    const bool showVersion = cfgBool(cfg, QStringLiteral("showversion"), true);

    QJsonObject position;
    if (positions_.contains(currentPath_)) {
        position.insert(QStringLiteral("i"), positions_.value(currentPath_).first);
        position.insert(QStringLiteral("r"), positions_.value(currentPath_).second);
    }

    QJsonObject js;
    js.insert(QStringLiteral("base"), base);
    js.insert(QStringLiteral("reader"), base + QStringLiteral(READER_PREFIX "/"));
    js.insert(QStringLiteral("theme"), theme);
    js.insert(QStringLiteral("fontSize"), fontSize);
    js.insert(QStringLiteral("maxWidth"), maxWidth);
    js.insert(QStringLiteral("lineHeight"), lineHeight);
    js.insert(QStringLiteral("justify"), justify);
    js.insert(QStringLiteral("publisherCSS"), publisherCss);
    js.insert(QStringLiteral("tocOpen"), toc);
    js.insert(QStringLiteral("showVersion"), showVersion);
    js.insert(QStringLiteral("version"), QStringLiteral(BV_VERSION));
    js.insert(QStringLiteral("token"), token_);
    js.insert(QStringLiteral("fileName"), fileName);
    js.insert(QStringLiteral("position"), position);
    js.insert(QStringLiteral("format"), format);
    QString json = QString::fromUtf8(QJsonDocument(js).toJson(QJsonDocument::Compact));
    json.replace(QLatin1String("</"), QLatin1String("<\\/"));

    if (!errorReason.isEmpty()) {
        return QStringLiteral(
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<meta http-equiv=\"Content-Security-Policy\" "
            "content=\"default-src 'none'; style-src 'unsafe-inline'\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<style>html,body{margin:0;height:100%;display:flex;align-items:center;"
            "justify-content:center;background:#fbfaf7;color:#3a352e;"
            "font:14px/1.6 sans-serif;}@media(prefers-color-scheme:dark){html,body{"
            "background:#16181c;color:#c9cdd4;}}.box{max-width:480px;padding:28px;"
            "text-align:center;}.t{font-size:16px;font-weight:600;margin-bottom:8px;}"
            ".f{opacity:.6;font-size:12px;margin-top:14px;word-break:break-all;}</style>"
            "</head><body><div class=\"box\"><div class=\"t\">Can't open this book</div>"
            "<div>%1</div><div class=\"f\">%2</div></div></body></html>")
            .arg(htmlEscape(errorReason), htmlEscape(fileName));
    }

    const bool tocOpen = toc && width() > 900;
    const QString csp =
        QStringLiteral("default-src 'none'; script-src 'self'; "
                       "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                       "font-src 'self' data:; media-src 'self' data:; "
                       "connect-src 'self'; base-uri 'none'; form-action 'none'; "
                       "frame-src 'none'");

    return QStringLiteral(
        "<!DOCTYPE html><html data-theme=\"%1\"%2><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"%3\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>%4</title>"
        "<link rel=\"stylesheet\" href=\"reader.css\">"
        "<script id=\"dc-config\" type=\"application/json\">%5</script>"
        "</head><body>"
        "<div id=\"progress\"><div id=\"progress-bar\"></div></div>"
        "<header id=\"bar\">"
        "<button id=\"toc-toggle\" type=\"button\" title=\"Contents (t)\" "
        "aria-label=\"Contents\">"
        "<svg viewBox=\"0 0 16 16\" width=\"15\" height=\"15\" aria-hidden=\"true\">"
        "<rect x=\"1\" y=\"2.5\" width=\"14\" height=\"1.6\" rx=\".8\"/>"
        "<rect x=\"1\" y=\"7.2\" width=\"14\" height=\"1.6\" rx=\".8\"/>"
        "<rect x=\"1\" y=\"11.9\" width=\"14\" height=\"1.6\" rx=\".8\"/></svg>"
        "</button>"
        "<div id=\"bar-title\"><span id=\"book-title\"></span>"
        "<span id=\"book-author\"></span></div>"
        "<div id=\"bar-tools\">"
        "<button id=\"font-down\" type=\"button\" title=\"Smaller text (-)\" "
        "data-label=\"A&#8722;\"></button>"
        "<button id=\"font-up\" type=\"button\" title=\"Larger text (+)\" "
        "data-label=\"A+\"></button>"
        "<span id=\"percent\"></span>"
        "</div>"
        "</header>"
        "<nav id=\"toc\" aria-label=\"Table of contents\">"
        "<div id=\"toc-head\" data-label=\"Contents\"></div>"
        "<ol id=\"toc-list\"></ol></nav>"
        "<div id=\"scrim\"></div>"
        "<main id=\"book\"><div id=\"status\">Opening book…</div></main>"
        "<script src=\"dompurify.min.js\"></script>"
        "<script src=\"reader.js\"></script>"
        "</body></html>")
        .arg(theme, tocOpen ? QStringLiteral(" class=\"toc-open\"") : QString(),
             csp, htmlEscape(fileName), json);
}

bool BookView::loadBook(const QString &path) {
    server_.stop();
    zip_.reset();
    fb2_.clear();
    ++loadCounter_;
    token_ = QStringLiteral("b%1%2")
                 .arg(loadCounter_)
                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')));
    readyToken_.clear();
    searches_.clear();
    searching_ = false;
    currentPath_ = path;

    bool isBook = false;
    QString format, reason;

    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.exists() || !fi.isFile()) {
        reason = path.isEmpty() ? QStringLiteral("No file name was given.")
                                : QStringLiteral("The file could not be read.");
    } else {
        ZipArchive *z = ZipOpen(QFile::encodeName(path).constData());
        if (z) {
            if (ZipHasEntry(z, "META-INF/container.xml")) {
                format = QStringLiteral("epub");
                zip_.reset(new ZipHolder{z});
                isBook = true;
            } else {
                const char *fb2Entry = nullptr;
                for (size_t i = 0; i < ZipEntryCount(z) && !fb2Entry; ++i) {
                    const char *name = ZipEntryName(z, i);
                    if (!name) continue;
                    const size_t len = strlen(name);
                    if (len >= 4 && strcasecmp(name + len - 4, ".fb2") == 0) fb2Entry = name;
                }
                if (fb2Entry) {
                    size_t len = 0;
                    unsigned char *bytes = ZipCopyEntry(z, fb2Entry, &len);
                    if (bytes && len <= 256u * 1024 * 1024) {
                        fb2_ = QByteArray(reinterpret_cast<const char *>(bytes), int(len));
                        free(bytes);
                        format = QStringLiteral("fb2");
                        isBook = true;
                    } else {
                        free(bytes);
                        reason = QStringLiteral(
                            "The FictionBook file inside this archive could not be decompressed.");
                    }
                } else {
                    reason = QStringLiteral("This ZIP is neither an EPUB "
                                            "(no META-INF/container.xml) nor a zipped FictionBook.");
                }
                ZipClose(z);
            }
        } else if (fi.size() > 0 && fi.size() <= 256ll * 1024 * 1024) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray raw = f.readAll();
                if (looksLikeFB2(raw)) {
                    fb2_ = raw;
                    format = QStringLiteral("fb2");
                    isBook = true;
                } else {
                    reason = QStringLiteral("This is neither an EPUB container nor a "
                                            "FictionBook document.");
                }
            } else {
                reason = QStringLiteral("The file could not be read.");
            }
        } else {
            reason = QStringLiteral("The file could not be read.");
        }
    }

    if (!server_.start()) {
        /* No loopback listener: show the reason without a book page. */
        readyToken_ = token_;
        server_.stop();
        web_->setHtml(buildShell(QString(), QStringLiteral("error"), fi.fileName(), reason));
        return false;
    }

    const QString base = QStringLiteral("http://127.0.0.1:%1/%2/")
                             .arg(server_.port()).arg(token_);
    server_.token = token_;
    server_.zip = zip_ ? zip_->zip : nullptr;
    server_.fb2 = fb2_;
    server_.hasFb2 = !fb2_.isEmpty();
    server_.shell = buildShell(base, isBook ? format : QStringLiteral("error"),
                               fi.fileName(), isBook ? QString() : reason);
    if (!isBook) readyToken_ = token_;   /* the error page answers searches at once */

    const QString loadToken = token_;
    QTimer::singleShot(20000, this, [this, loadToken] {
        if (loadToken == token_ && readyToken_ != token_) {
            readyToken_ = token_;
            runSearches();
        }
    });

    web_->load(QUrl(base + QLatin1String(READER_PREFIX "/index.html")));
    return isBook;
}

void BookView::serverMessage(const QJsonObject &msg) {
    if (msg.value(QStringLiteral("k")).toString() != token_) return;
    const QString type = msg.value(QStringLiteral("t")).toString();
    if (type == QLatin1String("ready")) {
        readyToken_ = token_;
        runSearches();
    } else if (type == QLatin1String("pos")) {
        const int i = msg.value(QStringLiteral("i")).toInt();
        const double r = msg.value(QStringLiteral("r")).toDouble();
        positions_.insert(currentPath_, qMakePair(i, r));
    } else if (type == QLatin1String("font")) {
        const int size = msg.value(QStringLiteral("v")).toInt();
        if (size >= 10 && size <= 40) gFontSizeOverride = size;
    }
}

void BookView::findText(const QString &text, int flags) {
    if (text.isEmpty()) return;
    searches_.append(SearchJob{text, flags});
    runSearches();
}

void BookView::runSearches() {
    if (searching_ || searches_.isEmpty() || readyToken_ != token_) return;

    const SearchJob job = searches_.takeFirst();
    searching_ = true;
    const QString token = token_;

    auto finish = [this, token] {
        if (token != token_) { searching_ = false; return; }
        searching_ = false;
        runSearches();
    };

    QWebEnginePage::FindFlags flags;
    if (job.flags & lcs_backwards) flags |= QWebEnginePage::FindBackward;
    if (job.flags & lcs_matchcase) flags |= QWebEnginePage::FindCaseSensitively;

    auto doFind = [this, job, flags, token, finish] {
        web_->page()->findText(job.text, flags, [this, token, finish](const QWebEngineFindTextResult &r) {
            /* Chromium's find highlights are not a DOM selection; tests read the
             * match count from this global. */
            web_->page()->runJavaScript(QStringLiteral("window.__dcFindResult=%1")
                                            .arg(r.numberOfMatches()));
            if (token == token_ && r.numberOfMatches() == 0) QApplication::beep();
            finish();
        });
    };

    if (job.flags & lcs_findfirst)
        web_->page()->runJavaScript(QStringLiteral("window.getSelection().removeAllRanges();"),
                                    [doFind](const QVariant &) { doFind(); });
    else
        doFind();
}

int BookView::sendCommand(int command) {
    if (!web_ || !web_->page()) return LISTPLUGIN_ERROR;
    if (command == lc_copy) {
        /* Chromium's QWebEnginePage::Copy acts on the focused frame, and the
         * page deliberately has no keyboard focus (DC's shortcuts must keep
         * working). Read the selection out and set the clipboard ourselves. */
        web_->page()->runJavaScript(QStringLiteral(
            "(function(){var s=window.getSelection();"
            "if(!s||!s.rangeCount)return '';"
            "var d=document.createElement('div');"
            "for(var i=0;i<s.rangeCount;i++)d.appendChild(s.getRangeAt(i).cloneContents());"
            "return JSON.stringify({text:s.toString(),html:d.innerHTML});})()"),
            [](const QVariant &v) {
                if (!v.isValid() || v.isNull() || v.toString().isEmpty()) return;
                const QJsonObject o = QJsonDocument::fromJson(v.toString().toUtf8()).object();
                const QString text = o.value(QStringLiteral("text")).toString();
                if (text.isEmpty()) return;
                auto *mime = new QMimeData;
                mime->setText(text);
                const QString html = o.value(QStringLiteral("html")).toString();
                if (!html.isEmpty()) mime->setHtml(html);
                QGuiApplication::clipboard()->setMimeData(mime);
            });
        return LISTPLUGIN_OK;
    }
    if (command == lc_selectall) {
        web_->page()->runJavaScript(QStringLiteral(
            "(function(){var el=document.getElementById('book');if(!el)return false;"
            "var s=window.getSelection();if(!s)return false;"
            "s.removeAllRanges();s.selectAllChildren(el);return true;})()"));
        return LISTPLUGIN_OK;
    }
    return LISTPLUGIN_ERROR;
}

void BookView::dispatchReaderKey(const QString &key) {
    QString escaped = key;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('\''), QLatin1String("\\'"));
    web_->page()->runJavaScript(
        QStringLiteral("document.dispatchEvent(new KeyboardEvent('keydown',{key:'%1',"
                       "bubbles:true,cancelable:true}))").arg(escaped));
}

void BookView::scrollBy(int dy) {
    web_->page()->runJavaScript(QStringLiteral("window.scrollBy(0,%1)").arg(dy));
}

void BookView::forwardKey(QKeyEvent *event) {
    QWidget *target = window();
    if (!target) return;
    QKeyEvent forwarded(QEvent::KeyPress, event->key(), event->modifiers(), event->text(),
                        event->isAutoRepeat(), event->count());
    using SendSpontaneous = bool (*)(QObject *, QEvent *);
    static SendSpontaneous sendSpontaneous = reinterpret_cast<SendSpontaneous>(
        dlsym(RTLD_DEFAULT, "_ZN16QCoreApplication20sendSpontaneousEventEP7QObjectP6QEvent"));
    if (sendSpontaneous) sendSpontaneous(target, &forwarded);
    else QCoreApplication::sendEvent(target, &forwarded);
}

void BookView::keyPressEvent(QKeyEvent *event) {
    const Qt::KeyboardModifiers mods = event->modifiers() &
        (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
    const bool plain = (mods & ~Qt::ShiftModifier) == Qt::NoModifier;

    /* Reader shortcuts, as in the macOS plugin: t toggles contents, +/- text size. */
    if (plain) {
        if (event->key() == Qt::Key_T) {
            dispatchReaderKey(QStringLiteral("t"));
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
            dispatchReaderKey(QStringLiteral("+"));
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Minus || event->key() == Qt::Key_Underscore) {
            dispatchReaderKey(QStringLiteral("-"));
            event->accept();
            return;
        }
    }

    const int page = qMax(80, web_->height() * 9 / 10);
    if ((mods & ~Qt::ShiftModifier) == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_Up:    scrollBy(-40); event->accept(); return;
        case Qt::Key_Down:  scrollBy(40);  event->accept(); return;
        case Qt::Key_PageUp:   scrollBy(-page); event->accept(); return;
        case Qt::Key_PageDown: scrollBy(page);  event->accept(); return;
        case Qt::Key_Space:
            scrollBy((mods & Qt::ShiftModifier) ? -page : page);
            event->accept();
            return;
        case Qt::Key_Home:
            web_->page()->runJavaScript(QStringLiteral("window.scrollTo(0,0)"));
            event->accept();
            return;
        case Qt::Key_End:
            web_->page()->runJavaScript(
                QStringLiteral("window.scrollTo(0,document.documentElement.scrollHeight)"));
            event->accept();
            return;
        default: break;
        }
    }
    if (mods == Qt::ControlModifier && event->key() == Qt::Key_Up) {
        web_->page()->runJavaScript(QStringLiteral("window.scrollTo(0,0)"));
        event->accept();
        return;
    }
    if (mods == Qt::ControlModifier && event->key() == Qt::Key_Down) {
        web_->page()->runJavaScript(
            QStringLiteral("window.scrollTo(0,document.documentElement.scrollHeight)"));
        event->accept();
        return;
    }

    forwardKey(event);
    event->accept();
}

/* ---- WLX exported API --------------------------------------------------- */

static QString fromNative(const char *s) { return s ? QString::fromUtf8(s) : QString(); }

static QString fromWide(const char16_t *s) {
    if (!s) return QString();
    size_t n = 0;
    while (s[n]) ++n;
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(s), int(n));
}

extern "C" {

__attribute__((visibility("default")))
HWND DCPCALL ListLoadW(HWND ParentWin, char16_t *FileToLoad, int ShowFlags) {
    (void)ShowFlags;
    auto *parent = reinterpret_cast<QWidget *>(ParentWin);
    auto *view = new BookView(parent);
    view->loadBook(fromWide(FileToLoad));
    view->show();
    return reinterpret_cast<HWND>(view);
}

__attribute__((visibility("default")))
HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags) {
    (void)ShowFlags;
    auto *parent = reinterpret_cast<QWidget *>(ParentWin);
    auto *view = new BookView(parent);
    view->loadBook(fromNative(FileToLoad));
    view->show();
    return reinterpret_cast<HWND>(view);
}

__attribute__((visibility("default")))
int DCPCALL ListLoadNextW(HWND ParentWin, HWND PluginWin, char16_t *FileToLoad, int ShowFlags) {
    (void)ParentWin; (void)ShowFlags;
    auto *view = reinterpret_cast<BookView *>(PluginWin);
    if (!view) return LISTPLUGIN_ERROR;
    return view->loadBook(fromWide(FileToLoad)) ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
int DCPCALL ListLoadNext(HWND ParentWin, HWND PluginWin, char *FileToLoad, int ShowFlags) {
    (void)ParentWin; (void)ShowFlags;
    auto *view = reinterpret_cast<BookView *>(PluginWin);
    if (!view) return LISTPLUGIN_ERROR;
    return view->loadBook(fromNative(FileToLoad)) ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
void DCPCALL ListCloseWindow(HWND ListWin) {
    if (!ListWin) return;
    delete reinterpret_cast<BookView *>(ListWin);
}

__attribute__((visibility("default")))
int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
    (void)Parameter;
    auto *view = reinterpret_cast<BookView *>(ListWin);
    return view ? view->sendCommand(Command) : LISTPLUGIN_ERROR;
}

__attribute__((visibility("default")))
int DCPCALL ListSearchTextW(HWND ListWin, char16_t *SearchString, int SearchParameter) {
    auto *view = reinterpret_cast<BookView *>(ListWin);
    if (!view || !SearchString || !SearchString[0]) return LISTPLUGIN_ERROR;
    view->findText(fromWide(SearchString), SearchParameter);
    return LISTPLUGIN_OK;
}

__attribute__((visibility("default")))
int DCPCALL ListSearchText(HWND ListWin, char *SearchString, int SearchParameter) {
    auto *view = reinterpret_cast<BookView *>(ListWin);
    if (!view || !SearchString || !SearchString[0]) return LISTPLUGIN_ERROR;
    view->findText(fromNative(SearchString), SearchParameter);
    return LISTPLUGIN_OK;
}

__attribute__((visibility("default")))
void DCPCALL ListGetDetectString(char *DetectString, int maxlen) {
    if (!DetectString || maxlen <= 0) return;
    const char *s = "EXT=\"EPUB\"|EXT=\"FB2\"|EXT=\"FBZ\"";
    std::snprintf(DetectString, size_t(maxlen), "%s", s);
}

__attribute__((visibility("default")))
void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *dps) {
    if (dps && dps->DefaultIniName[0])
        gIniPath = QString::fromLocal8Bit(dps->DefaultIniName);
}

} /* extern "C" */
