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

    // The fix-up node carries the Z-up-mm -> Y-up-m conversion with the
    // avatar FACING glTF +Z (review 2026-08-05: a bare Rx(-90) kept the
    // up axis right but showed the manikin's back). Quaternion
    // (0, √.5, √.5, 0) = Ry(180)·Rx(-90).
    const cgltf_node& fix = glb.data->nodes[0];
    CHECK(fix.has_scale);
    CHECK(fix.scale[0] == Approx(0.001));
    CHECK(fix.has_rotation);
    CHECK(fix.rotation[0] == Approx(0.0).margin(1e-6));
    CHECK(fix.rotation[1] == Approx(std::sqrt(0.5)).margin(1e-6));
    CHECK(fix.rotation[2] == Approx(std::sqrt(0.5)).margin(1e-6));
    CHECK(fix.rotation[3] == Approx(0.0).margin(1e-6));
    // Sanity of that quaternion: our front +Y -> glTF front +Z, our up
    // +Z -> glTF up +Y.
    const gp_Quaternion q(fix.rotation[0], fix.rotation[1],
                          fix.rotation[2], fix.rotation[3]);
    const gp_Vec front = q.Multiply(gp_Vec(0, 1, 0));
    CHECK(front.Z() == Approx(1.0).margin(1e-9));
    const gp_Vec up = q.Multiply(gp_Vec(0, 0, 1));
    CHECK(up.Y() == Approx(1.0).margin(1e-9));

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

TEST_CASE("sculpted avatar with hands exports a valid GLB", "[anim]")
{
    // The sculpted build emits COMPOUND parts (fusiform tube + two cap
    // spheres) plus torso loft, pelvis blob, deltoids and knuckles: the
    // exporter's face walk must tessellate all of them and cgltf must
    // still validate the result (review coverage gap).
    const AvatarResult avatar =
        loadAvatarFile(goldenPath("manikin-sculpte-mains.json"));
    REQUIRE(avatar.ok);
    const ChainResult chain =
        loadChainFile(goldenPath("humanoid-14.json"), avatar.spec.heightM);
    REQUIRE(chain.ok);
    const ClipResult clip = loadClipFile(
        goldenPath("pose3d/hand-demo.json"), chain.chain);
    REQUIRE(clip.ok);
    const RigidAvatarProvider provider(avatar.spec);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("hands.glb"));
    const GlbResult res = exportGlb(chain.chain, clip.clip, provider,
                                    avatar.spec, path);
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    // 14 limb parts + torso extras (2 deltoids, pivot sphere, pelvis
    // blob, head) + knuckles: well past the capsule build's 13 meshes.
    CHECK(res.meshes > 20);
    CHECK(res.triangles > 5000);

    Loaded glb;
    parseAndValidate(path, glb);
    // Both mittens exist as named nodes and the wrists are animated.
    bool handNode = false;
    for (cgltf_size i = 0; i < glb.data->nodes_count; ++i)
        if (glb.data->nodes[i].name
            && QString::fromUtf8(glb.data->nodes[i].name)
                   .contains(QStringLiteral("hand_l")))
            handNode = true;
    CHECK(handNode);
    REQUIRE(glb.data->animations_count == 1);
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

