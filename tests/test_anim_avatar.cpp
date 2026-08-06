// Avatar v1 parsing + rigid manikin geometry (core/anim/Avatar.h).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>

#include "anim/Avatar.h"
#include "anim/Chain.h"

using Catch::Approx;
using namespace viki::anim;

namespace {

QString goldenPath(const char* name)
{
    return QStringLiteral(VIKICAD_GOLDEN_DIR "/anim/")
           + QLatin1String(name);
}

Chain humanoid(std::optional<double> scaleOverrideM = {})
{
    const ChainResult res =
        loadChainFile(goldenPath("humanoid-12.json"), scaleOverrideM);
    REQUIRE(res.ok);
    return res.chain;
}

AvatarSpec manikin()
{
    const AvatarResult res =
        loadAvatarFile(goldenPath("manikin-neutral.json"));
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    return res.spec;
}

bool hasSolid(const TopoDS_Shape& s)
{
    TopExp_Explorer exp(s, TopAbs_SOLID);
    return exp.More();
}

Bnd_Box bboxOf(const TopoDS_Shape& s)
{
    Bnd_Box box;
    BRepBndLib::Add(s, box);
    return box;
}

} // namespace

TEST_CASE("manikin-neutral golden avatar parses", "[anim]")
{
    const AvatarSpec spec = manikin();
    CHECK(spec.id == QStringLiteral("manikin-neutral"));
    CHECK(spec.chainId == QStringLiteral("humanoid-12"));
    CHECK(spec.type == AvatarType::Rigid);
    REQUIRE(spec.heightM.has_value());
    CHECK(*spec.heightM == Approx(1.75));
    CHECK(spec.radiusFracFor(QStringLiteral("spine")) == Approx(0.062));
    CHECK(spec.radiusFracFor(QStringLiteral("nonesuch"))
          == Approx(0.026)); // falls back to default
    CHECK(spec.hasHead);
    CHECK(spec.headRadiusFrac == Approx(0.063));
    CHECK(spec.headElongation == Approx(1.22));
    CHECK(spec.jointStyle == JointStyle::Blend);
    CHECK(spec.baseColor == 0x8FB5A8u);
    CHECK(spec.accentColor == 0xE8DCC8u);
    CHECK(spec.roughness == Approx(0.7));
    CHECK(spec.metallic == Approx(0.0));
}

TEST_CASE("rigid provider builds a capsule per segment", "[anim]")
{
    const Chain chain = humanoid(1.75);
    const AvatarSpec spec = manikin();
    const RigidAvatarProvider provider(spec);

    const int spine = chain.indexOf(QStringLiteral("spine"));
    const auto parts = provider.partsForJoint(chain, spine);
    REQUIRE(parts.size() == 1); // fused capsule
    CHECK(hasSolid(parts[0].shape));
    CHECK_FALSE(parts[0].accent);

    // Capsule bounds: radius 0.062 x 1750 = 108.5 mm around the axis,
    // length 525 mm along +Z plus a rounded cap at each end.
    double xmin, ymin, zmin, xmax, ymax, zmax;
    bboxOf(parts[0].shape).Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double r = 0.062 * 1750.0;
    const double len = 0.300 * 1750.0;
    CHECK(zmin == Approx(-r).margin(1.0));
    CHECK(zmax == Approx(len + r).margin(1.0));
    CHECK(xmax == Approx(r).margin(1.0));
    CHECK(ymin == Approx(-r).margin(1.0));
}

