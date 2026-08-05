// pose3d v1 parsing + sampling (core/anim/AnimClip.h): carry-forward
// densification, slerp, loop frame lists, breath, joint-stop warnings.
// Test data is deliberately ASYMMETRIC where geometry is involved (DEVLOG
// 2026-08-03: a symmetric vector can green-light the wrong axis).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "anim/AnimClip.h"
#include "anim/Chain.h"
#include "anim/ForwardKinematics.h"

using Catch::Approx;
using namespace viki::anim;

namespace {

Chain humanoid()
{
    const ChainResult res = loadChainFile(
        QStringLiteral(VIKICAD_GOLDEN_DIR "/anim/humanoid-12.json"));
    REQUIRE(res.ok);
    return res.chain;
}

ClipResult parseClip(const Chain& chain, const char* json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    REQUIRE(doc.isObject());
    return clipFromJson(doc.object(), chain);
}

gp_Vec rotate(const gp_Quaternion& q, const gp_Vec& v)
{
    return q.Multiply(v);
}

} // namespace

TEST_CASE("euler XYZ is extrinsic, X applied first", "[anim]")
{
    const Chain chain = humanoid();
    const ClipResult res = parseClip(chain, R"({
        "id":"euler-order","schema_version":"1","chain":"humanoid-12",
        "fps":24,
        "keyframes":[{"t":0,"joints":{"spine":[90,0,90]}}]})");
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    const int spine = chain.indexOf(QStringLiteral("spine"));
    const gp_Quaternion q = res.clip.keys[0].values[spine].rot;
    // Extrinsic X then Z on +Z: Rx(90) sends +Z to -Y, Rz(90) sends -Y to +X.
    const gp_Vec v = rotate(q, gp_Vec(0, 0, 1));
    CHECK(v.X() == Approx(1.0).margin(1e-9));
    CHECK(v.Y() == Approx(0.0).margin(1e-9));
    CHECK(v.Z() == Approx(0.0).margin(1e-9));
}

TEST_CASE("unspecified channels carry forward from the previous keyframe",
          "[anim]")
{
    const Chain chain = humanoid();
    const ClipResult res = parseClip(chain, R"({
        "id":"carry","schema_version":"1","chain":"humanoid-12","fps":24,
        "keyframes":[
            {"t":0, "joints":{"forearm_l":[-90,0,0]}},
            {"t":1, "joints":{"spine":[0,0,30]}},
            {"t":2, "joints":{"forearm_l":[-30,0,0]}}
        ]})");
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    const AnimClip& clip = res.clip;
    const int forearm = chain.indexOf(QStringLiteral("forearm_l"));
    REQUIRE(clip.keys.size() == 3);

    // Keyframe 1 does not mention the forearm: it must still hold -90 deg.
    gp_Quaternion expect90;
    expect90.SetEulerAngles(gp_Extrinsic_XYZ, -M_PI / 2, 0, 0);
    CHECK(clip.keys[1].values[forearm].rot.IsEqual(expect90));

    // Between t=0 and t=1 the forearm is CONSTANT (carry-forward semantics,
    // not sparse-track interpolation toward the t=2 value). Compare by
    // rotating a vector: slerp output is not bitwise-identical to its
    // endpoints, so a strict quaternion IsEqual would be flaky.
    const PoseSample mid = clip.sampleAt(0.5);
    const gp_Vec vm = rotate(mid.values[forearm].rot, gp_Vec(0, 0, 1));
    CHECK(vm.X() == Approx(0.0).margin(1e-9));
    CHECK(vm.Y() == Approx(1.0).margin(1e-9)); // Rx(-90) sends +Z to +Y
    CHECK(vm.Z() == Approx(0.0).margin(1e-9));

    // Between t=1 and t=2 it slerps -90 -> -30: at t=1.5 that is -60 deg
    // about X; -60 deg sends +Z to (0, +sin60, +cos60).
    const PoseSample late = clip.sampleAt(1.5);
    const gp_Vec v = rotate(late.values[forearm].rot, gp_Vec(0, 0, 1));
    CHECK(v.X() == Approx(0.0).margin(1e-9));
    CHECK(v.Y() == Approx(std::sin(M_PI / 3)).margin(1e-9));
    CHECK(v.Z() == Approx(0.5).margin(1e-9));
}

