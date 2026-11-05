/*
 * MediaInfo — Double Commander Content (WDX) plugin for Linux.
 *
 * Exposes per-file media metadata as content fields you can show in custom
 * columns / tooltips: image & video dimensions, audio & video duration,
 * bitrate, codecs, PDF page count, and an adaptive "Summary" field that picks
 * the single most useful string per file type.
 *
 * Linux port of the upstream macOS plugin (NikolaiSachok/DC-plugins).
 * The upstream macOS plugin uses ImageIO, AVFoundation and CGPDF; here the equivalents
 * are system libraries:
 *   - images -> Qt's QImageReader (header read only; never decodes pixels)
 *   - audio/video -> libavformat (FFmpeg)
 *   - PDF -> poppler-glib
 * No network access, ever.
 *
 * The WDX ABI is the same on every platform Double Commander supports; only
 * the "window handle" style types differ, and content plugins have none.
 */

/* Qt's `signals` keyword macro collides with a field in glib's
   GDBusInterfaceInfo (pulled in by poppler-glib). We use neither signals nor
   slots here. */
#define QT_NO_KEYWORDS

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSize>
#include <QString>
#include <QUrl>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
}
#include <poppler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fenv.h>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "contplug.h"

#define MI_VERSION "0.1.0"

/* ---- Field table -------------------------------------------------------- */

enum {
    F_SUMMARY = 0,
    F_DIMENSIONS,
    F_WIDTH,
    F_HEIGHT,
    F_MEGAPIXELS,
    F_DPI,
    F_COLORDEPTH,
    F_DURATION,
    F_DURATIONSECS,
    F_FRAMERATE,
    F_BITRATE,
    F_SAMPLERATE,
    F_CHANNELS,
    F_VIDEOCODEC,
    F_AUDIOCODEC,
    F_PAGECOUNT,
    F_PLUGINVERSION,
    F_COUNT
};

typedef struct {
    const char *name;
    int         type;     /* ft_* */
    const char *units;
} MIField;

/* Order MUST match the enum above. */
static const MIField kFields[F_COUNT] = {
    [F_SUMMARY]       = { "Summary",        ft_string,           ""      },
    [F_DIMENSIONS]    = { "Dimensions",     ft_string,           ""      },
    [F_WIDTH]         = { "Width",          ft_numeric_32,       "px"    },
    [F_HEIGHT]        = { "Height",         ft_numeric_32,       "px"    },
    [F_MEGAPIXELS]    = { "Megapixels",     ft_numeric_floating, "MP"    },
    [F_DPI]           = { "DPI",            ft_numeric_32,       "dpi"   },
    [F_COLORDEPTH]    = { "Bit depth",      ft_numeric_32,       "bit"   },
    [F_DURATION]      = { "Duration",       ft_string,           ""      },
    [F_DURATIONSECS]  = { "Duration (s)",   ft_numeric_floating, "s"     },
    [F_FRAMERATE]     = { "Frame rate",     ft_numeric_floating, "fps"   },
    [F_BITRATE]       = { "Bitrate",        ft_numeric_32,       "kbps"  },
    [F_SAMPLERATE]    = { "Sample rate",    ft_numeric_32,       "Hz"    },
    [F_CHANNELS]      = { "Channels",       ft_numeric_32,       ""      },
    [F_VIDEOCODEC]    = { "Video codec",    ft_string,           ""      },
    [F_AUDIOCODEC]    = { "Audio codec",    ft_string,           ""      },
    [F_PAGECOUNT]     = { "Page count",     ft_numeric_32,       "pages" },
    [F_PLUGINVERSION] = { "Plugin version", ft_string,           ""      },
};

/* ---- Categories --------------------------------------------------------- */

typedef enum { CAT_OTHER = 0, CAT_IMAGE, CAT_AUDIO, CAT_VIDEO, CAT_PDF } MICategory;

static bool extIn(const QString &ext, const char *const *list) {
    for (int i = 0; list[i]; ++i)
        if (ext == QLatin1String(list[i])) return true;
    return false;
}

