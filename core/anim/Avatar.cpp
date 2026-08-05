#include "anim/Avatar.h"

#include <QFile>
#include <QJsonDocument>

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Ax2.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Quaternion.hxx>

#include "io/OcctMessages.h"

namespace viki {
namespace anim {

namespace {

AvatarResult fail(const QString& message)
{
    AvatarResult r;
    r.error = message;
    return r;
}

// "#RRGGBB" -> 0xRRGGBB; falls back to `fallback` with a warning.
quint32 parseColor(const QJsonObject& obj, const QString& key,
                   quint32 fallback, QStringList& warnings)
{
    if (!obj.contains(key))
        return fallback;
    const QString s = obj.value(key).toString();
    bool okParse = false;
    quint32 rgb = 0;
    if (s.size() == 7 && s.startsWith(QLatin1Char('#')))
        rgb = s.mid(1).toUInt(&okParse, 16);
    if (!okParse) {
        warnings.append(QStringLiteral("avatar: material.%1 \"%2\" is not "
                                       "#RRGGBB, using default")
                            .arg(key, s));
        return fallback;
    }
    return rgb;
}

bool hasSolid(const TopoDS_Shape& shape)
{
    if (shape.IsNull())
        return false;
    TopExp_Explorer exp(shape, TopAbs_SOLID);
    return exp.More();
}

// Fuse that never trusts IsDone(): force Shape(), require a SOLID, report
// failure through the return so the caller can fall back (LESSONS
// 2026-07-09 / 2026-07-10).
TopoDS_Shape tryFuse(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    try {
        BRepAlgoAPI_Fuse fuse(a, b);
        const TopoDS_Shape out = fuse.Shape();
        if (hasSolid(out))
            return out;
    } catch (...) {
    }
    return TopoDS_Shape();
}

} // namespace

double AvatarSpec::radiusFracFor(const QString& jointName) const
{
    const auto it = segmentRadiusFrac.constFind(jointName);
    if (it != segmentRadiusFrac.constEnd())
        return it.value();
    return segmentRadiusFrac.value(QStringLiteral("default"), 0.026);
}

AvatarResult avatarFromJson(const QJsonObject& obj)
{
    if (obj.value(QLatin1String("schema_version")).toString()
        != QLatin1String("1"))
        return fail(QStringLiteral("avatar: unsupported schema_version "
                                   "(expected \"1\")"));

    AvatarResult res;
    AvatarSpec& spec = res.spec;
    spec.id = obj.value(QLatin1String("id")).toString();
    if (spec.id.isEmpty())
        return fail(QStringLiteral("avatar: missing \"id\""));
    spec.name = obj.value(QLatin1String("name")).toString();
    spec.chainId = obj.value(QLatin1String("chain")).toString();
    if (spec.chainId.isEmpty())
        return fail(QStringLiteral("avatar: missing \"chain\""));

    const QString type = obj.value(QLatin1String("type")).toString();
    if (type == QLatin1String("rigid"))
        spec.type = AvatarType::Rigid;
    else if (type == QLatin1String("skinned"))
        spec.type = AvatarType::Skinned;
    else
        return fail(QStringLiteral("avatar: type must be \"rigid\" or "
                                   "\"skinned\""));

    if (obj.contains(QLatin1String("height_m"))) {
        const double h = obj.value(QLatin1String("height_m")).toDouble();
        if (!(h > 0))
            return fail(QStringLiteral("avatar: height_m must be > 0"));
        spec.heightM = h;
    }

    if (spec.type == AvatarType::Rigid) {
        const QJsonValue rigidVal = obj.value(QLatin1String("rigid"));
        if (!rigidVal.isObject())
            return fail(QStringLiteral("avatar: type rigid requires a "
                                       "\"rigid\" block"));
        const QJsonObject rigid = rigidVal.toObject();

        const QJsonValue radiiVal =
            rigid.value(QLatin1String("segment_radius"));
        if (!radiiVal.isObject()
            || !radiiVal.toObject().contains(QLatin1String("default")))
            return fail(QStringLiteral("avatar: rigid.segment_radius needs "
                                       "at least a \"default\" entry"));
        const QJsonObject radii = radiiVal.toObject();
        for (auto it = radii.begin(); it != radii.end(); ++it) {
            const double r = it.value().toDouble();
            if (!(r > 0))
                return fail(QStringLiteral("avatar: segment_radius \"%1\" "
                                           "must be > 0")
                                .arg(it.key()));
            spec.segmentRadiusFrac.insert(it.key(), r);
        }

        const QJsonValue headVal = rigid.value(QLatin1String("head"));
        if (headVal.isObject()) {
            const QJsonObject head = headVal.toObject();
            spec.headRadiusFrac =
                head.value(QLatin1String("radius")).toDouble(0.0);
            spec.headElongation =
                head.value(QLatin1String("elongation")).toDouble(1.0);
            if (spec.headRadiusFrac > 0 && spec.headElongation > 0)
                spec.hasHead = true;
            else if (head.contains(QLatin1String("radius")))
                return fail(QStringLiteral("avatar: head.radius and "
                                           "head.elongation must be > 0"));
        }

        const QString style =
            rigid.value(QLatin1String("joint_style"))
                .toString(QStringLiteral("blend"));
        if (style == QLatin1String("blend"))
            spec.jointStyle = JointStyle::Blend;
        else if (style == QLatin1String("sphere"))
            spec.jointStyle = JointStyle::Sphere;
        else if (style == QLatin1String("none"))
            spec.jointStyle = JointStyle::None;
        else
            return fail(QStringLiteral("avatar: joint_style must be blend, "
                                       "sphere or none"));
    } else {
        const QJsonObject skinned =
            obj.value(QLatin1String("skinned")).toObject();
        spec.meshFile =
            skinned.value(QLatin1String("mesh_file")).toString();
    }

    const QJsonValue matVal = obj.value(QLatin1String("material"));
    if (matVal.isObject()) {
        const QJsonObject mat = matVal.toObject();
        spec.baseColor = parseColor(mat, QStringLiteral("base_color"),
                                    spec.baseColor, res.warnings);
        spec.accentColor = parseColor(mat, QStringLiteral("accent_color"),
                                      spec.accentColor, res.warnings);
        spec.roughness = std::clamp(
            mat.value(QLatin1String("roughness")).toDouble(spec.roughness),
            0.0, 1.0);
        spec.metallic = std::clamp(
            mat.value(QLatin1String("metallic")).toDouble(spec.metallic),
            0.0, 1.0);
    }

    res.ok = true;
    return res;
}

AvatarResult loadAvatarFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("avatar: cannot read %1").arg(path));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull())
        return fail(QStringLiteral("avatar: %1: invalid JSON (%2)")
                        .arg(path, perr.errorString()));
    if (!doc.isObject())
        return fail(QStringLiteral("avatar: %1: top level is not an object")
                        .arg(path));
    return avatarFromJson(doc.object());
}