TEST_CASE("the neck carries the ellipsoid head", "[anim]")
{
    const Chain chain = humanoid(1.75);
    const AvatarSpec spec = manikin();
    const RigidAvatarProvider provider(spec);

    const int neck = chain.indexOf(QStringLiteral("neck"));
    const auto parts = provider.partsForJoint(chain, neck);
    REQUIRE(parts.size() == 2); // capsule + head
    const AvatarPart* head = nullptr;
    for (const auto& p : parts)
        if (p.name == QStringLiteral("head"))
            head = &p;
    REQUIRE(head != nullptr);
    CHECK(head->accent);
    CHECK(hasSolid(head->shape));

    // The head floats past the neck tip and is taller than wide
    // (elongation 1.22 along the rest direction +Z). AddOptimal, not Add:
    // the stretched sphere is a B-spline surface and the fast bbox hulls
    // its control points, which overshoots sideways.
    double xmin, ymin, zmin, xmax, ymax, zmax;
    Bnd_Box tight;
    BRepBndLib::AddOptimal(head->shape, tight);
    tight.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double neckLen = 0.095 * 1750.0;
    CHECK(zmax > neckLen);
    const double width = xmax - xmin;
    const double height = zmax - zmin;
    CHECK(height / width == Approx(1.22).margin(0.02));
}

TEST_CASE("point joints carry no geometry", "[anim]")
{
    const Chain chain = humanoid(1.75);
    const RigidAvatarProvider provider(manikin());
    // The pelvis root has length 0: nothing to draw there.
    CHECK(provider.partsForJoint(chain, 0).empty());
}

TEST_CASE("joint_style sphere adds accent balls", "[anim]")
{
    const Chain chain = humanoid(1.75);
    AvatarSpec spec = manikin();
    spec.jointStyle = JointStyle::Sphere;
    const RigidAvatarProvider provider(spec);
    const auto parts =
        provider.partsForJoint(chain,
                               chain.indexOf(QStringLiteral("forearm_l")));
    bool foundBall = false;
    for (const auto& p : parts)
        if (p.name.endsWith(QStringLiteral("_joint")) && p.accent)
            foundBall = true;
    CHECK(foundBall);
}

namespace {

// Sculpted variant of the manikin, matching the extension fields proposed
// for avatar schema v2 (contract entry SCHEMA-AVATAR-V2-DEMANDE).
AvatarSpec sculptedManikin()
{
    const QJsonDocument doc = QJsonDocument::fromJson(R"({
        "id":"manikin-sculpted-test","schema_version":"1",
        "chain":"humanoid-12","type":"rigid","height_m":1.75,
        "rigid":{
          "segment_radius":{"default":0.026,"spine":0.062,"neck":0.020,
            "thigh_l":0.036,"thigh_r":0.036,"shin_l":0.028,"shin_r":0.028,
            "upperarm_l":0.028,"upperarm_r":0.028,
            "forearm_l":0.024,"forearm_r":0.024,
            "foot_l":0.022,"foot_r":0.022},
          "head":{"radius":0.063,"elongation":1.22},
          "joint_style":"blend",
          "build":"sculpted",
          "sculpt":{
            "taper":{"default":0.62,"neck":1.0,
                     "forearm_l":0.72,"forearm_r":0.72},
            "bulge":1.06,
            "torso":{"joint":"spine","hip":0.058,"waist":0.054,
                     "chest":0.066,"shoulder":0.078,"depth":0.62},
            "pelvis":{"joint":"pelvis","width":0.085,"depth":0.042,
                      "drop":0.060},
            "flatten":{"foot_l":0.62,"foot_r":0.62}
          }
        },
        "presentation":{"ground_shadow":0.35},
        "material":{"base_color":"#8FB5A8","accent_color":"#E8DCC8"}})");
    const AvatarResult res = avatarFromJson(doc.object());
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    return res.spec;
}

} // namespace

