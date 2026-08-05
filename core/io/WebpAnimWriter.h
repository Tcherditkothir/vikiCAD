#pragma once

#include <vector>

#include <QImage>
#include <QString>

namespace viki {

struct WebpAnimResult {
    bool ok = false;
    QString error;
    int frames = 0;
    qint64 bytes = 0;
};

// Animated WebP assembly (libwebpmux's WebPAnimEncoder — the same engine
// behind img2webp, linked directly so the export has no external-binary
// dependency). Frames must share one size; alpha survives (lossy WebP
// carries an alpha plane), which is what puts the avatar loops on a
// transparent background in the app.
//
// `loopCount` follows the WebP convention: 0 loops forever, 1 plays once
// and freezes on the last frame — the natural encoding of the pose3d loop
// modes (pingpong/cycle -> 0, hold -> 1).
//
// Streaming variant: frames are encoded AS THEY ARRIVE, so a long clip
// never holds all its RGBA frames in memory (240 frames of 512x640 would
// be ~300 MB). writeWebpAnim below is the convenience wrapper over it.
class WebpAnimStream {
public:
    WebpAnimStream(int width, int height, int loopCount,
                   float quality = 80.0f);
    ~WebpAnimStream();
    WebpAnimStream(const WebpAnimStream&) = delete;
    WebpAnimStream& operator=(const WebpAnimStream&) = delete;

    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    int frames() const { return m_frames; }

    // Timestamps must be strictly increasing; each frame lasts until the
    // next timestamp (the final frame until `endTimestampMs` of finish()).
    bool addFrame(const QImage& frame, int timestampMs);
    WebpAnimResult finish(const QString& path, int endTimestampMs);

private:
    struct Impl;
    Impl* m_impl = nullptr;
    QString m_error;
    int m_width = 0;
    int m_height = 0;
    int m_frames = 0;
};

WebpAnimResult writeWebpAnim(const std::vector<QImage>& frames,
                             int frameDurationMs, int loopCount,
                             const QString& path, float quality = 80.0f);

} // namespace viki
