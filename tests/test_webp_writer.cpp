// Animated WebP assembly (core/io/WebpAnimWriter.h), re-read through
// libwebpdemux — the independent decoder half of the same library family.
#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <webp/demux.h>

#include "io/WebpAnimWriter.h"

using namespace viki;

namespace {

std::vector<QImage> movingSquare(int count)
{
    std::vector<QImage> frames;
    for (int f = 0; f < count; ++f) {
        QImage img(64, 48, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.fillRect(6 + f * 7, 10, 16, 16, QColor(40, 160, 90));
        frames.push_back(img);
    }
    return frames;
}

struct Demuxed {
    WebPDemuxer* demux = nullptr;
    QByteArray bytes; // must outlive the demuxer
    ~Demuxed()
    {
        if (demux)
            WebPDemuxDelete(demux);
    }
};

void demuxFile(const QString& path, Demuxed& out)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    out.bytes = f.readAll();
    WebPData data;
    data.bytes = reinterpret_cast<const uint8_t*>(out.bytes.constData());
    data.size = static_cast<size_t>(out.bytes.size());
    out.demux = WebPDemux(&data);
    REQUIRE(out.demux != nullptr);
}

} // namespace

TEST_CASE("animated webp round-trips frame count, loop and alpha",
          "[anim]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("loop.webp"));

    const auto frames = movingSquare(5);
    const WebpAnimResult res = writeWebpAnim(frames, 40, 0, path);
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    CHECK(res.frames == 5);
    CHECK(res.bytes > 0);

    Demuxed d;
    demuxFile(path, d);
    CHECK(WebPDemuxGetI(d.demux, WEBP_FF_FRAME_COUNT) == 5u);
    CHECK(WebPDemuxGetI(d.demux, WEBP_FF_LOOP_COUNT) == 0u);
    CHECK(WebPDemuxGetI(d.demux, WEBP_FF_CANVAS_WIDTH) == 64u);
    CHECK(WebPDemuxGetI(d.demux, WEBP_FF_CANVAS_HEIGHT) == 48u);
    // The transparent background must survive as a real alpha channel.
    CHECK((WebPDemuxGetI(d.demux, WEBP_FF_FORMAT_FLAGS) & ALPHA_FLAG)
          != 0u);

    // Every frame carries the 40 ms duration.
    WebPIterator iter;
    REQUIRE(WebPDemuxGetFrame(d.demux, 1, &iter));
    CHECK(iter.duration == 40);
    WebPDemuxReleaseIterator(&iter);
}

TEST_CASE("hold mode maps to a play-once loop count", "[anim]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("hold.webp"));
    REQUIRE(writeWebpAnim(movingSquare(3), 50, 1, path).ok);

    Demuxed d;
    demuxFile(path, d);
    CHECK(WebPDemuxGetI(d.demux, WEBP_FF_LOOP_COUNT) == 1u);
}

TEST_CASE("webp writer is byte-deterministic", "[anim]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString pathA = dir.filePath(QStringLiteral("a.webp"));
    const QString pathB = dir.filePath(QStringLiteral("b.webp"));
    const auto frames = movingSquare(4);
    REQUIRE(writeWebpAnim(frames, 33, 0, pathA).ok);
    REQUIRE(writeWebpAnim(frames, 33, 0, pathB).ok);
    QFile a(pathA), b(pathB);
    REQUIRE(a.open(QIODevice::ReadOnly));
    REQUIRE(b.open(QIODevice::ReadOnly));
    CHECK(a.readAll() == b.readAll());
}

TEST_CASE("webp writer refuses broken input", "[anim]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bad.webp"));

    CHECK_FALSE(writeWebpAnim({}, 40, 0, path).ok);

    auto frames = movingSquare(2);
    frames.push_back(QImage(32, 32, QImage::Format_ARGB32));
    const WebpAnimResult res = writeWebpAnim(frames, 40, 0, path);
    CHECK_FALSE(res.ok);
    CHECK(res.error.contains(QStringLiteral("32x32")));
}