RigidAvatarProvider::RigidAvatarProvider(AvatarSpec spec)
    : m_spec(std::move(spec))
{
}

std::vector<AvatarPart> RigidAvatarProvider::partsForJoint(
    const Chain& chain, int index) const
{
    silenceOcctMessages();
    std::vector<AvatarPart> parts;
    const Joint& joint = chain.joints[static_cast<size_t>(index)];
    const double r = m_spec.radiusFracFor(joint.name) * chain.scaleMm;
    const double len = joint.lengthMm;

    if (len > 1e-6 && r > 1e-6) {
        const gp_Dir dir(joint.restDirection);
        TopoDS_Shape segment =
            BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), dir), r, len)
                .Shape();
        if (m_spec.jointStyle != JointStyle::None && !segment.IsNull()) {
            // Capsule: round both ends. A failed fuse falls back to the
            // separate pieces — same silhouette, never a missing limb.
            const TopoDS_Shape cap0 =
                BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), r).Shape();
            gp_Pnt endPnt(joint.restDirection.XYZ() * len);
            const TopoDS_Shape cap1 =
                BRepPrimAPI_MakeSphere(endPnt, r).Shape();
            TopoDS_Shape fused = tryFuse(segment, cap0);
            if (!fused.IsNull())
                fused = tryFuse(fused, cap1);
            if (!fused.IsNull()) {
                segment = fused;
            } else {
                AvatarPart p0;
                p0.shape = cap0;
                p0.name = joint.name + QStringLiteral("_cap0");
                parts.push_back(p0);
                AvatarPart p1;
                p1.shape = cap1;
                p1.name = joint.name + QStringLiteral("_cap1");
                parts.push_back(p1);
            }
        }
        if (!segment.IsNull()) {
            AvatarPart part;
            part.shape = segment;
            part.name = joint.name;
            parts.push_back(part);
        }
        if (m_spec.jointStyle == JointStyle::Sphere) {
            AvatarPart ball;
            ball.shape =
                BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), r * 1.15).Shape();
            ball.name = joint.name + QStringLiteral("_joint");
            ball.accent = true;
            if (!ball.shape.IsNull())
                parts.push_back(ball);
        }
    }

    if (m_spec.hasHead && joint.name == QLatin1String("neck")) {
        // Ellipsoid head at the far end of the neck: a sphere stretched
        // along the segment direction by `elongation`, overlapping the neck
        // tip slightly so the two read as one body.
        const double hr = m_spec.headRadiusFrac * chain.scaleMm;
        TopoDS_Shape head = BRepPrimAPI_MakeSphere(hr).Shape();
        if (!head.IsNull()) {
            gp_GTrsf stretch;
            stretch.SetValue(3, 3, m_spec.headElongation);
            head = BRepBuilderAPI_GTransform(head, stretch, true).Shape();
        }
        if (!head.IsNull()) {
            gp_Quaternion zToDir;
            zToDir.SetRotation(gp_Vec(0, 0, 1), joint.restDirection);
            gp_Trsf rot;
            rot.SetRotation(zToDir);
            gp_Trsf move;
            const double centerAlong =
                len + hr * m_spec.headElongation * 0.85;
            move.SetTranslation(joint.restDirection.XYZ() * centerAlong);
            head = BRepBuilderAPI_Transform(head, move * rot, true).Shape();
        }
        if (!head.IsNull()) {
            AvatarPart part;
            part.shape = head;
            part.name = QStringLiteral("head");
            part.accent = true;
            parts.push_back(part);
        }
    }

    return parts;
}

ProviderResult makeAvatarProvider(const AvatarSpec& spec)
{
    ProviderResult res;
    if (spec.type == AvatarType::Skinned) {
        res.error = QStringLiteral(
            "avatar \"%1\": skinned avatars are a later worksite — use a "
            "rigid avatar for now")
                        .arg(spec.id);
        return res;
    }
    res.provider = std::make_unique<RigidAvatarProvider>(spec);
    res.ok = true;
    return res;
}

} // namespace anim
} // namespace viki