TEST_CASE("slerp midpoint halves a single-axis rotation", "[anim]")
{
    const Chain chain = humanoid();
    const ClipResult res = parseClip(chain, R"({
        "id":"slerp","schema_version":"1","chain":"humanoid-12","fps":24,
        "keyframes":[
            {"t":0},
            {"t":2, "joints":{"neck":[0,0,90]}}
        ]})");
    REQUIRE(res.ok);
    const int neck = chain.indexOf(QStringLiteral("neck"));
    const PoseSample mid = res.clip.sampleAt(1.0);
    // 45 deg about Z sends +X to (cos45, sin45, 0).
    const gp_Vec v = rotate(mid.values[neck].rot, gp_Vec(1, 0, 0));
    CHECK(v.X() == Approx(std::sqrt(0.5)).margin(1e-9));
    CHECK(v.Y() == Approx(std::sqrt(0.5)).margin(1e-9));
    CHECK(v.Z() == Approx(0.0).margin(1e-9));
}

TEST_CASE("root defaults to the rest attach and root_pos is absolute",
          "[anim]")
{
    const Chain chain = humanoid();
    const ClipResult res = parseClip(chain, R"({
        "id":"root","schema_version":"1","chain":"humanoid-12","fps":24,
        "keyframes":[
            {"t":0},
            {"t":1, "root_pos":[0.011, -0.022, 0.05],
                    "root_rot":[0, 0, 90]}
        ]})");
    REQUIRE(res.ok);
    // Rest: standing pelvis height 0.531 x 1750 mm.
    CHECK(res.clip.keys[0].rootPosMm.Z() == Approx(0.531 * 1750.0));
    CHECK(res.clip.keys[0].rootPosMm.X() == Approx(0.0));
    // Keyframe 1: metres -> millimetres, ABSOLUTE (not an offset).
    CHECK(res.clip.keys[1].rootPosMm.X() == Approx(11.0));
    CHECK(res.clip.keys[1].rootPosMm.Y() == Approx(-22.0));
    CHECK(res.clip.keys[1].rootPosMm.Z() == Approx(50.0));
    const gp_Vec f = rotate(res.clip.keys[1].rootRot, gp_Vec(1, 0, 0));
    CHECK(f.Y() == Approx(1.0).margin(1e-9));
}

TEST_CASE("frame lists follow the loop mode", "[anim]")
{
    const Chain chain = humanoid();
    const char* base = R"({
        "id":"loops","schema_version":"1","chain":"humanoid-12","fps":24,
        "keyframes":[{"t":0},{"t":2,"joints":{"spine":[10,4,7]}}]%1})";

    SECTION("default = hold over [0, duration]")
    {
        const ClipResult r = parseClip(
            chain, QString(QLatin1String(base))
                       .arg(QString())
                       .toUtf8()
                       .constData());
        REQUIRE(r.ok);
        const auto times = r.clip.frameTimes();
        REQUIRE(times.size() == 49); // 2 s at 24 fps, both ends included
        CHECK(times.front() == Approx(0.0));
        CHECK(times.back() == Approx(2.0));
    }
    SECTION("cycle drops the end frame")
    {
        const ClipResult r = parseClip(
            chain,
            QString(QLatin1String(base))
                .arg(QStringLiteral(",\"loop\":{\"mode\":\"cycle\"}"))
                .toUtf8()
                .constData());
        REQUIRE(r.ok);
        const auto times = r.clip.frameTimes();
        REQUIRE(times.size() == 48);
        CHECK(times.back() == Approx(2.0 - 1.0 / 24.0));
    }
    SECTION("pingpong bakes the way back without duplicated ends")
    {
        const ClipResult r = parseClip(
            chain,
            QString(QLatin1String(base))
                .arg(QStringLiteral(",\"loop\":{\"mode\":\"pingpong\"}"))
                .toUtf8()
                .constData());
        REQUIRE(r.ok);
        const auto times = r.clip.frameTimes();
        REQUIRE(times.size() == 96); // 49 forward + 47 back
        CHECK(times[48] == Approx(2.0));       // the turn-around
        CHECK(times[49] == Approx(2.0 - 1.0 / 24.0));
        CHECK(times.back() == Approx(1.0 / 24.0)); // start not repeated
    }
    SECTION("windowed cycle honours start/end")
    {
        const ClipResult r = parseClip(
            chain,
            QString(QLatin1String(base))
                .arg(QStringLiteral(
                    ",\"loop\":{\"mode\":\"cycle\",\"start\":0.5,"
                    "\"end\":1.5}"))
                .toUtf8()
                .constData());
        REQUIRE(r.ok);
        const auto times = r.clip.frameTimes();
        REQUIRE(times.size() == 24);
        CHECK(times.front() == Approx(0.5));
        CHECK(times.back() == Approx(0.5 + 23.0 / 24.0));
    }
    SECTION("loop.end beyond the last keyframe is refused")
    {
        const ClipResult r = parseClip(
            chain,
            QString(QLatin1String(base))
                .arg(QStringLiteral(
                    ",\"loop\":{\"mode\":\"cycle\",\"end\":5}"))
                .toUtf8()
                .constData());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("loop.end")));
    }
}

