#include "io/WebpAnimWriter.h"

#include <cstring>

#include <QFile>

#include <webp/encode.h>
#include <webp/mux.h>

namespace viki {

namespace {

WebpAnimResult failWebp(const QString& message)
{
    WebpAnimResult r;
    r.error = message;
    return r;
}

} // namespace

struct WebpAnimStream::Impl {
    WebPAnimEncoder* enc = nullptr;
    float quality = 80.0f;
};

WebpAnimStream::WebpAnimStream(int width, int height, int loopCount,
                               float quality)
    : m_width(width), m_height(height)
{
    if (width <= 0 || height <= 0) {
        m_error = QStringLiteral("webp: empty frame size");
        return;
    }
    WebPAnimEncoderOptions encOptions;
    if (!WebPAnimEncoderOptionsInit(&encOptions)) {
        m_error = QStringLiteral("webp: encoder options init failed");
        return;
    }
    encOptions.anim_params.loop_count = loopCount;
    m_impl = new Impl;
    m_impl->quality = quality;
    m_impl->enc = WebPAnimEncoderNew(width, height, &encOptions);
    if (!m_impl->enc)
        m_error = QStringLiteral("webp: encoder init failed");
}

WebpAnimStream::~WebpAnimStream()
{
    if (m_impl) {
        if (m_impl->enc)
            WebPAnimEncoderDelete(m_impl->enc);
        delete m_impl;
    }
}

bool WebpAnimStream::addFrame(const QImage& frame, int timestampMs)
{
    if (!ok())
        return false;
    if (frame.width() != m_width || frame.height() != m_height) {
        m_error = QStringLiteral("webp: frame %1 is %2x%3, expected %4x%5")
                      .arg(m_frames)
                      .arg(frame.width())
                      .arg(frame.height())
                      .arg(m_width)
                      .arg(m_height);
        return false;
    }
    // QImage ARGB32 (non-premultiplied) is the same 0xAARRGGBB packing as
    // WebPPicture's use_argb buffer.
    const QImage converted =
        frame.convertToFormat(QImage::Format_ARGB32);

    WebPConfig config;
    if (!WebPConfigInit(&config)) {
        m_error = QStringLiteral("webp: config init failed");
        return false;
    }
    config.quality = m_impl->quality;

    WebPPicture pic;
    if (!WebPPictureInit(&pic)) {
        m_error = QStringLiteral("webp: picture init failed");
        return false;
    }
    pic.width = m_width;
    pic.height = m_height;
    pic.use_argb = 1;
    if (!WebPPictureAlloc(&pic)) {
        m_error = QStringLiteral("webp: picture alloc failed");
        return false;
    }
    for (int y = 0; y < m_height; ++y)
        std::memcpy(pic.argb
                        + static_cast<size_t>(y)
                              * static_cast<size_t>(pic.argb_stride),
                    converted.constScanLine(y),
                    static_cast<size_t>(m_width) * 4);

    const int added =
        WebPAnimEncoderAdd(m_impl->enc, &pic, timestampMs, &config);
    WebPPictureFree(&pic);
    if (!added) {
        m_error = QStringLiteral("webp: frame %1 rejected (%2)")
                      .arg(m_frames)
                      .arg(QString::fromUtf8(
                          WebPAnimEncoderGetError(m_impl->enc)));
        return false;
    }
    ++m_frames;
    return true;
}

WebpAnimResult WebpAnimStream::finish(const QString& path,
                                      int endTimestampMs)
{
    if (!ok())
        return failWebp(m_error);
    if (m_frames == 0)
        return failWebp(QStringLiteral("webp: no frames to encode"));

    // Flush: the final NULL frame closes the previous frame's duration.
    WebPAnimEncoderAdd(m_impl->enc, nullptr, endTimestampMs, nullptr);

    WebPData data;
    WebPDataInit(&data);
    const int assembled = WebPAnimEncoderAssemble(m_impl->enc, &data);
    if (!assembled) {
        m_error = QStringLiteral("webp: assembly failed");
        return failWebp(m_error);
    }

    WebpAnimResult result;
    result.frames = m_frames;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        WebPDataClear(&data);
        m_error = QStringLiteral("webp: cannot write %1").arg(path);
        return failWebp(m_error);
    }
    const qint64 written =
        out.write(reinterpret_cast<const char*>(data.bytes),
                  static_cast<qint64>(data.size));
    result.bytes = static_cast<qint64>(data.size);
    WebPDataClear(&data);
    if (written != result.bytes) {
        m_error = QStringLiteral("webp: short write on %1").arg(path);
        return failWebp(m_error);
    }
    result.ok = true;
    return result;
}

WebpAnimResult writeWebpAnim(const std::vector<QImage>& frames,
                             int frameDurationMs, int loopCount,
                             const QString& path, float quality)
{
    if (frames.empty())
        return failWebp(QStringLiteral("webp: no frames to encode"));
    if (frameDurationMs <= 0)
        return failWebp(QStringLiteral("webp: frame duration must be > 0"));

    WebpAnimStream stream(frames.front().width(), frames.front().height(),
                          loopCount, quality);
    for (size_t i = 0; i < frames.size(); ++i)
        if (!stream.addFrame(frames[i],
                             static_cast<int>(i) * frameDurationMs))
            return failWebp(stream.error());
    return stream.finish(path, static_cast<int>(frames.size())
                                   * frameDurationMs);
}

} // namespace viki