static MICategory categoryForPath(const QString &path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext.isEmpty()) return CAT_OTHER;

    static const char *const img[] = {
        "jpg","jpeg","png","gif","tiff","tif","bmp","webp","heic","heif","avif",
        "ico","icns","psd","jp2","jxl","dng","cr2","cr3","nef","arw","orf","rw2",
        "raf","sr2","pef", nullptr };
    static const char *const aud[] = {
        "mp3","m4a","m4b","aac","wav","aiff","aif","aifc","caf","flac","ogg",
        "oga","opus","wma","mka","ape","wv", nullptr };
    static const char *const vid[] = {
        "mp4","mov","m4v","3gp","3g2","avi","mkv","webm","mpg","mpeg","ts",
        "m2ts","mts","vob","wmv","flv","f4v","ogv", nullptr };

    if (extIn(ext, img)) return CAT_IMAGE;
    if (extIn(ext, aud)) return CAT_AUDIO;
    if (extIn(ext, vid)) return CAT_VIDEO;
    if (ext == QLatin1String("pdf")) return CAT_PDF;
    return CAT_OTHER;
}

/* Only extensions a backend can actually read. Images beyond Qt's base set
   (tiff/webp/heic/avif/raw/…) need the matching Qt image plugin installed. */
static const char *kDetectString =
    "(EXT=\"JPG\")|(EXT=\"JPEG\")|(EXT=\"PNG\")|(EXT=\"GIF\")|(EXT=\"TIFF\")|"
    "(EXT=\"TIF\")|(EXT=\"BMP\")|(EXT=\"WEBP\")|(EXT=\"HEIC\")|(EXT=\"HEIF\")|"
    "(EXT=\"AVIF\")|(EXT=\"ICO\")|(EXT=\"ICNS\")|(EXT=\"PSD\")|(EXT=\"JP2\")|"
    "(EXT=\"JXL\")|(EXT=\"DNG\")|(EXT=\"CR2\")|(EXT=\"CR3\")|(EXT=\"NEF\")|"
    "(EXT=\"ARW\")|(EXT=\"ORF\")|(EXT=\"RW2\")|(EXT=\"RAF\")|(EXT=\"SR2\")|"
    "(EXT=\"PEF\")|(EXT=\"MP3\")|(EXT=\"M4A\")|(EXT=\"M4B\")|(EXT=\"AAC\")|"
    "(EXT=\"WAV\")|(EXT=\"AIFF\")|(EXT=\"AIF\")|(EXT=\"AIFC\")|(EXT=\"CAF\")|"
    "(EXT=\"FLAC\")|(EXT=\"OGG\")|(EXT=\"OGA\")|(EXT=\"OPUS\")|(EXT=\"WMA\")|"
    "(EXT=\"MKA\")|(EXT=\"APE\")|(EXT=\"WV\")|(EXT=\"MP4\")|(EXT=\"MOV\")|"
    "(EXT=\"M4V\")|(EXT=\"3GP\")|(EXT=\"3G2\")|(EXT=\"AVI\")|(EXT=\"MKV\")|"
    "(EXT=\"WEBM\")|(EXT=\"MPG\")|(EXT=\"MPEG\")|(EXT=\"TS\")|(EXT=\"M2TS\")|"
    "(EXT=\"MTS\")|(EXT=\"VOB\")|(EXT=\"WMV\")|(EXT=\"FLV\")|(EXT=\"F4V\")|"
    "(EXT=\"OGV\")|(EXT=\"PDF\")";

/* ---- Parsed info -------------------------------------------------------- */

struct Info {
    std::optional<int>    width, height, dpi, bits, pageCount;
    std::optional<int>    bitrate, sampleRate, channels;
    std::optional<double> megapixels, durationSecs, fps;
    std::string dimensions, duration, summary, videoCodec, audioCodec;
};

/* ---- Formatting helpers ------------------------------------------------- */

/* Reject non-finite, negative, and absurd (>~100 years) values: the last
   guards against a corrupt/hostile container feeding an out-of-range double
   into lround, which is otherwise undefined. */
static std::optional<std::string> formatDuration(double secs) {
    if (!std::isfinite(secs) || secs < 0 || secs > 3.15e9) return std::nullopt;
    long t = std::lround(secs);
    long h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", h, m, s);
    else       std::snprintf(buf, sizeof buf, "%ld:%02ld", m, s);
    return std::string(buf);
}

