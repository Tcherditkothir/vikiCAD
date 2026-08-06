#include "anim/Avatar.h"

#include <QFile>
#include <QJsonDocument>

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Elips.hxx>
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

// Rotation taking the canonical +Z axis onto `dir`. For antiparallel
// directions OCCT picks an orthogonal axis; the choice is deterministic and
// only the torso loft is not axisymmetric (its joint conventionally points
// +Z anyway).
gp_Trsf zToDirection(const gp_Dir& dir)
{
    gp_Quaternion q;
    q.SetRotation(gp_Vec(0, 0, 1), gp_Vec(dir));
    gp_Trsf t;
    t.SetRotation(q);
    return t;
}

// Fusiform limb: ONE solid of revolution around the segment axis — rounded
// proximal cap at the joint pivot, slight mid swell, taper to the distal
// radius, rounded distal cap at the child pivot. No booleans: robust, fast
// to tessellate, G1-smooth by construction.
TopoDS_Shape makeFusiform(double rProx, double rDist, double lenMm,
                          double bulge, const gp_Dir& dir)
{
    const double rMid = bulge * (0.60 * rProx + 0.40 * rDist);
    const std::vector<std::pair<double, double>> guide = {
        {-0.88 * rProx, 0.0},
        {-0.58 * rProx, 0.76 * rProx},
        {0.12 * lenMm, rProx},
        {0.40 * lenMm, rMid},
        {0.80 * lenMm, 1.02 * rDist},
        {lenMm + 0.58 * rDist, 0.76 * rDist},
        {lenMm + 0.88 * rDist, 0.0},
    };
    try {
        TColgp_Array1OfPnt pts(1, static_cast<Standard_Integer>(guide.size()));
        for (size_t i = 0; i < guide.size(); ++i)
            pts.SetValue(static_cast<Standard_Integer>(i + 1),
                         gp_Pnt(guide[i].second, 0.0, guide[i].first));
        Handle(Geom_BSplineCurve) curve =
            GeomAPI_PointsToBSpline(pts, 3, 8, GeomAbs_C2, 0.05).Curve();
        if (curve.IsNull())
            return TopoDS_Shape();
        const TopoDS_Edge profile = BRepBuilderAPI_MakeEdge(curve).Edge();
        const TopoDS_Edge closing = BRepBuilderAPI_MakeEdge(
            gp_Pnt(0, 0, guide.back().first),
            gp_Pnt(0, 0, guide.front().first)).Edge();
        BRepBuilderAPI_MakeWire wire(profile, closing);
        const TopoDS_Face face =
            BRepBuilderAPI_MakeFace(wire.Wire(), true).Face();
        TopoDS_Shape solid =
            BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(0, 0, 0),
                                               gp_Dir(0, 0, 1))).Shape();
        if (solid.IsNull() || !hasSolid(solid))
            return TopoDS_Shape();
        return BRepBuilderAPI_Transform(solid, zToDirection(dir), true)
            .Shape();
    } catch (...) {
    }
    return TopoDS_Shape();
}