TEST_CASE("sculpted build parses its extension fields", "[anim]")
{
    const AvatarSpec spec = sculptedManikin();
    CHECK(spec.build == RigidBuild::Sculpted);
    CHECK(spec.sculpt.taperFor(QStringLiteral("shin_l")) == Approx(0.62));
    CHECK(spec.sculpt.taperFor(QStringLiteral("neck")) == Approx(1.0));
    CHECK(spec.sculpt.taperFor(QStringLiteral("forearm_r")) == Approx(0.72));
    REQUIRE(spec.sculpt.torso.has_value());
    CHECK(spec.sculpt.torso->joint == QStringLiteral("spine"));
    CHECK(spec.sculpt.torso->waist == Approx(0.054));
    CHECK(spec.sculpt.torso->depth == Approx(0.62));
    REQUIRE(spec.sculpt.pelvis.has_value());
    CHECK(spec.sculpt.pelvis->width == Approx(0.085));
    CHECK(spec.sculpt.flatten.value(QStringLiteral("foot_l"))
          == Approx(0.62));
    CHECK(spec.groundShadow == Approx(0.35));

    // The golden manikin, with no extension fields, stays on the capsule
    // build — its geometry is untouched by this worksite.
    CHECK(manikin().build == RigidBuild::Capsule);
    CHECK(manikin().groundShadow == Approx(0.0));
}

TEST_CASE("sculpted limbs are single tapered solids", "[anim]")
{
    const Chain chain = humanoid(1.75);
    const RigidAvatarProvider provider(sculptedManikin());

    // Forearm: a leaf segment, tapering by its per-joint ratio 0.72. One
    // fusiform solid + one knuckle sphere at the elbow.
    const auto parts = provider.partsForJoint(
        chain, chain.indexOf(QStringLiteral("forearm_l")));
    REQUIRE(parts.size() == 2);
    const AvatarPart* limb = nullptr;
    const AvatarPart* knuckle = nullptr;
    for (const auto& p : parts) {
        if (p.name == QStringLiteral("forearm_l"))
            limb = &p;
        if (p.name == QStringLiteral("forearm_l_joint"))
            knuckle = &p;
    }
    REQUIRE(limb != nullptr);
    REQUIRE(knuckle != nullptr);
    CHECK(hasSolid(limb->shape));
    CHECK_FALSE(knuckle->accent); // blend style hides it in the base colour

    // rest_direction -Z: the solid spans from the rounded proximal cap
    // just above the elbow down past the distal cap at the wrist.
    const double rP = 0.024 * 1750.0;
    const double rD = rP * 0.72;
    const double len = 0.16 * 1750.0;
    double xmin, ymin, zmin, xmax, ymax, zmax;
    Bnd_Box tight;
    BRepBndLib::AddOptimal(limb->shape, tight);
    tight.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    // Pole ends carry no tangent constraint: the approximated B-spline
    // overshoots the cap by up to ~20 % of the local radius. Cosmetic
    // (slightly deeper rounding), hence the loose margins here.
    CHECK(zmax == Approx(0.88 * rP).margin(0.25 * rP));
    CHECK(zmin == Approx(-(len + 0.88 * rD)).margin(0.25 * rD));
    // Widest near the elbow (proximal radius), so the x extent tracks rP,
    // not the wrist radius.
    CHECK(xmax - xmin == Approx(2.0 * rP).margin(5.0));
}

TEST_CASE("sculpted limbs taper to the child radius at the joint",
          "[anim]")
{
    const Chain chain = humanoid(1.75);
    const RigidAvatarProvider provider(sculptedManikin());

    // The thigh is continued by the shin: its distal radius must be the
    // shin's proximal radius so the bent knee reads as one volume. Probe
    // the geometry: slice the bbox of the thigh solid near the knee.
    const auto parts = provider.partsForJoint(
        chain, chain.indexOf(QStringLiteral("thigh_l")));
    const AvatarPart* limb = nullptr;
    for (const auto& p : parts)
        if (p.name == QStringLiteral("thigh_l"))
            limb = &p;
    REQUIRE(limb != nullptr);

    // Distal cap: the solid ends 0.88 * r_shin past the knee pivot (loose
    // margin: the unconstrained pole overshoots by up to ~20 % of the
    // radius, see the fusiform test).
    const double rShin = 0.028 * 1750.0;
    const double len = 0.245 * 1750.0;
    double xmin, ymin, zmin, xmax, ymax, zmax;
    Bnd_Box tight;
    BRepBndLib::AddOptimal(limb->shape, tight);
    tight.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    CHECK(zmin == Approx(-(len + 0.88 * rShin)).margin(0.25 * rShin));
}

