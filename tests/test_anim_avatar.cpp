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
    const double neckLen = 0.220 * 1750.0;
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