static std::string formatDims(int w, int h) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d \xC3\x97 %d", w, h);  /* U+00D7, UTF-8 */
    return std::string(buf);
}

/* Populate the common video fields and the adaptive Summary from raw numbers.
   Any of the inputs may be 0/absent. */
static void fillVideoFields(Info &v, long w, long h, double secs, double fps) {
    std::string dims;
    if (w > 0 && h > 0 && w <= 100000 && h <= 100000) {
        v.width = int(w);
        v.height = int(h);
        dims = formatDims(int(w), int(h));
        v.dimensions = dims;
    }
    if (secs > 0) {
        if (auto d = formatDuration(secs)) {
            v.duration = *d;
            v.durationSecs = std::round(secs * 10.0) / 10.0;
        }
    }
    if (fps > 0 && fps < 1000)
        v.fps = std::round(fps * 100.0) / 100.0;

    if (!dims.empty() && !v.duration.empty())
        v.summary = dims + " \xC2\xB7 " + v.duration;      /* U+00B7, UTF-8 */
    else if (!dims.empty())
        v.summary = dims;
    else if (!v.duration.empty())
        v.summary = v.duration;
}

/* ---- Image backends ----------------------------------------------------- */

static QByteArray readHead(const QString &path, qint64 maxBytes = 1 << 20) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.read(maxBytes);
}

static uint32_t be32(const uchar *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint16_t be16(const uchar *p) {
    return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

/* PNG: IHDR bit depth + pHYs pixels-per-metre. */
static void parsePngHeader(const QByteArray &b, Info &v) {
    static const uchar sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() < 33 || std::memcmp(b.constData(), sig, 8) != 0) return;
    const uchar *d = reinterpret_cast<const uchar *>(b.constData());
    if (std::memcmp(d + 12, "IHDR", 4) != 0) return;
    v.bits = int(d[24]);                        /* bit depth per component */

    qint64 pos = 8;
    while (pos + 12 <= b.size()) {
        const uchar *c = d + pos;
        const uint32_t len = be32(c);
        if (std::memcmp(c + 4, "pHYs", 4) == 0 && len >= 9 && pos + 12 + 9 <= b.size()) {
            const uint32_t ppu = be32(c + 8);
            const uchar unit = c[16];           /* 1 = metre */
            if (unit == 1 && ppu > 0) {
                const int dpi = int(std::lround(double(ppu) * 0.0254));
                if (dpi > 0 && dpi <= 100000) v.dpi = dpi;
            }
            break;
        }
        if (len > uint32_t(1 << 30)) break;     /* hostile length */
        pos += 12 + qint64(len);
    }
}

/* JPEG: SOFn precision + dimensions, JFIF density for DPI. */
static void parseJpegHeader(const QByteArray &b, Info &v) {
    const uchar *d = reinterpret_cast<const uchar *>(b.constData());
    const qint64 n = b.size();
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return;

    qint64 i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) { ++i; continue; }
        const uchar marker = d[i + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) break;   /* EOI / start of scan */
        const uint16_t len = be16(d + i + 2);
        if (len < 2 || i + 2 + len > n) break;

        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
            marker != 0xC8 && marker != 0xCC && len >= 7) {
            v.bits = int(d[i + 4]);                    /* sample precision */
        } else if (marker == 0xE0 && len >= 16 &&
                   std::memcmp(d + i + 4, "JFIF\0", 5) == 0) {
            const uchar units = d[i + 11];
            const uint16_t xd = be16(d + i + 12);
            int dpi = 0;
            if (units == 1) dpi = xd;
            else if (units == 2) dpi = int(std::lround(double(xd) * 2.54));
            if (dpi > 0 && dpi <= 100000) v.dpi = dpi;
        }
        i += 2 + len;
    }
}

