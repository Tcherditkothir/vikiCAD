#include "anim/GlbExporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include "io/OcctMessages.h"

namespace viki {
namespace anim {

namespace {

struct MeshData {
    std::vector<float> positions; // xyz triplets, joint-local mm
    std::vector<float> normals;
    std::vector<quint32> indices;
    float posMin[3] = {0, 0, 0};
    float posMax[3] = {0, 0, 0};
    QString name;
    bool accent = false;
};

// ObjIo's face-walk, feeding arrays instead of an OBJ stream. Normals are
// guaranteed: BRepMesh does not always leave per-node normals behind, and a
// shaded GLB without normals renders faceted or black in strict viewers.
bool tessellate(const TopoDS_Shape& shape, double deflection, MeshData& out)
{
    BRepMesh_IncrementalMesh mesher(shape, deflection, Standard_False, 0.5,
                                    Standard_True);
    mesher.Perform();

    bool first = true;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Face face = TopoDS::Face(exp.Current());
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri =
            BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull() || tri->NbTriangles() == 0)
            continue;
        if (!tri->HasNormals())
            BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);

        const gp_Trsf trsf = loc.Transformation();
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        const quint32 base =
            static_cast<quint32>(out.positions.size() / 3);

        const int nbNodes = tri->NbNodes();
        for (int i = 1; i <= nbNodes; ++i) {
            gp_Pnt p = tri->Node(i);
            p.Transform(trsf);
            const float x = static_cast<float>(p.X());
            const float y = static_cast<float>(p.Y());
            const float z = static_cast<float>(p.Z());
            out.positions.push_back(x);
            out.positions.push_back(y);
            out.positions.push_back(z);
            if (first) {
                out.posMin[0] = out.posMax[0] = x;
                out.posMin[1] = out.posMax[1] = y;
                out.posMin[2] = out.posMax[2] = z;
                first = false;
            } else {
                out.posMin[0] = std::min(out.posMin[0], x);
                out.posMin[1] = std::min(out.posMin[1], y);
                out.posMin[2] = std::min(out.posMin[2], z);
                out.posMax[0] = std::max(out.posMax[0], x);
                out.posMax[1] = std::max(out.posMax[1], y);
                out.posMax[2] = std::max(out.posMax[2], z);
            }
            const gp_Dir n = tri->Normal(i);
            gp_Vec nv(n.X(), n.Y(), n.Z());
            nv.Transform(trsf);
            if (reversed)
                nv.Reverse();
            out.normals.push_back(static_cast<float>(nv.X()));
            out.normals.push_back(static_cast<float>(nv.Y()));
            out.normals.push_back(static_cast<float>(nv.Z()));
        }

        for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int a = 0, b = 0, c = 0;
            tri->Triangle(t).Get(a, b, c);
            if (reversed)
                std::swap(b, c);
            out.indices.push_back(base + static_cast<quint32>(a - 1));
            out.indices.push_back(base + static_cast<quint32>(b - 1));
            out.indices.push_back(base + static_cast<quint32>(c - 1));
        }
    }
    return !out.indices.empty();
}

// Single embedded GLB buffer: every view is 4-byte aligned as the spec
// demands for accessor component types.
class BinBuilder {
public:
    QByteArray bytes;
    QJsonArray views;

    int addView(const void* data, qsizetype size,
                std::optional<int> target)
    {
        while (bytes.size() % 4 != 0)
            bytes.append('\0');
        QJsonObject view;
        view.insert(QLatin1String("buffer"), 0);
        view.insert(QLatin1String("byteOffset"),
                    static_cast<double>(bytes.size()));
        view.insert(QLatin1String("byteLength"),
                    static_cast<double>(size));
        if (target)
            view.insert(QLatin1String("target"), *target);
        bytes.append(static_cast<const char*>(data), size);
        views.append(view);
        return views.size() - 1;
    }
};

QJsonArray vec3Json(double x, double y, double z)
{
    QJsonArray a;
    a.append(x);
    a.append(y);
    a.append(z);
    return a;
}

double srgbToLinear(int c255)
{
    const double c = c255 / 255.0;
    return (c <= 0.04045) ? c / 12.92
                          : std::pow((c + 0.055) / 1.055, 2.4);
}