TEST_CASE("breath adds a local oscillation that can be ignored", "[anim]")
{
    const Chain chain = humanoid();
    const ClipResult res = parseClip(chain, R"({
        "id":"breath","schema_version":"1","chain":"humanoid-12","fps":24,
        "keyframes":[{"t":0},{"t":4,"joints":{"spine":[15,5,0]}}],
        "breath":{"period_s":4,"amplitude_deg":2,"joints":["spine"]}})");
    REQUIRE(res.ok);
    REQUIRE(res.clip.hasBreath);
    const int spine = chain.indexOf(QStringLiteral("spine"));
    // sin peaks at t = period/4 = 1 s.
    const PoseSample with = res.clip.sampleAt(1.0, true);
    const PoseSample without = res.clip.sampleAt(1.0, false);
    CHECK_FALSE(with.values[spine].rot.IsEqual(without.values[spine].rot));
    // The added rotation is exactly amplitude_deg about local X at the peak.
    const gp_Quaternion delta =
        without.values[spine].rot.Inverted() * with.values[spine].rot;
    gp_Vec axis;
    Standard_Real angle = 0;
    delta.GetVectorAndAngle(axis, angle);
    CHECK(angle == Approx(2.0 * M_PI / 180.0).margin(1e-9));
    CHECK(std::abs(axis.X()) == Approx(1.0).margin(1e-9));
}

TEST_CASE("stop violations warn without clamping", "[anim]")
{
    const Chain chain = humanoid();
    // shin_l x stop is [-155, 0]: +10 deg hyper-extends the knee.
    const ClipResult res = parseClip(chain, R"({
        "id":"limits","schema_version":"1","chain":"humanoid-12","fps":24,
        "keyframes":[{"t":0,"joints":{"shin_l":[10,0,0]}}]})");
    REQUIRE(res.ok);
    REQUIRE_FALSE(res.warnings.isEmpty());
    CHECK(res.warnings.first().contains(QStringLiteral("shin_l.x")));
    // Not clamped: the value renders as authored.
    const int shin = chain.indexOf(QStringLiteral("shin_l"));
    const gp_Vec v =
        rotate(res.clip.keys[0].values[shin].rot, gp_Vec(0, 0, 1));
    // Rx(+10 deg) on +Z: y' = -z sin(10), so Y goes negative.
    CHECK(v.Y() == Approx(-std::sin(10.0 * M_PI / 180.0)).margin(1e-9));
}

TEST_CASE("all 15 pilot poses parse, sample and solve", "[anim]")
{
    // golden/anim/pose3d/ mirrors GenMov3D data/pose3d/ (2026-08-05): the
    // real production inputs of the pipeline, not synthetic fixtures.
    const Chain chain = humanoid();
    QDir dir(QStringLiteral(VIKICAD_GOLDEN_DIR "/anim/pose3d"));
    const QStringList files =
        dir.entryList(QStringList() << QStringLiteral("*.json"),
                      QDir::Files, QDir::Name);
    REQUIRE(files.size() == 15);
    for (const QString& f : files) {
        DYNAMIC_SECTION(f.toStdString())
        {
            const ClipResult res =
                loadClipFile(dir.filePath(f), chain);
            INFO(res.error.toStdString());
            REQUIRE(res.ok);
            REQUIRE_FALSE(res.clip.keys.empty());
            // Sample midway and at both ends; every joint origin must come
            // out finite (a NaN anywhere would poison meshing and rendering
            // silently).
            for (const double t :
                 {0.0, res.clip.duration() * 0.5, res.clip.duration()}) {
                const auto world =
                    worldTransforms(chain, res.clip.sampleAt(t));
                for (size_t j = 0; j < world.size(); ++j) {
                    const gp_Pnt p =
                        jointOrigin(world, static_cast<int>(j));
                    CHECK(std::isfinite(p.X()));
                    CHECK(std::isfinite(p.Y()));
                    CHECK(std::isfinite(p.Z()));
                }
            }
            // The pipeline generated these WITH the anatomical stops applied,
            // so none of them should trip a stop warning.
            INFO(res.warnings.join(QStringLiteral(" | ")).toStdString());
            CHECK(res.warnings.isEmpty());
        }
    }
}