/* Classic TIFF: XResolution (282), ResolutionUnit (296), BitsPerSample (258). */
static void parseTiffHeader(const QByteArray &b, Info &v) {
    const uchar *d = reinterpret_cast<const uchar *>(b.constData());
    const qint64 n = b.size();
    if (n < 8) return;
    bool le;
    if (d[0] == 'I' && d[1] == 'I') le = true;
    else if (d[0] == 'M' && d[1] == 'M') le = false;
    else return;
    auto rd16 = [&](qint64 o) -> uint16_t {
        if (o + 2 > n) return 0;
        return le ? uint16_t(d[o] | (d[o + 1] << 8))
                  : uint16_t((d[o] << 8) | d[o + 1]);
    };
    auto rd32 = [&](qint64 o) -> uint32_t {
        if (o + 4 > n) return 0;
        return le ? (uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) |
                     (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24))
                  : ((uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) |
                     (uint32_t(d[o + 2]) << 8) | uint32_t(d[o + 3]));
    };

    const qint64 ifd = rd32(4);
    const uint16_t count = rd16(ifd);
    int unit = 2;                                   /* default: inch */
    double xres = 0;
    for (uint16_t k = 0; k < count; ++k) {
        const qint64 e = ifd + 2 + qint64(k) * 12;
        const uint16_t tag = rd16(e);
        const uint16_t type = rd16(e + 2);
        const uint32_t cnt = rd32(e + 4);
        if (tag == 258 && type == 3 && cnt >= 1)
            v.bits = int(rd16(e + 8));
        else if (tag == 296 && type == 3 && cnt == 1)
            unit = int(rd16(e + 8));
        else if (tag == 282 && type == 5 && cnt == 1) {
            const qint64 off = rd32(e + 8);
            const uint32_t num = rd32(off), den = rd32(off + 4);
            if (den) xres = double(num) / double(den);
        }
    }
    if (xres > 0) {
        if (unit == 3) xres *= 2.54;                /* centimetres */
        const int dpi = int(std::lround(xres));
        if (dpi > 0 && dpi <= 100000) v.dpi = dpi;
    }
}

static Info parseImage(const QString &path) {
    Info v;
    QImageReader reader(path);
    const QSize sz = reader.size();
    if (!sz.isValid() || sz.width() <= 0 || sz.height() <= 0) return v;

    v.width = sz.width();
    v.height = sz.height();
    v.dimensions = formatDims(sz.width(), sz.height());
    v.summary = v.dimensions;
    v.megapixels = std::round(double(sz.width()) * double(sz.height()) / 1.0e5) / 10.0;

    const QByteArray head = readHead(path);
    parsePngHeader(head, v);
    parseJpegHeader(head, v);
    parseTiffHeader(head, v);
    return v;
}

/* ---- PDF backend -------------------------------------------------------- */

static Info parsePdf(const QString &path) {
    Info v;
    GError *err = nullptr;
    const QByteArray uri = QUrl::fromLocalFile(path).toString().toUtf8();
    PopplerDocument *doc = poppler_document_new_from_file(uri.constData(), nullptr, &err);
    if (!doc) {
        if (err) g_error_free(err);
        return v;
    }
    const int pages = poppler_document_get_n_pages(doc);
    g_object_unref(doc);
    if (pages <= 0) return v;

    v.pageCount = pages;
    v.summary = (pages == 1) ? "1 page" : std::to_string(pages) + " pages";
    return v;
}

/* ---- Audio / video backend (libavformat) -------------------------------- */

static std::string codecName(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264:        return "H.264";
        case AV_CODEC_ID_HEVC:        return "HEVC";
        case AV_CODEC_ID_MPEG4:       return "MPEG-4";
        case AV_CODEC_ID_MPEG2VIDEO:  return "MPEG-2";
        case AV_CODEC_ID_MPEG1VIDEO:  return "MPEG-1";
        case AV_CODEC_ID_VP8:         return "VP8";
        case AV_CODEC_ID_VP9:         return "VP9";
        case AV_CODEC_ID_AV1:         return "AV1";
        case AV_CODEC_ID_THEORA:      return "Theora";
        case AV_CODEC_ID_PRORES:      return "ProRes";
        case AV_CODEC_ID_MJPEG:       return "MJPEG";
        case AV_CODEC_ID_AAC:         return "AAC";
        case AV_CODEC_ID_MP3:         return "MP3";
        case AV_CODEC_ID_AC3:         return "AC-3";
        case AV_CODEC_ID_EAC3:        return "E-AC-3";
        case AV_CODEC_ID_OPUS:        return "Opus";
        case AV_CODEC_ID_VORBIS:      return "Vorbis";
        case AV_CODEC_ID_FLAC:        return "FLAC";
        case AV_CODEC_ID_ALAC:        return "ALAC";
        case AV_CODEC_ID_TRUEHD:      return "TrueHD";
        case AV_CODEC_ID_DTS:         return "DTS";
        default: break;
    }
    const char *n = avcodec_get_name(id);
    if (!n || std::strcmp(n, "unknown") == 0) return std::string();
    if (std::strncmp(n, "pcm_", 4) == 0) return "PCM";   /* one label, as macOS */
    return std::string(n);
}