QJsonObject materialJson(const QString& name, quint32 rgb,
                         double roughness, double metallic)
{
    QJsonObject pbr;
    QJsonArray base;
    base.append(srgbToLinear(static_cast<int>((rgb >> 16) & 0xFF)));
    base.append(srgbToLinear(static_cast<int>((rgb >> 8) & 0xFF)));
    base.append(srgbToLinear(static_cast<int>(rgb & 0xFF)));
    base.append(1.0);
    pbr.insert(QLatin1String("baseColorFactor"), base);
    pbr.insert(QLatin1String("roughnessFactor"), roughness);
    pbr.insert(QLatin1String("metallicFactor"), metallic);
    QJsonObject mat;
    mat.insert(QLatin1String("name"), name);
    mat.insert(QLatin1String("pbrMetallicRoughness"), pbr);
    return mat;
}

bool quatIsRest(const gp_Quaternion& q)
{
    return std::abs(q.X()) < 1e-12 && std::abs(q.Y()) < 1e-12
           && std::abs(q.Z()) < 1e-12
           && std::abs(std::abs(q.W()) - 1.0) < 1e-12;
}

GlbResult failGlb(const QString& message)
{
    GlbResult r;
    r.error = message;
    return r;
}

} // namespace

GlbResult exportGlb(const Chain& chain, const AnimClip& clip,
                    const AvatarProvider& provider,
                    const AvatarSpec& avatar, const QString& path,
                    double deflectionMm)
{
    silenceOcctMessages();
    if (chain.joints.empty() || clip.keys.empty())
        return failGlb(QStringLiteral("glb: nothing to export"));
    if (deflectionMm <= 0)
        deflectionMm = 0.8;

    GlbResult result;
    const int jointCount = static_cast<int>(chain.joints.size());

    // ---- Meshes (avatar parts, joint-local) -----------------------------
    std::vector<MeshData> meshes;
    std::vector<std::vector<int>> jointMeshes(
        static_cast<size_t>(jointCount));
    for (int j = 0; j < jointCount; ++j) {
        const auto parts = provider.partsForJoint(chain, j);
        for (const AvatarPart& part : parts) {
            if (part.shape.IsNull())
                continue;
            MeshData mesh;
            mesh.name = part.name;
            mesh.accent = part.accent;
            if (!tessellate(part.shape, deflectionMm, mesh))
                continue;
            jointMeshes[static_cast<size_t>(j)].push_back(
                static_cast<int>(meshes.size()));
            result.triangles +=
                static_cast<int>(mesh.indices.size() / 3);
            meshes.push_back(std::move(mesh));
        }
    }
    if (meshes.empty())
        return failGlb(QStringLiteral("glb: the avatar produced no "
                                      "geometry"));
    result.meshes = static_cast<int>(meshes.size());

    BinBuilder bin;
    QJsonArray accessors;
    QJsonArray meshesJson;

    const int kArrayBuffer = 34962;
    const int kElementArrayBuffer = 34963;
    const int kFloat = 5126;
    const int kUInt = 5125;

    for (const MeshData& mesh : meshes) {
        const int idxView =
            bin.addView(mesh.indices.data(),
                        static_cast<qsizetype>(mesh.indices.size()
                                               * sizeof(quint32)),
                        kElementArrayBuffer);
        QJsonObject idxAcc;
        idxAcc.insert(QLatin1String("bufferView"), idxView);
        idxAcc.insert(QLatin1String("componentType"), kUInt);
        idxAcc.insert(QLatin1String("count"),
                      static_cast<double>(mesh.indices.size()));
        idxAcc.insert(QLatin1String("type"), QStringLiteral("SCALAR"));
        accessors.append(idxAcc);
        const int idxAccIndex = accessors.size() - 1;

        const int posView =
            bin.addView(mesh.positions.data(),
                        static_cast<qsizetype>(mesh.positions.size()
                                               * sizeof(float)),
                        kArrayBuffer);
        QJsonObject posAcc;
        posAcc.insert(QLatin1String("bufferView"), posView);
        posAcc.insert(QLatin1String("componentType"), kFloat);
        posAcc.insert(QLatin1String("count"),
                      static_cast<double>(mesh.positions.size() / 3));
        posAcc.insert(QLatin1String("type"), QStringLiteral("VEC3"));
        posAcc.insert(QLatin1String("min"),
                      vec3Json(mesh.posMin[0], mesh.posMin[1],
                               mesh.posMin[2]));
        posAcc.insert(QLatin1String("max"),
                      vec3Json(mesh.posMax[0], mesh.posMax[1],
                               mesh.posMax[2]));
        accessors.append(posAcc);
        const int posAccIndex = accessors.size() - 1;

        const int nrmView =
            bin.addView(mesh.normals.data(),
                        static_cast<qsizetype>(mesh.normals.size()
                                               * sizeof(float)),
                        kArrayBuffer);
        QJsonObject nrmAcc;
        nrmAcc.insert(QLatin1String("bufferView"), nrmView);
        nrmAcc.insert(QLatin1String("componentType"), kFloat);
        nrmAcc.insert(QLatin1String("count"),
                      static_cast<double>(mesh.normals.size() / 3));
        nrmAcc.insert(QLatin1String("type"), QStringLiteral("VEC3"));
        accessors.append(nrmAcc);
        const int nrmAccIndex = accessors.size() - 1;

        QJsonObject attributes;
        attributes.insert(QLatin1String("POSITION"), posAccIndex);
        attributes.insert(QLatin1String("NORMAL"), nrmAccIndex);
        QJsonObject primitive;
        primitive.insert(QLatin1String("attributes"), attributes);
        primitive.insert(QLatin1String("indices"), idxAccIndex);
        primitive.insert(QLatin1String("material"), mesh.accent ? 1 : 0);
        QJsonArray primitives;
        primitives.append(primitive);
        QJsonObject meshJson;
        meshJson.insert(QLatin1String("name"), mesh.name);
        meshJson.insert(QLatin1String("primitives"), primitives);
        meshesJson.append(meshJson);
    }

    // ---- Nodes ----------------------------------------------------------
    // 0 = axis/unit fix-up, 1..jointCount = joints, then the mesh nodes.
    QJsonArray nodes;
    {
        QJsonObject fix;
        fix.insert(QLatin1String("name"), QStringLiteral("zup_mm_to_gltf"));
        // Ry(180) * Rx(-90) as (x, y, z, w): our up +Z lands on glTF +Y
        // AND our front +Y lands on glTF +Z (the spec's forward-facing
        // convention; +X becomes -X = the spec's "right"). A bare Rx(-90)
        // keeps up correct but shows the avatar's BACK to a spec-abiding
        // viewer.
        QJsonArray rot;
        rot.append(0.0);
        rot.append(std::sqrt(0.5));
        rot.append(std::sqrt(0.5));
        rot.append(0.0);
        fix.insert(QLatin1String("rotation"), rot);
        fix.insert(QLatin1String("scale"),
                   vec3Json(0.001, 0.001, 0.001));
        QJsonArray children;
        children.append(1); // the chain root joint
        fix.insert(QLatin1String("children"), children);
        nodes.append(fix);
    }
    std::vector<QJsonArray> childrenOf(static_cast<size_t>(jointCount));
    for (int j = 1; j < jointCount; ++j)
        childrenOf[static_cast<size_t>(chain.joints[j].parent)].append(
            1 + j);
    int nextNode = 1 + jointCount;
    std::vector<std::pair<QString, int>> meshNodes; // name, mesh index
    for (int j = 0; j < jointCount; ++j) {
        for (const int m : jointMeshes[static_cast<size_t>(j)]) {
            childrenOf[static_cast<size_t>(j)].append(nextNode++);
            meshNodes.emplace_back(meshes[static_cast<size_t>(m)].name, m);
        }
    }
    for (int j = 0; j < jointCount; ++j) {
        const Joint& joint = chain.joints[static_cast<size_t>(j)];
        QJsonObject node;
        node.insert(QLatin1String("name"), joint.name);
        const gp_Vec t =
            (j == 0) ? chain.root().attachMm : joint.attachMm;
        node.insert(QLatin1String("translation"),
                    vec3Json(t.X(), t.Y(), t.Z()));
        if (!childrenOf[static_cast<size_t>(j)].isEmpty())
            node.insert(QLatin1String("children"),
                        childrenOf[static_cast<size_t>(j)]);
        nodes.append(node);
    }
    for (const auto& mn : meshNodes) {
        QJsonObject node;
        node.insert(QLatin1String("name"), mn.first);
        node.insert(QLatin1String("mesh"), mn.second);
        nodes.append(node);
    }

    // ---- Animation ------------------------------------------------------
    // Baked key list: the authored keys for hold/cycle; for pingpong the
    // LOOP WINDOW (boundary samples + interior keys, times shifted to
    // start at 0) followed by its mirror — one GLB playthrough is exactly
    // one loop period, the same content the WebP loops over. The authored
    // window rides in extras.loop for consumers that want the raw clip.
    std::vector<DenseKey> baked;
    if (clip.loop == LoopMode::PingPong && clip.keys.size() > 1) {
        const double start = clip.loopStart;
        const double end = clip.loopEnd;
        const auto sampleKey = [&clip](double t) {
            const PoseSample s = clip.sampleAt(t, false);
            DenseKey k;
            k.t = t;
            k.rootPosMm = s.rootPosMm;
            k.rootRot = s.rootRot;
            k.values = s.values;
            return k;
        };
        baked.push_back(sampleKey(start));
        for (const DenseKey& key : clip.keys)
            if (key.t > start + 1e-9 && key.t < end - 1e-9)
                baked.push_back(key);
        baked.push_back(sampleKey(end));
        for (DenseKey& key : baked)
            key.t -= start;
        const double window = end - start;
        for (size_t k = baked.size() - 1; k-- > 0;) {
            DenseKey mirrored = baked[k];
            mirrored.t = 2.0 * window - mirrored.t;
            baked.push_back(mirrored);
        }
    } else {
        baked = clip.keys;
    }
    std::vector<float> timesF;
    timesF.reserve(baked.size());
    for (const DenseKey& key : baked)
        timesF.push_back(static_cast<float>(key.t));
    const int timeView =
        bin.addView(timesF.data(),
                    static_cast<qsizetype>(timesF.size() * sizeof(float)),
                    std::nullopt);
    QJsonObject timeAcc;
    timeAcc.insert(QLatin1String("bufferView"), timeView);
    timeAcc.insert(QLatin1String("componentType"), kFloat);
    timeAcc.insert(QLatin1String("count"),
                   static_cast<double>(timesF.size()));
    timeAcc.insert(QLatin1String("type"), QStringLiteral("SCALAR"));
    QJsonArray tmin;
    tmin.append(static_cast<double>(timesF.front()));
    QJsonArray tmax;
    tmax.append(static_cast<double>(timesF.back()));
    timeAcc.insert(QLatin1String("min"), tmin);
    timeAcc.insert(QLatin1String("max"), tmax);
    accessors.append(timeAcc);
    const int timeAccIndex = accessors.size() - 1;

    QJsonArray samplers;
    QJsonArray channels;

    const auto addRotationChannel =
        [&](int jointIndex, const std::vector<gp_Quaternion>& quats) {
            // Sign-align consecutive quaternions: naive component lerp in a
            // viewer takes the long way round a sign flip.
            std::vector<float> out;
            out.reserve(quats.size() * 4);
            gp_Quaternion prev;
            bool havePrev = false;
            for (gp_Quaternion q : quats) {
                if (havePrev) {
                    const double dot = q.X() * prev.X() + q.Y() * prev.Y()
                                       + q.Z() * prev.Z()
                                       + q.W() * prev.W();
                    if (dot < 0)
                        q = gp_Quaternion(-q.X(), -q.Y(), -q.Z(),
                                          -q.W());
                }
                prev = q;
                havePrev = true;
                out.push_back(static_cast<float>(q.X()));
                out.push_back(static_cast<float>(q.Y()));
                out.push_back(static_cast<float>(q.Z()));
                out.push_back(static_cast<float>(q.W()));
            }
            const int view = bin.addView(
                out.data(),
                static_cast<qsizetype>(out.size() * sizeof(float)),
                std::nullopt);
            QJsonObject acc;
            acc.insert(QLatin1String("bufferView"), view);
            acc.insert(QLatin1String("componentType"), kFloat);
            acc.insert(QLatin1String("count"),
                       static_cast<double>(out.size() / 4));
            acc.insert(QLatin1String("type"), QStringLiteral("VEC4"));
            accessors.append(acc);
            QJsonObject sampler;
            sampler.insert(QLatin1String("input"), timeAccIndex);
            sampler.insert(QLatin1String("output"), accessors.size() - 1);
            sampler.insert(QLatin1String("interpolation"),
                           QStringLiteral("LINEAR"));
            samplers.append(sampler);
            QJsonObject target;
            target.insert(QLatin1String("node"), 1 + jointIndex);
            target.insert(QLatin1String("path"),
                          QStringLiteral("rotation"));
            QJsonObject channel;
            channel.insert(QLatin1String("sampler"), samplers.size() - 1);
            channel.insert(QLatin1String("target"), target);
            channels.append(channel);
        };

    const auto addTranslationChannel =
        [&](int jointIndex, const std::vector<gp_Vec>& vecs) {
            std::vector<float> out;
            out.reserve(vecs.size() * 3);
            for (const gp_Vec& v : vecs) {
                out.push_back(static_cast<float>(v.X()));
                out.push_back(static_cast<float>(v.Y()));
                out.push_back(static_cast<float>(v.Z()));
            }
            const int view = bin.addView(
                out.data(),
                static_cast<qsizetype>(out.size() * sizeof(float)),
                std::nullopt);
            QJsonObject acc;
            acc.insert(QLatin1String("bufferView"), view);
            acc.insert(QLatin1String("componentType"), kFloat);
            acc.insert(QLatin1String("count"),
                       static_cast<double>(out.size() / 3));
            acc.insert(QLatin1String("type"), QStringLiteral("VEC3"));
            accessors.append(acc);
            QJsonObject sampler;
            sampler.insert(QLatin1String("input"), timeAccIndex);
            sampler.insert(QLatin1String("output"), accessors.size() - 1);
            sampler.insert(QLatin1String("interpolation"),
                           QStringLiteral("LINEAR"));
            samplers.append(sampler);
            QJsonObject target;
            target.insert(QLatin1String("node"), 1 + jointIndex);
            target.insert(QLatin1String("path"),
                          QStringLiteral("translation"));
            QJsonObject channel;
            channel.insert(QLatin1String("sampler"), samplers.size() - 1);
            channel.insert(QLatin1String("target"), target);
            channels.append(channel);
        };

    for (int j = 0; j < jointCount; ++j) {
        const Joint& joint = chain.joints[static_cast<size_t>(j)];
        bool animated = false;
        if (j == 0) {
            // A glTF node carries ONE translation and ONE rotation, so the
            // root placement and the root's own typed channel compose here
            // exactly like worldTransforms does:
            //   T = root_pos + R_root(slide),  R = root_rot * typed.
            std::vector<gp_Vec> pos;
            std::vector<gp_Quaternion> rot;
            for (const DenseKey& key : baked) {
                const JointChannel& ch = key.values[0];
                gp_Quaternion typedRot;
                gp_Vec slide(0, 0, 0);
                switch (joint.type) {
                case JointType::Ball:
                    typedRot = ch.rot;
                    break;
                case JointType::Revolute:
                    typedRot.SetVectorAndAngle(joint.axis, ch.scalar);
                    break;
                case JointType::Prismatic:
                    slide = joint.axis * ch.scalar;
                    break;
                case JointType::Fixed:
                case JointType::Free:
                    break;
                }
                pos.push_back(key.rootPosMm
                              + key.rootRot.Multiply(slide));
                rot.push_back(key.rootRot * typedRot);
            }
            addTranslationChannel(j, pos);
            addRotationChannel(j, rot);
            animated = true;
        } else if (joint.type == JointType::Ball) {
            bool allRest = true;
            for (const DenseKey& key : clip.keys)
                if (!quatIsRest(key.values[static_cast<size_t>(j)].rot))
                    allRest = false;
            if (!allRest) {
                std::vector<gp_Quaternion> rot;
                for (const DenseKey& key : baked)
                    rot.push_back(
                        key.values[static_cast<size_t>(j)].rot);
                addRotationChannel(j, rot);
                animated = true;
            }
        } else if (joint.type == JointType::Revolute) {
            bool allRest = true;
            for (const DenseKey& key : clip.keys)
                if (std::abs(
                        key.values[static_cast<size_t>(j)].scalar)
                    > 1e-12)
                    allRest = false;
            if (!allRest) {
                std::vector<gp_Quaternion> rot;
                for (const DenseKey& key : baked) {
                    gp_Quaternion q;
                    q.SetVectorAndAngle(
                        joint.axis,
                        key.values[static_cast<size_t>(j)].scalar);
                    rot.push_back(q);
                }
                addRotationChannel(j, rot);
                animated = true;
            }
        } else if (joint.type == JointType::Prismatic) {
            bool allRest = true;
            for (const DenseKey& key : clip.keys)
                if (std::abs(
                        key.values[static_cast<size_t>(j)].scalar)
                    > 1e-12)
                    allRest = false;
            if (!allRest) {
                std::vector<gp_Vec> pos;
                for (const DenseKey& key : baked)
                    pos.push_back(
                        joint.attachMm
                        + joint.axis
                              * key.values[static_cast<size_t>(j)]
                                    .scalar);
                addTranslationChannel(j, pos);
                animated = true;
            }
        }
        if (animated)
            ++result.animatedNodes;
    }

    // ---- Assembly -------------------------------------------------------
    QJsonObject root;
    {
        QJsonObject asset;
        asset.insert(QLatin1String("version"), QStringLiteral("2.0"));
        asset.insert(QLatin1String("generator"),
                     QStringLiteral("vikiCAD anim"));
        root.insert(QLatin1String("asset"), asset);
    }
    root.insert(QLatin1String("scene"), 0);
    {
        QJsonArray sceneNodes;
        sceneNodes.append(0);
        QJsonObject scene;
        scene.insert(QLatin1String("name"), clip.id);
        scene.insert(QLatin1String("nodes"), sceneNodes);
        QJsonArray scenes;
        scenes.append(scene);
        root.insert(QLatin1String("scenes"), scenes);
    }
    root.insert(QLatin1String("nodes"), nodes);
    root.insert(QLatin1String("meshes"), meshesJson);
    {
        QJsonArray materials;
        materials.append(materialJson(QStringLiteral("base"),
                                      avatar.baseColor, avatar.roughness,
                                      avatar.metallic));
        materials.append(materialJson(QStringLiteral("accent"),
                                      avatar.accentColor, avatar.roughness,
                                      avatar.metallic));
        root.insert(QLatin1String("materials"), materials);
    }
    {
        QJsonObject animation;
        animation.insert(QLatin1String("name"), clip.id);
        animation.insert(QLatin1String("samplers"), samplers);
        animation.insert(QLatin1String("channels"), channels);
        QJsonArray animations;
        animations.append(animation);
        root.insert(QLatin1String("animations"), animations);
    }
    root.insert(QLatin1String("accessors"), accessors);
    root.insert(QLatin1String("bufferViews"), bin.views);
    {
        QJsonObject buffer;
        buffer.insert(QLatin1String("byteLength"),
                      static_cast<double>(bin.bytes.size()));
        QJsonArray buffers;
        buffers.append(buffer);
        root.insert(QLatin1String("buffers"), buffers);
    }
    {
        // Everything a smarter consumer needs to loop properly.
        QJsonObject loop;
        loop.insert(QLatin1String("mode"),
                    clip.loop == LoopMode::PingPong
                        ? QStringLiteral("pingpong")
                        : (clip.loop == LoopMode::Cycle
                               ? QStringLiteral("cycle")
                               : QStringLiteral("hold")));
        loop.insert(QLatin1String("start"), clip.loopStart);
        loop.insert(QLatin1String("end"), clip.loopEnd);
        QJsonObject extras;
        extras.insert(QLatin1String("chain"), chain.id);
        extras.insert(QLatin1String("pose"), clip.id);
        extras.insert(QLatin1String("fps"), clip.fps);
        extras.insert(QLatin1String("loop"), loop);
        root.insert(QLatin1String("extras"), extras);
    }

    // ---- GLB container --------------------------------------------------
    QByteArray json =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4 != 0)
        json.append(' ');
    QByteArray binChunk = bin.bytes;
    while (binChunk.size() % 4 != 0)
        binChunk.append('\0');

    QByteArray glb;
    const auto append32 = [&glb](quint32 v) {
        char raw[4];
        raw[0] = static_cast<char>(v & 0xFF);
        raw[1] = static_cast<char>((v >> 8) & 0xFF);
        raw[2] = static_cast<char>((v >> 16) & 0xFF);
        raw[3] = static_cast<char>((v >> 24) & 0xFF);
        glb.append(raw, 4);
    };
    append32(0x46546C67u); // 'glTF'
    append32(2u);
    append32(static_cast<quint32>(12 + 8 + json.size() + 8
                                  + binChunk.size()));
    append32(static_cast<quint32>(json.size()));
    append32(0x4E4F534Au); // 'JSON'
    glb.append(json);
    append32(static_cast<quint32>(binChunk.size()));
    append32(0x004E4942u); // 'BIN'
    glb.append(binChunk);

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return failGlb(QStringLiteral("glb: cannot write %1").arg(path));
    if (out.write(glb) != glb.size())
        return failGlb(QStringLiteral("glb: short write on %1").arg(path));
    out.close();

    result.bytes = glb.size();
    result.ok = true;
    return result;
}

} // namespace anim
} // namespace viki
