/*
 * Generate sample images for the ImageView harness. Qt covers the raster
 * formats it can write; animated GIF / multi-size ICO / multi-page TIFF /
 * orientation-tagged JPEG are produced by test/run.sh with ImageMagick.
 *
 * Build: g++ -std=c++17 -fPIC -o build/make_samples test/make_samples.cpp \
 *            $(pkg-config --cflags --libs Qt6Gui) -lz
 * Run:   ./build/make_samples <out-dir>
 */
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QPainter>

#include <cstdio>
#include <cstring>

#include <zlib.h>

static bool save(const QImage &img, const QString &path, const char *fmt = nullptr) {
    QImageWriter w(path, fmt);
    if (!w.write(img)) {
        std::fprintf(stderr, "cannot write %s: %s\n", qPrintable(path),
                     qPrintable(w.errorString()));
        return false;
    }
    std::printf("  %-18s ok\n", qPrintable(QFileInfo(path).fileName()));
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: make_samples <out-dir>\n");
        return 2;
    }
    const QString dir = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(dir);

    QImage photo(320, 200, QImage::Format_RGB32);
    photo.fill(QColor(42, 90, 180));
    save(photo, dir + "/photo.png", "PNG");
    save(photo, dir + "/photo.jpg", "JPEG");
    save(photo, dir + "/photo.bmp", "BMP");

    QImage alpha(200, 100, QImage::Format_ARGB32);
    alpha.fill(QColor(200, 60, 60, 255));
    {
        QPainter p(&alpha);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(0, 0, 80, 60), Qt::transparent);
    }
    save(alpha, dir + "/alpha.png", "PNG");

    QImage pixel(16, 16, QImage::Format_RGB32);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            pixel.setPixelColor(x, y, ((x ^ y) & 1) ? Qt::black : Qt::white);
    save(pixel, dir + "/pixel.png", "PNG");

    QImage wide(1600, 200, QImage::Format_RGB32);
    wide.fill(QColor(60, 140, 90));
    save(wide, dir + "/wide.png", "PNG");

    QImage huge(20000, 100, QImage::Format_RGB32);
    huge.fill(QColor(120, 80, 160));
    save(huge, dir + "/huge.png", "PNG");

    QImage tall(200, 2000, QImage::Format_RGB32);
    tall.fill(QColor(80, 60, 160));
    save(tall, dir + "/tall.png", "PNG");

    /* Orientation metadata: rewrite photo.jpg with an EXIF Orientation=6 tag,
       so a 320x200 file must display as 200x320. Qt's writer cannot emit EXIF
       orientation, so the APP1 segment is inserted after SOI by hand. */
    {
        QFile in(dir + "/photo.jpg");
        if (in.open(QIODevice::ReadOnly)) {
            const QByteArray jpg = in.readAll();
            in.close();
            if (jpg.size() > 2 && uchar(jpg[0]) == 0xFF && uchar(jpg[1]) == 0xD8) {
                QByteArray tiff("II", 2);
                tiff.append(char(42)).append(char(0));
                tiff.append("\x08\x00\x00\x00", 4);
                QByteArray ifd;
                ifd.append("\x01\x00", 2);                       /* one entry */
                ifd.append("\x12\x01", 2);                       /* 0x0112 Orientation */
                ifd.append("\x03\x00", 2);                       /* SHORT */
                ifd.append("\x01\x00\x00\x00", 4);             /* count 1 */
                ifd.append("\x06\x00\x00\x00", 4);             /* 6 = Rotate 90 CW */
                ifd.append("\x00\x00\x00\x00", 4);             /* next IFD */
                QByteArray data("Exif", 4);
                data.append("\x00\x00", 2);
                data.append(tiff);
                data.append(ifd);
                QByteArray seg;
                seg.append(char(0xFF)).append(char(0xE1));
                seg.append(char((data.size() + 2) >> 8)).append(char((data.size() + 2) & 0xFF));
                seg.append(data);
                QFile out(dir + "/oriented.jpg");
                if (out.open(QIODevice::WriteOnly)) {
                    out.write(jpg.left(2));
                    out.write(seg);
                    out.write(jpg.mid(2));
                    out.close();
                    std::printf("  %-18s ok\n", "oriented.jpg");
                }
            }
        }
    }

    /* SVG: a normal diagram, transparent fills, and a broken file. */
    const QByteArray svg =
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20' viewBox='0 0 40 20'>"
        "<rect x='0' y='0' width='40' height='20' fill='#eef'/>"
        "<path d='M2 18 L20 2 L38 18 Z' fill='#36c' stroke='#123'/>"
        "</svg>";
    {
        QFile f(dir + "/edge.svg");
        (void)f.open(QIODevice::WriteOnly);
        f.write(svg);
    }
    std::printf("  %-18s ok\n", "edge.svg");

    {
        gzFile gz = gzopen(QFile::encodeName(dir + "/edge.svgz").constData(), "wb9");
        if (gz) {
            gzwrite(gz, svg.constData(), unsigned(svg.size()));
            gzclose(gz);
            std::printf("  %-18s ok\n", "edge.svgz");
        }
    }

    const QByteArray transparent =
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'>"
        "<rect width='40' height='20' fill='transparent'/>"
        "</svg>";
    {
        QFile f(dir + "/transparent.svg");
        (void)f.open(QIODevice::WriteOnly);
        f.write(transparent);
    }
    std::printf("  %-18s ok\n", "transparent.svg");

    {
        QFile f(dir + "/broken.svg");
        (void)f.open(QIODevice::WriteOnly);
        f.write("<svg><<not really");
    }
    std::printf("  %-18s ok\n", "broken.svg");

    /* Not images: a fake PNG and a disk image with a .raw name. */
    {
        QByteArray junk("not a png at all");
        QFile f(dir + "/garbage.png");
        (void)f.open(QIODevice::WriteOnly);
        f.write(junk);
    }
    {
        QByteArray junk(1 << 20, '\0');
        for (int i = 0; i < junk.size(); ++i) junk[i] = char((i * 31) & 0xFF);
        QFile f(dir + "/disk.raw");
        (void)f.open(QIODevice::WriteOnly);
        f.write(junk);
    }
    std::printf("  %-18s ok\n  %-18s ok\n", "garbage.png", "disk.raw");
    return 0;
}