TEST_CASE("pingpong bakes the LOOP WINDOW, not the whole clip", "[anim]")
{
    // Review 2026-08-05: the GLB used to mirror the full clip while the
    // WebP honoured loop.start/loop.end — the two contractual outputs
    // told different stories. Keys at 0/2/4 s, window [1, 3]: one GLB
    // playthrough must span 2 x (3-1) = 4 s starting at 0.
    const AvatarResult avatar =
        loadAvatarFile(goldenPath("manikin-neutral.json"));
    REQUIRE(avatar.ok);
    const ChainResult chain =
        loadChainFile(goldenPath("humanoid-12.json"), avatar.spec.heightM);
    REQUIRE(chain.ok);
    const char* poseJson = R"({
        "id":"windowed","schema_version":"1","chain":"humanoid-12",
        "fps":24,
        "keyframes":[{"t":0},{"t":2,"joints":{"neck":[0,0,40]}},
                     {"t":4,"joints":{"neck":[0,0,0]}}],
        "loop":{"mode":"pingpong","start":1,"end":3}})";
    const ClipResult clip = clipFromJson(
        QJsonDocument::fromJson(poseJson).object(), chain.chain);
    REQUIRE(clip.ok);
    const RigidAvatarProvider provider(avatar.spec);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("w.glb"));
    REQUIRE(exportGlb(chain.chain, clip.clip, provider, avatar.spec, path)
                .ok);

    Loaded glb;
    parseAndValidate(path, glb);
    REQUIRE(glb.data->animations_count == 1);
    const cgltf_accessor* input =
        glb.data->animations[0].samplers[0].input;
    REQUIRE(input != nullptr);
    CHECK(input->has_min);
    CHECK(input->min[0] == Approx(0.0).margin(1e-4));
    CHECK(input->has_max);
    CHECK(input->max[0] == Approx(4.0).margin(1e-4));
    // Window boundary samples + the interior key at t=2, mirrored:
    // [0, 1, 2, 3, 4] relative -> 5 input times.
    CHECK(input->count == 5u);
}

TEST_CASE("a typed root joint gets composed animation channels", "[anim]")
{
    // The lever case from the review: a single revolute root must move in
    // the GLB too (rotation channel with non-identity quaternions).
    const char* leverJson = R"({
        "id": "lever", "schema_version": "1", "scale_reference": 1.0,
        "joints": [
            { "name": "lever", "parent": null, "type": "revolute",
              "axis": [0, 1, 0], "attach": [0.01, 0.02, 0.03],
              "length": 0.1, "rest_direction": [1, 0, 0] }
        ]})";
    const ChainResult chain =
        chainFromJson(QJsonDocument::fromJson(leverJson).object());
    REQUIRE(chain.ok);
    const char* poseJson = R"({
        "id":"swing","schema_version":"1","chain":"lever","fps":24,
        "keyframes":[{"t":0,"joints":{"lever":0}},
                     {"t":1,"joints":{"lever":80}}]})";
    const ClipResult clip = clipFromJson(
        QJsonDocument::fromJson(poseJson).object(), chain.chain);
    REQUIRE(clip.ok);

    AvatarSpec spec;
    spec.id = QStringLiteral("stub");
    spec.chainId = QStringLiteral("lever");
    spec.segmentRadiusFrac.insert(QStringLiteral("default"), 0.01);
    const RigidAvatarProvider provider(spec);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("lever.glb"));
    REQUIRE(exportGlb(chain.chain, clip.clip, provider, spec, path).ok);

    Loaded glb;
    parseAndValidate(path, glb);
    REQUIRE(glb.data->animations_count == 1);
    const cgltf_animation& anim = glb.data->animations[0];
    const cgltf_accessor* rotOut = nullptr;
    for (cgltf_size i = 0; i < anim.channels_count; ++i)
        if (anim.channels[i].target_path
            == cgltf_animation_path_type_rotation)
            rotOut = anim.channels[i].sampler->output;
    REQUIRE(rotOut != nullptr);
    REQUIRE(rotOut->count == 2u);
    cgltf_float first[4] = {0, 0, 0, 0};
    cgltf_float last[4] = {0, 0, 0, 0};
    REQUIRE(cgltf_accessor_read_float(rotOut, 0, first, 4));
    REQUIRE(cgltf_accessor_read_float(rotOut, 1, last, 4));
    // Key 0: identity. Key 1: 80 deg about +Y -> y = sin(40 deg).
    CHECK(first[3] == Approx(1.0).margin(1e-6));
    CHECK(last[1]
          == Approx(std::sin(40.0 * M_PI / 180.0)).margin(1e-5));
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
