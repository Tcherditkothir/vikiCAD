// GLB export round-trip through an INDEPENDENT reader (vendored cgltf) —
// LESSONS 2026-07-26: a file writer is judged by a foreign re-read, never
// by its own code agreeing with itself.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "anim/AnimClip.h"
#include "anim/Avatar.h"
#include "anim/Chain.h"
#include "anim/GlbExporter.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

using Catch::Approx;
using namespace viki::anim;

namespace {

QString goldenPath(const char* name)
{
    return QStringLiteral(VIKICAD_GOLDEN_DIR "/anim/")
           + QLatin1String(name);
}

struct Loaded {
    cgltf_data* data = nullptr;
    ~Loaded()
    {
        if (data)
            cgltf_free(data);
    }
};

void parseAndValidate(const QString& path, Loaded& out)
{
    const QByteArray p = path.toLocal8Bit();
    cgltf_options options{};
    REQUIRE(cgltf_parse_file(&options, p.constData(), &out.data)
            == cgltf_result_success);
    REQUIRE(cgltf_load_buffers(&options, out.data, p.constData())
            == cgltf_result_success);
    REQUIRE(cgltf_validate(out.data) == cgltf_result_success);
}

} // namespace

TEST_CASE("manikin + vrksasana exports a valid animated GLB", "[anim]")
{
    const AvatarResult avatar =
        loadAvatarFile(goldenPath("manikin-neutral.json"));
    REQUIRE(avatar.ok);
    const ChainResult chain =
        loadChainFile(goldenPath("humanoid-12.json"), avatar.spec.heightM);
    REQUIRE(chain.ok);
    const ClipResult clip = loadClipFile(
        goldenPath("pose3d/yog-vrksasana.json"), chain.chain);
    REQUIRE(clip.ok);
    const RigidAvatarProvider provider(avatar.spec);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vrksasana.glb"));
    const GlbResult res = exportGlb(chain.chain, clip.clip, provider,
                                    avatar.spec, path);
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    CHECK(res.bytes > 0);
    // 12 segments with a length -> 12 capsules, plus the head.
    CHECK(res.meshes == 13);
    CHECK(res.triangles > 500);

    Loaded glb;
    parseAndValidate(path, glb);
    CHECK(glb.data->meshes_count == 13);
    CHECK(glb.data->materials_count == 2);
    // 1 fix-up + 13 joints + 13 mesh nodes.
    CHECK(glb.data->nodes_count == 27);
    REQUIRE(glb.data->animations_count == 1);

    // The fix-up node carries the Z-up-mm -> Y-up-m conversion.
    const cgltf_node& fix = glb.data->nodes[0];
    CHECK(fix.has_scale);
    CHECK(fix.scale[0] == Approx(0.001));
    CHECK(fix.has_rotation);
    CHECK(fix.rotation[0] == Approx(-std::sqrt(0.5)).margin(1e-6));

    // vrksasana loops pingpong over 4 s: the baked way back stretches the
    // sampler input to 8 s.
    const cgltf_animation& anim = glb.data->animations[0];
    REQUIRE(anim.samplers_count > 0);
    const cgltf_accessor* input = anim.samplers[0].input;
    REQUIRE(input != nullptr);
    CHECK(input->has_max);
    CHECK(input->max[0] == Approx(8.0).margin(1e-4));

    // Spot-check a channel target: joint nodes start at index 1.
    REQUIRE(anim.channels_count > 0);
    bool foundRotation = false;
    for (cgltf_size i = 0; i < anim.channels_count; ++i)
        if (anim.channels[i].target_path
            == cgltf_animation_path_type_rotation)
            foundRotation = true;
    CHECK(foundRotation);
}

TEST_CASE("GLB export is byte-deterministic", "[anim]")
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
    const RigidAvatarProvider provider(avatar.spec);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString pathA = dir.filePath(QStringLiteral("a.glb"));
    const QString pathB = dir.filePath(QStringLiteral("b.glb"));
    REQUIRE(exportGlb(chain.chain, clip.clip, provider, avatar.spec, pathA)
                .ok);
    REQUIRE(exportGlb(chain.chain, clip.clip, provider, avatar.spec, pathB)
                .ok);

    QFile a(pathA), b(pathB);
    REQUIRE(a.open(QIODevice::ReadOnly));
    REQUIRE(b.open(QIODevice::ReadOnly));
    CHECK(a.readAll() == b.readAll());
}

TEST_CASE("channels at rest are skipped", "[anim]")
{
    // A crane where only the boom moves: expect root translation +
    // root rotation + boom rotation = 3 channels, carriage and hook silent.
    const char* craneJson = R"({
        "id": "crane", "schema_version": "1", "scale_reference": 1.0,
        "joints": [
            { "name": "base", "parent": null, "type": "free",
              "attach": [0.03, 0.02, 0.05], "length": 0.13,
              "rest_direction": [0, 0, 1] },
            { "name": "boom", "parent": "base", "type": "revolute",
              "axis": [0, 1, 0], "attach": [0.01, 0.02, 0.13],
              "length": 0.4, "rest_direction": [1, 0, 0] },
            { "name": "carriage", "parent": "boom", "type": "prismatic",
              "axis": [1, 0, 0], "attach": [0.07, 0, 0],
              "length": 0.02, "rest_direction": [1, 0, 0] }
        ]})";
    const ChainResult chain =
        chainFromJson(QJsonDocument::fromJson(craneJson).object());
    REQUIRE(chain.ok);
    const char* poseJson = R"({
        "id":"boom-only","schema_version":"1","chain":"crane","fps":24,
        "keyframes":[{"t":0},{"t":1,"joints":{"boom":45}}]})";
    const ClipResult clip = clipFromJson(
        QJsonDocument::fromJson(poseJson).object(), chain.chain);
    REQUIRE(clip.ok);

    AvatarSpec spec;
    spec.id = QStringLiteral("stub");
    spec.chainId = QStringLiteral("crane");
    spec.segmentRadiusFrac.insert(QStringLiteral("default"), 0.01);
    const RigidAvatarProvider provider(spec);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("crane.glb"));
    const GlbResult res =
        exportGlb(chain.chain, clip.clip, provider, spec, path);
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    CHECK(res.animatedNodes == 2); // root + boom

    Loaded glb;
    parseAndValidate(path, glb);
    REQUIRE(glb.data->animations_count == 1);
    CHECK(glb.data->animations[0].channels_count == 3);
}