/* A display matrix in the codec parameters means the picture is rotated for
   display: swap the coded width/height for 90°/270°. */
static void applyRotation(const AVCodecParameters *par, int &w, int &h) {
    for (int i = 0; i < par->nb_coded_side_data; ++i) {
        const AVPacketSideData &sd = par->coded_side_data[i];
        if (sd.type == AV_PKT_DATA_DISPLAYMATRIX && sd.size >= 9 * int(sizeof(int32_t))) {
            double deg = av_display_rotation_get(reinterpret_cast<const int32_t *>(sd.data));
            if (!std::isfinite(deg)) continue;
            long d = std::lround(deg);
            if (d < 0) d += 360;
            if (d == 90 || d == 270) std::swap(w, h);
        }
    }
}

static Info parseAV(const QString &path) {
    Info v;
    const QByteArray p = path.toUtf8();
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) < 0) return v;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return v;
    }

    double secs = 0;
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        secs = double(fmt->duration) / double(AV_TIME_BASE);
    } else {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream *st = fmt->streams[i];
            if (st->duration > 0)
                secs = std::max(secs, double(st->duration) * av_q2d(st->time_base));
        }
    }

    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (vs >= 0) {
        AVStream *st = fmt->streams[vs];
        int w = st->codecpar->width, h = st->codecpar->height;
        applyRotation(st->codecpar, w, h);
        const AVRational sar = av_guess_sample_aspect_ratio(fmt, st, nullptr);
        if (sar.num > 0 && sar.den > 0 && sar.num != sar.den)
            w = int(std::lround(double(w) * double(sar.num) / double(sar.den)));

        AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
        if (fr.num <= 0 || fr.den <= 0) fr = st->avg_frame_rate;
        const double fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0;
        fillVideoFields(v, w, h, secs, fps);

        const std::string vc = codecName(st->codecpar->codec_id);
        if (!vc.empty()) v.videoCodec = vc;
    } else if (as >= 0) {
        fillVideoFields(v, 0, 0, secs, 0);
    }

    if (as >= 0) {
        AVStream *st = fmt->streams[as];
        const std::string ac = codecName(st->codecpar->codec_id);
        if (!ac.empty()) v.audioCodec = ac;
        if (st->codecpar->sample_rate > 0) v.sampleRate = st->codecpar->sample_rate;
        if (st->codecpar->ch_layout.nb_channels > 0)
            v.channels = st->codecpar->ch_layout.nb_channels;
    }

    int64_t bitrate = fmt->bit_rate;
    if (bitrate <= 0) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (fmt->streams[i]->codecpar->bit_rate > 0)
                bitrate += fmt->streams[i]->codecpar->bit_rate;
    }
    if (bitrate > 0) {
        const long long kbps = std::llround(double(bitrate) / 1000.0);
        if (kbps > 0 && kbps <= 2147483647LL) v.bitrate = int(kbps);
    }

    avformat_close_input(&fmt);
    return v;
}

/* ---- Parse dispatch + cache -------------------------------------------- */

static std::mutex gLock;
static std::unordered_map<std::string, Info> gCache;
static const size_t kCacheLimit = 256;

/* Double Commander is a Lazarus/FPC app that ENABLES floating-point exception
   traps. Library code (and this plugin's own math) may raise them where the
   default masked environment would not, which DC surfaces as a fatal "Access
   violation". Mask exceptions across every parse and restore the host's
   environment before returning. */