// Torso: smooth loft through elliptical sections (half-width along local X,
// half-depth = half-width * depth along local Y) from the hip line up past
// the neck base, so the shoulders round off and the arm attach points sink
// into the trunk. Flat bottom face — the pelvis blob overlaps it.
TopoDS_Shape makeTorsoLoft(const TorsoSculpt& torso, double scaleMm,
                           double lenMm, const gp_Dir& dir)
{
    struct Section {
        double z;
        double halfWidth;
    };
    // Two stages, like the limbs: first a SMOOTHED meridian profile
    // (approximated B-spline — a smooth loft straight through near-equal
    // sections oscillates: wasp waist + flared skirt out of thin air),
    // then a dense RULED loft of ellipses sampled on that profile.
    const std::vector<Section> guide = {
        {0.0, torso.hip},
        {0.20 * lenMm / scaleMm, 0.5 * (torso.hip + torso.waist)},
        {0.40 * lenMm / scaleMm, torso.waist},
        {0.64 * lenMm / scaleMm, 0.5 * (torso.waist + torso.chest)},
        {0.82 * lenMm / scaleMm, torso.chest},
        {0.92 * lenMm / scaleMm, torso.shoulder},
        // Domed top: curve well past the shoulder line down to a near-point
        // — a wide flat top ellipse reads as a lampshade crease.
        {0.985 * lenMm / scaleMm, 0.74 * torso.shoulder},
        {lenMm / scaleMm + 0.018, 0.38 * torso.shoulder},
        {lenMm / scaleMm + 0.030, 0.10 * torso.shoulder},
    };
    try {
        TColgp_Array1OfPnt pts(1, static_cast<Standard_Integer>(guide.size()));
        for (size_t i = 0; i < guide.size(); ++i)
            pts.SetValue(static_cast<Standard_Integer>(i + 1),
                         gp_Pnt(guide[i].halfWidth * scaleMm, 0.0,
                                guide[i].z * scaleMm));
        // Loose tolerance on purpose: approximation smooths the profile
        // instead of chasing every guide point. Degree capped at 3 —
        // higher degrees fit globally and Runge-oscillate at both ends
        // (the trunk grew a 68 mm spout under the hem).
        Handle(Geom_BSplineCurve) profile =
            GeomAPI_PointsToBSpline(pts, 3, 3, GeomAbs_C2, 2.0).Curve();
        if (profile.IsNull())
            return TopoDS_Shape();

        // RULED loft, densely sampled. Tried and rejected: smooth loft
        // straight through the guide (oscillates — wasp waist + skirt),
        // smooth loft through the samples (the surface approximation
        // explodes into a tent near the almost-degenerate top section).
        // Ruled through 48 samples of the smoothed profile is robust and
        // the facet normals differ by too little for Phong to show bands.
        BRepOffsetAPI_ThruSections loft(true /*solid*/, true /*ruled*/);
        const int kSamples = 48;
        const double t0 = profile->FirstParameter();
        const double t1 = profile->LastParameter();
        for (int i = 0; i <= kSamples; ++i) {
            const gp_Pnt p = profile->Value(
                t0 + (t1 - t0) * static_cast<double>(i) / kSamples);
            const double w = std::max(p.X(), 1.0); // never a degenerate wire
            const gp_Elips ellipse(
                gp_Ax2(gp_Pnt(0, 0, p.Z()), gp_Dir(0, 0, 1),
                       gp_Dir(1, 0, 0)),
                w, w * torso.depth);
            const TopoDS_Edge e = BRepBuilderAPI_MakeEdge(ellipse).Edge();
            loft.AddWire(BRepBuilderAPI_MakeWire(e).Wire());
        }
        TopoDS_Shape solid = loft.Shape();
        if (solid.IsNull() || !hasSolid(solid))
            return TopoDS_Shape();
        return BRepBuilderAPI_Transform(solid, zToDirection(dir), true)
            .Shape();
    } catch (...) {
    }
    return TopoDS_Shape();
}

