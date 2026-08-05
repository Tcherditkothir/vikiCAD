// Offscreen frame rendering (offscreen/OffscreenRenderer.h). These tests
// need a reachable X display for GLX; when there is none (CI container)
// the renderer reports invalid and the cases SKIP instead of failing —
// the raster path is then covered by the local run only.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "OffscreenRenderer.h"
#include "anim/AnimClip.h"
#include "anim/Avatar.h"
#include "anim/Chain.h"

using namespace viki::anim;

namespace {

QString goldenPath(const char* name)
{
    return QStringLiteral(VIKICAD_GOLDEN_DIR "/anim/")
           + QLatin1String(name);
}

struct Scene {
    Chain chain;
    AnimClip clip;
    AvatarSpec avatar;
};

Scene tadasana()
{
    const AvatarResult avatar =
        loadAvatarFile(goldenPath("manikin-neutral.json"));
    REQUIRE(avatar.ok);
    const ChainResult chain =
        loadChainFile(goldenPath("humanoid-12.json"), avatar.spec.heightM);
    REQUIRE(chain.ok);
    const ClipResult clip = loadClipFile(
        goldenPath("pose3d/yog-tadasana.json"), chain.chain);
    REQUIRE(clip.ok);
    return {chain.chain, clip.clip, avatar.spec};
}

std::vector<QImage> renderAll(OffscreenRenderer& renderer,
                              const Scene& scene,
                              const RenderOptions& options)
{
    std::vector<QImage> frames;
    const RigidAvatarProvider provider(scene.avatar);
    const RenderClipResult res = renderer.renderClip(
        scene.chain, scene.clip, provider, scene.avatar, options,
        [&frames](int, const QImage& img) {
            frames.push_back(img.copy());
            return true;
        });
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    return frames;
}

} // namespace

TEST_CASE("offscreen frames have alpha background and opaque body",
          "[anim][render]")
{
    OffscreenRenderer renderer;
    if (!renderer.valid())
        SKIP("no GL/X display: " << renderer.initError().toStdString());

    Scene scene = tadasana();
    RenderOptions options;
    options.width = 96;
    options.height = 120;
    const auto frames = renderAll(renderer, scene, options);
    REQUIRE(frames.size() == scene.clip.frameTimes().size());

    const QImage& first = frames.front();
    REQUIRE(first.size() == QSize(96, 120));
    // Background corner: fully transparent.
    CHECK(qAlpha(first.pixel(2, 2)) == 0);
    // Somewhere in the frame the avatar is opaque.
    int opaque = 0;
    for (int y = 0; y < first.height(); ++y)
        for (int x = 0; x < first.width(); ++x)
            if (qAlpha(first.pixel(x, y)) > 200)
                ++opaque;
    CHECK(opaque > 100);
}

TEST_CASE("offscreen rendering is deterministic across runs",
          "[anim][render]")
{
    OffscreenRenderer renderer;
    if (!renderer.valid())
        SKIP("no GL/X display: " << renderer.initError().toStdString());

    Scene scene = tadasana();
    RenderOptions options;
    options.width = 96;
    options.height = 120;
    const auto a = renderAll(renderer, scene, options);
    const auto b = renderAll(renderer, scene, options);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
        REQUIRE(a[i] == b[i]);
}

TEST_CASE("side and front cameras see different silhouettes",
          "[anim][render]")
{
    OffscreenRenderer renderer;
    if (!renderer.valid())
        SKIP("no GL/X display: " << renderer.initError().toStdString());

    Scene scene = tadasana();
    RenderOptions side;
    side.width = 96;
    side.height = 120;
    RenderOptions front = side;
    front.camera = CameraView::Front;
    const auto sideFrames = renderAll(renderer, scene, side);
    const auto frontFrames = renderAll(renderer, scene, front);
    REQUIRE(sideFrames.size() == frontFrames.size());
    // Standing straight, the front view is clearly wider (shoulders/arms)
    // than the side view: count opaque columns.
    const auto opaqueColumns = [](const QImage& img) {
        int cols = 0;
        for (int x = 0; x < img.width(); ++x) {
            for (int y = 0; y < img.height(); ++y) {
                if (qAlpha(img.pixel(x, y)) > 200) {
                    ++cols;
                    break;
                }
            }
        }
        return cols;
    };
    CHECK(opaqueColumns(frontFrames.front())
          > opaqueColumns(sideFrames.front()));
}
