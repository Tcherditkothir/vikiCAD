#pragma once

#include <functional>
#include <memory>

#include <QImage>
#include <QString>

#include "anim/AnimClip.h"
#include "anim/Avatar.h"
#include "anim/Chain.h"

namespace viki {
namespace anim {

// Headless 3D rendering of an animated chain to RGBA frames with a
// TRANSPARENT background — the raster half of `vikicad-cli anim render`.
//
// Lives in its own static library (vikioffscreen), NOT in vikicore: core
// stays visualization-free by architecture rule; only targets that render
// link TKV3d/TKService/TKOpenGl. The GL surface is an OCCT virtual
// Xw_Window (never mapped) with buffersNoSwap; the background alpha comes
// from buffersOpaqueAlpha=false + a Graphic3d_BT_RGBA dump — probed on
// this machine before this class was written: background pixels dump at
// alpha 0, geometry at alpha 1, no matting tricks.
//
// Requires a reachable X display (GLX). Construction reports failure
// cleanly instead of throwing, so CI (no display) can SKIP.

enum class CameraView {
    Side,         // eye on +X: the Y-Z silhouette the pipeline predicts
    Front,        // eye on +Y: facing the avatar's front
    ThreeQuarter, // ~30 deg azimuth off front, slightly elevated
};

struct RenderOptions {
    int width = 512;
    int height = 640;
    CameraView camera = CameraView::Side;
    // FitAll margin around the whole-animation bounding box. The camera is
    // framed ONCE over the union of every frame's bbox (floor z=0
    // included), then never moves: frames are directly comparable and the
    // ground stays at the bottom of the image.
    double margin = 0.06;
    bool applyBreath = true;
    double deflectionMm = 0.8;
};

struct RenderClipResult {
    bool ok = false;
    QString error;
    int frames = 0;
};

class OffscreenRenderer {
public:
    OffscreenRenderer();
    ~OffscreenRenderer();
    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;

    // False when no GL/X context could be built; initError() says why.
    bool valid() const;
    QString initError() const;

    // Renders every frame time of clip.frameTimes() and hands each RGBA
    // image to `sink` (frame index, image). A sink returning false aborts
    // cleanly. The scene is torn down afterwards; the renderer can be
    // reused for another clip.
    RenderClipResult renderClip(
        const Chain& chain, const AnimClip& clip,
        const AvatarProvider& provider, const AvatarSpec& avatar,
        const RenderOptions& options,
        const std::function<bool(int, const QImage&)>& sink);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace anim
} // namespace viki