TEST_CASE("sculpted torso: elliptic loft, deltoids, no arm on the axis",
          "[anim]")
{
    const Chain chain = humanoid(1.75);
    const RigidAvatarProvider provider(sculptedManikin());

    const auto parts = provider.partsForJoint(
        chain, chain.indexOf(QStringLiteral("spine")));
    const AvatarPart* trunk = nullptr;
    int deltoids = 0;
    bool neckDeltoid = false;
    for (const auto& p : parts) {
        if (p.name == QStringLiteral("spine"))
            trunk = &p;
        if (p.name.endsWith(QStringLiteral("_deltoid"))) {
            ++deltoids;
            if (p.name.startsWith(QStringLiteral("neck")))
                neckDeltoid = true;
        }
    }
    REQUIRE(trunk != nullptr);
    CHECK(hasSolid(trunk->shape));
    CHECK(deltoids == 2);      // one per arm...
    CHECK_FALSE(neckDeltoid);  // ...but none for the on-axis neck

    // Elliptic sections: depth (Y) is 62 % of width (X).
    double xmin, ymin, zmin, xmax, ymax, zmax;
    Bnd_Box tight;
    BRepBndLib::AddOptimal(trunk->shape, tight);
    tight.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    CHECK((ymax - ymin) / (xmax - xmin) == Approx(0.62).margin(0.05));
    // The loft runs from the hip line up past the neck base (rounded top);
    // profile approximation leaves a few mm of slack at both ends.
    CHECK(zmin == Approx(0.0).margin(8.0));
    // The dome tip may overshoot ~20 mm above the last guide point (cubic
    // fit through the sharply-curving top); the tip is a few mm wide and
    // hides inside the neck column.
    CHECK(zmax == Approx(0.30 * 1750.0 + 0.030 * 1750.0).margin(25.0));
}

TEST_CASE("sculpted root carries the pelvis blob", "[anim]")
{
    const Chain chain = humanoid(1.75);
    const RigidAvatarProvider provider(sculptedManikin());
    const auto parts = provider.partsForJoint(chain, 0);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].name == QStringLiteral("pelvis_pelvis"));
    CHECK(hasSolid(parts[0].shape));

    double xmin, ymin, zmin, xmax, ymax, zmax;
    Bnd_Box tight;
    BRepBndLib::AddOptimal(parts[0].shape, tight);
    tight.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    CHECK(xmax == Approx(0.085 * 1750.0).margin(2.0));
    CHECK(zmin == Approx(-0.060 * 1750.0).margin(2.0));
    // The blob tops out 0.035 * scale above the hip line: it rises into
    // the torso hem so the two meet on a curve, not a ledge.
    CHECK(zmax == Approx(0.035 * 1750.0).margin(2.0));
}

TEST_CASE("sculpted flatten squashes feet, sculpted head seats deeper",
          "[anim]")
{
    const Chain chain = humanoid(1.75);
    const RigidAvatarProvider provider(sculptedManikin());

    // Foot: rest_direction +Y, flatten 0.62 acts on local Z: the foot is
    // wider than tall.
    const auto foot = provider.partsForJoint(
        chain, chain.indexOf(QStringLiteral("foot_l")));
    const AvatarPart* sole = nullptr;
    for (const auto& p : foot)
        if (p.name == QStringLiteral("foot_l"))
            sole = &p;
    REQUIRE(sole != nullptr);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    Bnd_Box tight;
    BRepBndLib::AddOptimal(sole->shape, tight);
    tight.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    CHECK((zmax - zmin) / (xmax - xmin) == Approx(0.62).margin(0.05));

    // Head: still on the neck, accent, seated deeper (0.70 overlap) than
    // the capsule build (0.85).
    const auto neck = provider.partsForJoint(
        chain, chain.indexOf(QStringLiteral("neck")));
    const AvatarPart* head = nullptr;
    for (const auto& p : neck)
        if (p.name == QStringLiteral("head"))
            head = &p;
    REQUIRE(head != nullptr);
    CHECK(head->accent);
}

