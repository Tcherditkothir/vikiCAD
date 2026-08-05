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

WebpAnimResult writeWebpAnim(const std::vector<QImage>& frames,
                             int frameDurationMs, int loopCount,
                             const QString& path, float quality)
{
    if (frames.empty())
        return failWebp(QStringLiteral("webp: no frames to encode"));
    if (frameDurationMs <= 0)
        return failWebp(QStringLiteral("webp: frame duration must be > 0"));

    const int width = frames.front().width();
    const int height = frames.front().height();
    if (width <= 0 || height <= 0)
        return failWebp(QStringLiteral("webp: empty frame"));

    WebPAnimEncoderOptions encOptions;
    if (!WebPAnimEncoderOptionsInit(&encOptions))
        return failWebp(QStringLiteral("webp: encoder options init failed"));
    encOptions.anim_params.loop_count = loopCount;

    WebPAnimEncoder* enc = WebPAnimEncoderNew(width, height, &encOptions);
    if (!enc)
        return failWebp(QStringLiteral("webp: encoder init failed"));

    WebpAnimResult result;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].width() != width || frames[i].height() != height) {
            WebPAnimEncoderDelete(enc);
            return failWebp(
                QStringLiteral("webp: frame %1 is %2x%3, expected %4x%5")
                    .arg(i)
                    .arg(frames[i].width())
                    .arg(frames[i].height())
                    .arg(width)
                    .arg(height));
        }
        // QImage ARGB32 (non-premultiplied) is the same 0xAARRGGBB packing
        // as WebPPicture's use_argb buffer.
        const QImage frame =
            frames[i].convertToFormat(QImage::Format_ARGB32);

        WebPConfig config;
        if (!WebPConfigInit(&config)) {
            WebPAnimEncoderDelete(enc);
            return failWebp(QStringLiteral("webp: config init failed"));
        }
        config.quality = quality;

        WebPPicture pic;
        if (!WebPPictureInit(&pic)) {
            WebPAnimEncoderDelete(enc);
            return failWebp(QStringLiteral("webp: picture init failed"));
        }
        pic.width = width;
        pic.height = height;
        pic.use_argb = 1;
        if (!WebPPictureAlloc(&pic)) {
            WebPAnimEncoderDelete(enc);
            return failWebp(QStringLiteral("webp: picture alloc failed"));
        }
        for (int y = 0; y < height; ++y)
            std::memcpy(pic.argb + static_cast<size_t>(y)
                                       * static_cast<size_t>(
                                           pic.argb_stride),
                        frame.constScanLine(y),
                        static_cast<size_t>(width) * 4);

        const int timestampMs = static_cast<int>(i) * frameDurationMs;
        const int added = WebPAnimEncoderAdd(enc, &pic, timestampMs,
                                             &config);
        WebPPictureFree(&pic);
        if (!added) {
            const QString detail =
                QString::fromUtf8(WebPAnimEncoderGetError(enc));
            WebPAnimEncoderDelete(enc);
            return failWebp(QStringLiteral("webp: frame %1 rejected (%2)")
                                .arg(i)
                                .arg(detail));
        }
        ++result.frames;
    }

    // Flush: the final NULL frame closes the previous frame's duration.
    WebPAnimEncoderAdd(enc, nullptr,
                       static_cast<int>(frames.size()) * frameDurationMs,
                       nullptr);

    WebPData data;
    WebPDataInit(&data);
    const int assembled = WebPAnimEncoderAssemble(enc, &data);
    WebPAnimEncoderDelete(enc);
    if (!assembled)
        return failWebp(QStringLiteral("webp: assembly failed"));

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        WebPDataClear(&data);
        return failWebp(QStringLiteral("webp: cannot write %1").arg(path));
    }
    const qint64 written =
        out.write(reinterpret_cast<const char*>(data.bytes),
                  static_cast<qint64>(data.size));
    result.bytes = static_cast<qint64>(data.size);
    WebPDataClear(&data);
    if (written != result.bytes)
        return failWebp(QStringLiteral("webp: short write on %1").arg(path));

    result.ok = true;
    return result;
}

} // namespace viki