TEST_CASE("clip validation refuses broken inputs", "[anim]")
{
    const Chain chain = humanoid();
    SECTION("chain id mismatch")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"robot-4","fps":24,
            "keyframes":[{"t":0}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("robot-4")));
    }
    SECTION("fps out of range")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"humanoid-12","fps":8,
            "keyframes":[{"t":0}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("fps")));
    }
    SECTION("unknown joint")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"humanoid-12","fps":24,
            "keyframes":[{"t":0,"joints":{"tail":[1,2,3]}}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("tail")));
    }
    SECTION("ball joint with a scalar")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"humanoid-12","fps":24,
            "keyframes":[{"t":0,"joints":{"spine":45}}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("ball")));
    }
    SECTION("free root driven through joints{}")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"humanoid-12","fps":24,
            "keyframes":[{"t":0,"joints":{"pelvis":[0,0,10]}}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("root_pos")));
    }
    SECTION("duplicate keyframe times")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"humanoid-12","fps":24,
            "keyframes":[{"t":1},{"t":1}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("non-increasing")));
    }
    SECTION("quoted numbers in a ball array are refused, not read as 0")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"humanoid-12","fps":24,
            "keyframes":[{"t":0,"joints":{"spine":["90",0,0]}}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("numbers")));
    }
    SECTION("id with path characters is refused (traversal guard)")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"../evil","schema_version":"1","chain":"humanoid-12",
            "fps":24,"keyframes":[{"t":0}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("id")));
    }
    SECTION("clips beyond the 300 s cap are refused")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"humanoid-12","fps":24,
            "keyframes":[{"t":0},{"t":301}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("300")));
    }
    SECTION("loop bounds of the wrong JSON type are refused")
    {
        const ClipResult r = parseClip(chain, R"({
            "id":"x","schema_version":"1","chain":"humanoid-12","fps":24,
            "keyframes":[{"t":0},{"t":1}],
            "loop":{"mode":"cycle","start":"0.5"}})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("loop.start")));
    }
}

TEST_CASE("breath closes exactly at a cycle loop seam", "[anim]")
{
    // Review 2026-08-05: a sine on absolute time popped at the cycle seam
    // whenever the window was not a multiple of the period. The phase now
    // folds onto a whole number of periods per loop.
    const Chain chain = humanoid();
    const ClipResult res = parseClip(chain, R"({
        "id":"seam","schema_version":"1","chain":"humanoid-12","fps":24,
        "keyframes":[{"t":0},{"t":3,"joints":{"neck":[0,0,10]}}],
        "loop":{"mode":"cycle","start":0,"end":3},
        "breath":{"period_s":4,"amplitude_deg":2,"joints":["spine"]}})");
    REQUIRE(res.ok);
    const int spine = chain.indexOf(QStringLiteral("spine"));
    const PoseSample atStart = res.clip.sampleAt(0.0, true);
    const PoseSample atEnd = res.clip.sampleAt(3.0, true);
    // The spine holds its keyframed value at both ends of the window, so
    // any difference would be breath phase — there must be none.
    const gp_Vec vs = atStart.values[spine].rot.Multiply(gp_Vec(0, 0, 1));
    const gp_Vec ve = atEnd.values[spine].rot.Multiply(gp_Vec(0, 0, 1));
    CHECK(vs.X() == Approx(ve.X()).margin(1e-9));
    CHECK(vs.Y() == Approx(ve.Y()).margin(1e-9));
    CHECK(vs.Z() == Approx(ve.Z()).margin(1e-9));
    // And the oscillation still exists inside the window (peak at 1/4 of
    // the folded period = 0.75 s for one cycle over 3 s).
    const PoseSample mid = res.clip.sampleAt(0.75, true);
    const PoseSample midPlain = res.clip.sampleAt(0.75, false);
    CHECK_FALSE(
        mid.values[spine].rot.IsEqual(midPlain.values[spine].rot));
}
