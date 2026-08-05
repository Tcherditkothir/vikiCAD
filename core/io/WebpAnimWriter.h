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
WebpAnimResult writeWebpAnim(const std::vector<QImage>& frames,
                             int frameDurationMs, int loopCount,
                             const QString& path, float quality = 80.0f);

} // namespace viki