struct FpGuard {
    fenv_t env;
    FpGuard() { fegetenv(&env); fesetenv(FE_DFL_ENV); }
    ~FpGuard() { fesetenv(&env); }
};

static Info parseForCategory(const QString &path, MICategory cat) {
    FpGuard fp;
    switch (cat) {
        case CAT_IMAGE: return parseImage(path);
        case CAT_PDF:   return parsePdf(path);
        case CAT_AUDIO:
        case CAT_VIDEO: return parseAV(path);
        default:        return Info();
    }
}

static std::string cacheKey(const QString &path) {
    const QFileInfo fi(path);
    const qint64 mtime = fi.exists() ? fi.lastModified().toSecsSinceEpoch() : 0;
    const qint64 size = fi.exists() ? fi.size() : -1;
    return path.toUtf8().toStdString() + '\x1f' + std::to_string(mtime) + ':' +
           std::to_string(size);
}

/* ---- Value writers ------------------------------------------------------ */

static int writeString(const std::string &s, void *buf, int maxlen) {
    if (maxlen <= 0) return ft_fieldempty;
    std::snprintf(static_cast<char *>(buf), size_t(maxlen), "%s", s.c_str());
    return ft_string;
}

/* ---- Exported WDX ABI --------------------------------------------------- */

static int valueFor(const Info &v, int field, void *out, int maxlen) {
    switch (field) {
    case F_SUMMARY:
        return v.summary.empty() ? ft_fieldempty : writeString(v.summary, out, maxlen);
    case F_DIMENSIONS:
        return v.dimensions.empty() ? ft_fieldempty : writeString(v.dimensions, out, maxlen);
    case F_DURATION:
        return v.duration.empty() ? ft_fieldempty : writeString(v.duration, out, maxlen);
    case F_VIDEOCODEC:
        return v.videoCodec.empty() ? ft_fieldempty : writeString(v.videoCodec, out, maxlen);
    case F_AUDIOCODEC:
        return v.audioCodec.empty() ? ft_fieldempty : writeString(v.audioCodec, out, maxlen);
    case F_WIDTH:
        if (!v.width) return ft_fieldempty;
        if (maxlen < int(sizeof(int32_t))) return ft_fieldempty;
        *static_cast<int32_t *>(out) = *v.width;
        return ft_numeric_32;
    case F_HEIGHT:
        if (!v.height) return ft_fieldempty;
        if (maxlen < int(sizeof(int32_t))) return ft_fieldempty;
        *static_cast<int32_t *>(out) = *v.height;
        return ft_numeric_32;
    case F_DPI:
        if (!v.dpi) return ft_fieldempty;
        if (maxlen < int(sizeof(int32_t))) return ft_fieldempty;
        *static_cast<int32_t *>(out) = *v.dpi;
        return ft_numeric_32;
    case F_COLORDEPTH:
        if (!v.bits) return ft_fieldempty;
        if (maxlen < int(sizeof(int32_t))) return ft_fieldempty;
        *static_cast<int32_t *>(out) = *v.bits;
        return ft_numeric_32;
    case F_PAGECOUNT:
        if (!v.pageCount) return ft_fieldempty;
        if (maxlen < int(sizeof(int32_t))) return ft_fieldempty;
        *static_cast<int32_t *>(out) = *v.pageCount;
        return ft_numeric_32;
    case F_BITRATE:
        if (!v.bitrate) return ft_fieldempty;
        if (maxlen < int(sizeof(int32_t))) return ft_fieldempty;
        *static_cast<int32_t *>(out) = *v.bitrate;
        return ft_numeric_32;
    case F_SAMPLERATE:
        if (!v.sampleRate) return ft_fieldempty;
        if (maxlen < int(sizeof(int32_t))) return ft_fieldempty;
        *static_cast<int32_t *>(out) = *v.sampleRate;
        return ft_numeric_32;
    case F_CHANNELS:
        if (!v.channels) return ft_fieldempty;
        if (maxlen < int(sizeof(int32_t))) return ft_fieldempty;
        *static_cast<int32_t *>(out) = *v.channels;
        return ft_numeric_32;
    case F_MEGAPIXELS:
        if (!v.megapixels) return ft_fieldempty;
        if (maxlen < int(sizeof(double))) return ft_fieldempty;
        *static_cast<double *>(out) = *v.megapixels;
        return ft_numeric_floating;
    case F_DURATIONSECS:
        if (!v.durationSecs) return ft_fieldempty;
        if (maxlen < int(sizeof(double))) return ft_fieldempty;
        *static_cast<double *>(out) = *v.durationSecs;
        return ft_numeric_floating;
    case F_FRAMERATE:
        if (!v.fps) return ft_fieldempty;
        if (maxlen < int(sizeof(double))) return ft_fieldempty;
        *static_cast<double *>(out) = *v.fps;
        return ft_numeric_floating;
    default:
        return ft_fieldempty;
    }
}