// Pelvis: ellipsoid blob on the (usually zero-length) root joint. Its top
// overlaps the torso bottom, its underside rounds off below the hip line.
TopoDS_Shape makePelvisBlob(const PelvisSculpt& pelvis, double scaleMm)
{
    // The top rises well into the torso hem so the wider pelvis meets the
    // narrower trunk on a CURVE — a shallow top reads as a skirt ledge.
    const double top = 0.035 * scaleMm;
    const double bottom = -pelvis.drop * scaleMm;
    const double halfHeight = 0.5 * (top - bottom);
    const double centerZ = 0.5 * (top + bottom);
    try {
        TopoDS_Shape blob = BRepPrimAPI_MakeSphere(1.0).Shape();
        if (blob.IsNull())
            return TopoDS_Shape();
        gp_GTrsf stretch;
        stretch.SetValue(1, 1, pelvis.width * scaleMm);
        stretch.SetValue(2, 2, pelvis.depth * scaleMm);
        stretch.SetValue(3, 3, halfHeight);
        blob = BRepBuilderAPI_GTransform(blob, stretch, true).Shape();
        if (blob.IsNull())
            return TopoDS_Shape();
        gp_Trsf move;
        move.SetTranslation(gp_Vec(0, 0, centerZ));
        return BRepBuilderAPI_Transform(blob, move, true).Shape();
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

double SculptParams::taperFor(const QString& jointName) const
{
    const auto it = taperFrac.constFind(jointName);
    if (it != taperFrac.constEnd())
        return it.value();
    return taperFrac.value(QStringLiteral("default"), 0.62);
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

        // Extension fields (schema v1 tolerates extra properties; proposed
        // for v2 — contract entry SCHEMA-AVATAR-V2-DEMANDE).
        const QString build = rigid.value(QLatin1String("build"))
                                  .toString(QStringLiteral("capsule"));
        if (build == QLatin1String("capsule"))
            spec.build = RigidBuild::Capsule;
        else if (build == QLatin1String("sculpted"))
            spec.build = RigidBuild::Sculpted;
        else
            return fail(QStringLiteral("avatar: rigid.build must be "
                                       "\"capsule\" or \"sculpted\""));

        const QJsonValue sculptVal = rigid.value(QLatin1String("sculpt"));
        if (sculptVal.isObject()) {
            const QJsonObject sculpt = sculptVal.toObject();
            SculptParams& sp = spec.sculpt;
            const QJsonValue taperVal = sculpt.value(QLatin1String("taper"));
            if (taperVal.isDouble()) {
                // Shorthand: a bare number sets the default ratio.
                const double t = taperVal.toDouble();
                if (!(t > 0.0 && t <= 1.5))
                    return fail(QStringLiteral(
                        "avatar: sculpt.taper must be in (0, 1.5]"));
                sp.taperFrac.insert(QStringLiteral("default"), t);
            } else if (taperVal.isObject()) {
                const QJsonObject taper = taperVal.toObject();
                for (auto it = taper.begin(); it != taper.end(); ++it) {
                    const double t = it.value().toDouble();
                    if (!(t > 0.0 && t <= 1.5))
                        return fail(
                            QStringLiteral("avatar: sculpt.taper \"%1\" "
                                           "must be in (0, 1.5]")
                                .arg(it.key()));
                    sp.taperFrac.insert(it.key(), t);
                }
            } else if (!taperVal.isUndefined() && !taperVal.isNull()) {
                return fail(QStringLiteral(
                    "avatar: sculpt.taper must be a number or an object"));
            }
            if (sculpt.contains(QLatin1String("bulge"))) {
                sp.bulge = sculpt.value(QLatin1String("bulge")).toDouble();
                if (!(sp.bulge >= 0.5 && sp.bulge <= 2.0))
                    return fail(QStringLiteral(
                        "avatar: sculpt.bulge must be in [0.5, 2]"));
            }
            const QJsonValue torsoVal = sculpt.value(QLatin1String("torso"));
            if (torsoVal.isObject()) {
                const QJsonObject t = torsoVal.toObject();
                TorsoSculpt torso;
                if (t.contains(QLatin1String("joint")))
                    torso.joint = t.value(QLatin1String("joint")).toString();
                if (torso.joint.isEmpty())
                    return fail(QStringLiteral(
                        "avatar: sculpt.torso.joint must be a joint name"));
                struct Field {
                    const char* key;
                    double* dst;
                };
                const Field fields[] = {
                    {"hip", &torso.hip},         {"waist", &torso.waist},
                    {"chest", &torso.chest},     {"shoulder", &torso.shoulder},
                };
                for (const Field& f : fields) {
                    if (!t.contains(QLatin1String(f.key)))
                        continue;
                    *f.dst = t.value(QLatin1String(f.key)).toDouble();
                    if (!(*f.dst > 0.0))
                        return fail(QStringLiteral("avatar: sculpt.torso.%1 "
                                                   "must be > 0")
                                        .arg(QLatin1String(f.key)));
                }
                if (t.contains(QLatin1String("depth"))) {
                    torso.depth = t.value(QLatin1String("depth")).toDouble();
                    if (!(torso.depth > 0.0 && torso.depth <= 1.0))
                        return fail(QStringLiteral(
                            "avatar: sculpt.torso.depth must be in (0, 1]"));
                }
                sp.torso = torso;
            }
            const QJsonValue pelvisVal =
                sculpt.value(QLatin1String("pelvis"));
            if (pelvisVal.isObject()) {
                const QJsonObject p = pelvisVal.toObject();
                PelvisSculpt pelvis;
                if (p.contains(QLatin1String("joint")))
                    pelvis.joint = p.value(QLatin1String("joint")).toString();
                if (pelvis.joint.isEmpty())
                    return fail(QStringLiteral(
                        "avatar: sculpt.pelvis.joint must be a joint name"));
                struct Field {
                    const char* key;
                    double* dst;
                };
                const Field fields[] = {
                    {"width", &pelvis.width},
                    {"depth", &pelvis.depth},
                    {"drop", &pelvis.drop},
                };
                for (const Field& f : fields) {
                    if (!p.contains(QLatin1String(f.key)))
                        continue;
                    *f.dst = p.value(QLatin1String(f.key)).toDouble();
                    if (!(*f.dst > 0.0))
                        return fail(QStringLiteral("avatar: sculpt.pelvis.%1 "
                                                   "must be > 0")
                                        .arg(QLatin1String(f.key)));
                }
                sp.pelvis = pelvis;
            }
            const QJsonValue flattenVal =
                sculpt.value(QLatin1String("flatten"));
            if (flattenVal.isObject()) {
                const QJsonObject flatten = flattenVal.toObject();
                for (auto it = flatten.begin(); it != flatten.end(); ++it) {
                    const double f = it.value().toDouble();
                    if (!(f > 0.0 && f <= 1.0))
                        return fail(QStringLiteral(
                                        "avatar: sculpt.flatten \"%1\" must "
                                        "be in (0, 1]")
                                        .arg(it.key()));
                    sp.flatten.insert(it.key(), f);
                }
            }
        } else if (!sculptVal.isUndefined() && !sculptVal.isNull()) {
            return fail(
                QStringLiteral("avatar: rigid.sculpt must be an object"));
        }
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

    // presentation.ground_shadow (extension field, proposed for schema v2):
    // opacity of the contact-shadow blob under the animation footprint.
    const QJsonValue presVal = obj.value(QLatin1String("presentation"));
    if (presVal.isObject()) {
        const QJsonObject pres = presVal.toObject();
        if (pres.contains(QLatin1String("ground_shadow"))) {
            const double g =
                pres.value(QLatin1String("ground_shadow")).toDouble(-1.0);
            if (g < 0.0 || g > 1.0)
                return fail(QStringLiteral(
                    "avatar: presentation.ground_shadow must be in [0, 1]"));
            spec.groundShadow = g;
        }
    } else if (!presVal.isUndefined() && !presVal.isNull()) {
        return fail(
            QStringLiteral("avatar: presentation must be an object"));
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
    if (m_spec.build == RigidBuild::Sculpted)
        return sculptedParts(chain, index);
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

    appendHead(parts, joint, len, chain.scaleMm, 0.85);

    return parts;
}

// Ellipsoid head at the far end of the joint named "neck": a sphere
// stretched along the segment direction by `elongation`, overlapping the
// neck tip so the two read as one body. `overlap` is the fraction of the
// stretched radius the centre sits past the neck tip (smaller = deeper
// overlap, used by the sculpted build to seat the head on the neck).
void RigidAvatarProvider::appendHead(std::vector<AvatarPart>& parts,
                                     const Joint& joint, double lenMm,
                                     double scaleMm, double overlap) const
{
    if (!m_spec.hasHead || joint.name != QLatin1String("neck"))
        return;
    const double hr = m_spec.headRadiusFrac * scaleMm;
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
            lenMm + hr * m_spec.headElongation * overlap;
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

// Largest proximal radius among the joint's continuing children — the limb
// tapers TO the next segment so bent joints stay continuous (the parent's
// distal cap and the child's proximal cap share the pivot and the radius).
// Leaf segments taper by sculpt.taper instead.
double RigidAvatarProvider::distalRadiusMm(const Chain& chain, int index,
                                           double rProx) const
{
    double best = 0.0;
    for (const Joint& j : chain.joints) {
        if (j.parent != index || j.lengthMm <= 1e-6)
            continue;
        best = std::max(best, m_spec.radiusFracFor(j.name) * chain.scaleMm);
    }
    if (best > 1e-6)
        return best;
    return rProx
        * m_spec.sculpt.taperFor(
            chain.joints[static_cast<size_t>(index)].name);
}

std::vector<AvatarPart> RigidAvatarProvider::sculptedParts(
    const Chain& chain, int index) const
{
    std::vector<AvatarPart> parts;
    const Joint& joint = chain.joints[static_cast<size_t>(index)];
    const SculptParams& sculpt = m_spec.sculpt;
    const double rProx = m_spec.radiusFracFor(joint.name) * chain.scaleMm;
    const double len = joint.lengthMm;

    const bool isTorso = sculpt.torso && joint.name == sculpt.torso->joint;
    const bool isPelvis =
        sculpt.pelvis && joint.name == sculpt.pelvis->joint;

    if (len > 1e-6 && rProx > 1e-6) {
        const gp_Dir dir(joint.restDirection);
        TopoDS_Shape solid;
        if (isTorso) {
            solid = makeTorsoLoft(*sculpt.torso, chain.scaleMm, len, dir);
            if (solid.IsNull())
                qWarning("anim: torso loft failed for joint \"%s\", "
                         "falling back to a fusiform trunk",
                         qUtf8Printable(joint.name));
        }
        if (solid.IsNull()) {
            // Limb — or the torso loft's fallback: same silhouette family,
            // never a missing trunk (LESSONS rule on degraded fallbacks).
            const double rDist = distalRadiusMm(chain, index, rProx);
            solid = makeFusiform(rProx, rDist, len, sculpt.bulge, dir);
        }
        if (isTorso && !solid.IsNull()) {
            // Deltoid caps: one sphere per off-axis limb attached to the
            // torso, carried BY the torso (a shoulder does not rotate with
            // the arm). Generic: any limb hanging off a trunk gets one.
            for (const Joint& child : chain.joints) {
                if (child.parent != index || child.lengthMm <= 1e-6)
                    continue;
                if (std::abs(child.attachMm.X()) < 1e-6
                    && std::abs(child.attachMm.Y()) < 1e-6)
                    continue; // on-axis (e.g. the neck): the loft covers it
                const double rc =
                    m_spec.radiusFracFor(child.name) * chain.scaleMm;
                AvatarPart cap;
                cap.shape =
                    BRepPrimAPI_MakeSphere(gp_Pnt(child.attachMm.X(),
                                                  child.attachMm.Y(),
                                                  child.attachMm.Z()),
                                           rc * 1.35)
                        .Shape();
                cap.name = child.name + QStringLiteral("_deltoid");
                if (!cap.shape.IsNull())
                    parts.push_back(cap);
            }
        }
        if (!solid.IsNull()) {
            const auto flatIt = sculpt.flatten.constFind(joint.name);
            if (flatIt != sculpt.flatten.constEnd()
                && flatIt.value() < 1.0) {
                // Squash in the joint's LOCAL frame, after the rotation to
                // rest_direction: local Z, or local Y when the segment
                // itself runs along ±Z (feet vs hands rule in the header).
                const bool alongZ = std::abs(joint.restDirection.Z()) > 0.9;
                gp_GTrsf squash;
                squash.SetValue(alongZ ? 2 : 3, alongZ ? 2 : 3,
                                flatIt.value());
                const TopoDS_Shape squashed =
                    BRepBuilderAPI_GTransform(solid, squash, true).Shape();
                if (!squashed.IsNull())
                    solid = squashed;
            }
            AvatarPart part;
            part.shape = solid;
            part.name = joint.name;
            parts.push_back(part);
        }

        // Torso pivot sphere: bridges the hip fold. The hem lifts off the
        // pelvis when the trunk flexes ~90° (downward dog); only a SPHERE
        // is rotation-invariant at a pivot, sized to the hem half-depth so
        // it fills the sagittal gap without ballooning the waist.
        if (isTorso && m_spec.jointStyle != JointStyle::None
            && joint.parent >= 0) {
            AvatarPart ball;
            ball.shape =
                BRepPrimAPI_MakeSphere(
                    gp_Pnt(0, 0, 0),
                    sculpt.torso->hip * sculpt.torso->depth * chain.scaleMm)
                    .Shape();
            ball.name = joint.name + QStringLiteral("_joint");
            if (!ball.shape.IsNull())
                parts.push_back(ball);
        }

        // Knuckle sphere at the pivot: covers the articulation at ANY
        // flexion angle (the fusiform caps alone can crease past ~120°).
        // Sphere style shows it in the accent colour, blend hides it in
        // the base colour, none drops it.
        if (!isTorso && m_spec.jointStyle != JointStyle::None
            && joint.parent >= 0) {
            const Joint& parent =
                chain.joints[static_cast<size_t>(joint.parent)];
            const bool parentDraws = parent.lengthMm > 1e-6
                || (sculpt.pelvis && parent.name == sculpt.pelvis->joint);
            if (parentDraws) {
                const bool accent = m_spec.jointStyle == JointStyle::Sphere;
                AvatarPart ball;
                ball.shape = BRepPrimAPI_MakeSphere(
                                 gp_Pnt(0, 0, 0),
                                 rProx * (accent ? 1.12 : 0.99))
                                 .Shape();
                ball.name = joint.name + QStringLiteral("_joint");
                ball.accent = accent;
                if (!ball.shape.IsNull())
                    parts.push_back(ball);
            }
        }
    }

    if (isPelvis) {
        AvatarPart blob;
        blob.shape = makePelvisBlob(*sculpt.pelvis, chain.scaleMm);
        blob.name = joint.name + QStringLiteral("_pelvis");
        if (!blob.shape.IsNull())
            parts.push_back(blob);
    }

    appendHead(parts, joint, len, chain.scaleMm, 0.70);

    return parts;
}

// The avatar file parses without its chain, so sculpt fields that NAME
// joints can only be cross-checked where the pair meets (CLI, GUI load).
// Warnings, not errors: a generic chain simply has no "spine".
QStringList avatarChainWarnings(const AvatarSpec& spec, const Chain& chain)
{
    QStringList warnings;
    if (spec.type != AvatarType::Rigid
        || spec.build != RigidBuild::Sculpted)
        return warnings;
    const SculptParams& sculpt = spec.sculpt;
    if (sculpt.torso && chain.indexOf(sculpt.torso->joint) < 0)
        warnings.append(QStringLiteral("avatar: sculpt.torso.joint \"%1\" "
                                       "is not a joint of chain \"%2\"")
                            .arg(sculpt.torso->joint, chain.id));
    if (sculpt.pelvis && chain.indexOf(sculpt.pelvis->joint) < 0)
        warnings.append(QStringLiteral("avatar: sculpt.pelvis.joint \"%1\" "
                                       "is not a joint of chain \"%2\"")
                            .arg(sculpt.pelvis->joint, chain.id));
    for (auto it = sculpt.flatten.constBegin();
         it != sculpt.flatten.constEnd(); ++it) {
        if (chain.indexOf(it.key()) < 0)
            warnings.append(
                QStringLiteral("avatar: sculpt.flatten names \"%1\", not a "
                               "joint of chain \"%2\"")
                    .arg(it.key(), chain.id));
    }
    return warnings;
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