TEST_CASE("sculpt fields referencing missing joints warn against the "
          "chain", "[anim]")
{
    const Chain chain = humanoid(1.75);
    AvatarSpec spec = sculptedManikin();
    CHECK(avatarChainWarnings(spec, chain).isEmpty());

    spec.sculpt.torso->joint = QStringLiteral("trunk");
    spec.sculpt.flatten.insert(QStringLiteral("tail"), 0.5);
    const QStringList warnings = avatarChainWarnings(spec, chain);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].contains(QStringLiteral("trunk")));
    CHECK(warnings[1].contains(QStringLiteral("tail")));

    // The capsule build has no sculpt fields to check.
    CHECK(avatarChainWarnings(manikin(), chain).isEmpty());
}

TEST_CASE("avatar validation refuses broken inputs", "[anim]")
{
    SECTION("missing rigid block")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid"})");
        const AvatarResult r = avatarFromJson(doc.object());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("rigid")));
    }
    SECTION("segment_radius without default")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid",
            "rigid":{"segment_radius":{"spine":0.06}}})");
        const AvatarResult r = avatarFromJson(doc.object());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("default")));
    }
    SECTION("bad colour warns and falls back")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid",
            "rigid":{"segment_radius":{"default":0.03}},
            "material":{"base_color":"teal"}})");
        const AvatarResult r = avatarFromJson(doc.object());
        REQUIRE(r.ok);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.spec.baseColor == 0x8FB5A8u);
    }
    SECTION("unknown rigid.build")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid",
            "rigid":{"segment_radius":{"default":0.03},
                     "build":"organic"}})");
        const AvatarResult r = avatarFromJson(doc.object());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("build")));
    }
    SECTION("sculpt.taper out of range")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid",
            "rigid":{"segment_radius":{"default":0.03},
                     "build":"sculpted","sculpt":{"taper":0}}})");
        const AvatarResult r = avatarFromJson(doc.object());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("taper")));
    }
    SECTION("sculpt.taper entry as quoted string")
    {
        // A quoted "0.7" silently parses to 0.0 via toDouble — the range
        // check must catch it (review lesson: no silent zeroes).
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid",
            "rigid":{"segment_radius":{"default":0.03},
                     "build":"sculpted",
                     "sculpt":{"taper":{"neck":"0.7"}}}})");
        const AvatarResult r = avatarFromJson(doc.object());
        CHECK_FALSE(r.ok);
    }
    SECTION("sculpt.torso.depth above 1")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid",
            "rigid":{"segment_radius":{"default":0.03},
                     "build":"sculpted",
                     "sculpt":{"torso":{"depth":1.4}}}})");
        const AvatarResult r = avatarFromJson(doc.object());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("depth")));
    }
    SECTION("sculpt.flatten of zero")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid",
            "rigid":{"segment_radius":{"default":0.03},
                     "build":"sculpted",
                     "sculpt":{"flatten":{"foot_l":0}}}})");
        const AvatarResult r = avatarFromJson(doc.object());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("flatten")));
    }
    SECTION("presentation.ground_shadow out of range")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"rigid",
            "rigid":{"segment_radius":{"default":0.03}},
            "presentation":{"ground_shadow":1.5}})");
        const AvatarResult r = avatarFromJson(doc.object());
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("ground_shadow")));
    }
    SECTION("skinned provider refuses for now")
    {
        const QJsonDocument doc = QJsonDocument::fromJson(R"({
            "id":"x","schema_version":"1","chain":"c","type":"skinned",
            "skinned":{"mesh_file":"m.glb"}})");
        const AvatarResult r = avatarFromJson(doc.object());
        REQUIRE(r.ok);
        const ProviderResult p = makeAvatarProvider(r.spec);
        CHECK_FALSE(p.ok);
        CHECK(p.error.contains(QStringLiteral("skinned")));
    }
}