static int getValue(const QString &path, int field, void *value, int maxlen, int flags) {
    if (field < 0 || field >= F_COUNT) return ft_nosuchfield;
    if (!value) return ft_fileerror;

    if (field == F_PLUGINVERSION)
        return writeString("MediaInfo " MI_VERSION, value, maxlen);

    const MICategory cat = categoryForPath(path);
    if (cat == CAT_OTHER) return ft_fieldempty;

    const std::string key = cacheKey(path);

    std::lock_guard<std::mutex> lock(gLock);
    auto it = gCache.find(key);
    if (it == gCache.end()) {
        /* FFmpeg/open probes can be slow; defer audio/video to DC's background
           thread when it asks (the same protocol the upstream plugin uses). */
        const bool slow = (cat == CAT_AUDIO || cat == CAT_VIDEO);
        if (slow && (flags & CONTENT_DELAYIFSLOW)) return ft_delayed;
        if (gCache.size() >= kCacheLimit) gCache.clear();
        it = gCache.emplace(key, parseForCategory(path, cat)).first;
    }
    return valueFor(it->second, field, value, maxlen);
}

extern "C" {

DLLEXPORT int DCPCALL ContentGetSupportedField(int n, char *name, char *units, int maxlen) {
    if (n < 0 || n >= F_COUNT) {
        if (name && maxlen > 0) name[0] = 0;
        return ft_nomorefields;
    }
    if (name && maxlen > 0)
        std::snprintf(name, size_t(maxlen), "%s", kFields[n].name);
    if (units && maxlen > 0)
        std::snprintf(units, size_t(maxlen), "%s", kFields[n].units);
    return kFields[n].type;
}

DLLEXPORT int DCPCALL ContentGetSupportedFieldFlags(int n) {
    (void)n;
    return 0;
}

DLLEXPORT int DCPCALL ContentGetValue(char *fileName, int field, int unit,
                                      void *fieldValue, int maxlen, int flags) {
    (void)unit;
    if (!fileName) return ft_fileerror;
    return getValue(QString::fromUtf8(fileName), field, fieldValue, maxlen, flags);
}

DLLEXPORT int DCPCALL ContentGetValueW(char16_t *fileName, int field, int unit,
                                       void *fieldValue, int maxlen, int flags) {
    (void)unit;
    if (!fileName) return ft_fileerror;
    return getValue(QString::fromUtf16(reinterpret_cast<const char16_t *>(fileName)),
                    field, fieldValue, maxlen, flags);
}

DLLEXPORT int DCPCALL ContentGetDetectString(char *detectString, int maxlen) {
    if (detectString && maxlen > 0)
        std::snprintf(detectString, size_t(maxlen), "%s", kDetectString);
    return 0;
}

DLLEXPORT void DCPCALL ContentSetDefaultParams(ContentDefaultParamStruct *dps) {
    (void)dps;
}

DLLEXPORT void DCPCALL ContentPluginUnloading(void) {
    std::lock_guard<std::mutex> lock(gLock);
    gCache.clear();
}

} /* extern "C" */

/* Quiet FFmpeg's stderr chatter (it would otherwise write into DC's terminal). */
__attribute__((constructor))
static void MIInit(void) {
    av_log_set_level(AV_LOG_QUIET);
}
