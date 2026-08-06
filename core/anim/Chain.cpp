#include "anim/Chain.h"

#include <algorithm>
#include <cmath>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace viki {
namespace anim {

namespace {

constexpr double kMetersToMm = 1000.0;
constexpr double kDegToRad = M_PI / 180.0;

// Read a 3-number JSON array. Returns false (and leaves `out` alone) when the
// value is missing; sets `err` when it is present but malformed.
bool readVec3(const QJsonObject& obj, const QString& key, gp_Vec& out,
              QString& err)
{
    if (!obj.contains(key))
        return false;
    const QJsonValue v = obj.value(key);
    if (!v.isArray() || v.toArray().size() != 3) {
        err = QStringLiteral("\"%1\" must be an array of 3 numbers").arg(key);
        return false;
    }
    const QJsonArray a = v.toArray();
    for (const QJsonValue& c : a) {
        if (!c.isDouble()) {
            err = QStringLiteral("\"%1\" must contain numbers only").arg(key);
            return false;
        }
    }
    out = gp_Vec(a[0].toDouble(), a[1].toDouble(), a[2].toDouble());
    return true;
}

std::optional<JointType> jointTypeFromString(const QString& s)
{
    if (s == QLatin1String("ball")) return JointType::Ball;
    if (s == QLatin1String("revolute")) return JointType::Revolute;
    if (s == QLatin1String("prismatic")) return JointType::Prismatic;
    if (s == QLatin1String("fixed")) return JointType::Fixed;
    if (s == QLatin1String("free")) return JointType::Free;
    return {};
}

ChainResult fail(const QString& message)
{
    ChainResult r;
    r.error = message;
    return r;
}

} // namespace

int Chain::indexOf(const QString& jointName) const
{
    for (size_t i = 0; i < joints.size(); ++i)
        if (joints[i].name == jointName)
            return static_cast<int>(i);
    return -1;
}

ChainResult chainFromJson(const QJsonObject& obj,
                          std::optional<double> scaleOverrideM)
{
    if (obj.value(QLatin1String("schema_version")).toString()
        != QLatin1String("1"))
        return fail(QStringLiteral("chain: unsupported schema_version "
                                   "(expected \"1\")"));

    ChainResult res;
    Chain& chain = res.chain;
    chain.id = obj.value(QLatin1String("id")).toString();
    // The schema pattern doubles as a path-traversal guard: ids end up in
    // output file names.
    static const QRegularExpression kIdPattern(
        QStringLiteral("^[a-z0-9-]+$"));
    if (!kIdPattern.match(chain.id).hasMatch())
        return fail(QStringLiteral("chain: \"id\" must match [a-z0-9-]+"));
    chain.name = obj.value(QLatin1String("name")).toString();

    // Extension field (tolerated by schema v1, documented in the GenMov3D
    // contract): pose3d files written for one of the LISTED chains load on
    // this chain too — its extra joints stay at rest. This is how
    // humanoid-14 renders the existing humanoid-12 pose bank with neutral
    // hands.
    const QJsonValue acceptsVal =
        obj.value(QLatin1String("accepts_pose_chains"));
    if (acceptsVal.isArray()) {
        for (const QJsonValue& v : acceptsVal.toArray()) {
            const QString id = v.toString();
            if (!kIdPattern.match(id).hasMatch())
                return fail(QStringLiteral(
                    "chain: accepts_pose_chains entries must match "
                    "[a-z0-9-]+"));
            chain.acceptsPoseChains.append(id);
        }
    } else if (!acceptsVal.isUndefined() && !acceptsVal.isNull()) {
        return fail(QStringLiteral(
            "chain: accepts_pose_chains must be an array of chain ids"));
    }

    if (obj.contains(QLatin1String("scale_reference"))
        && !obj.value(QLatin1String("scale_reference")).isDouble())
        return fail(QStringLiteral("chain: scale_reference must be a "
                                   "number"));
    double scaleM = obj.value(QLatin1String("scale_reference")).toDouble(1.0);
    if (scaleOverrideM)
        scaleM = *scaleOverrideM;
    if (!(scaleM > 0.0))
        return fail(QStringLiteral("chain: scale_reference must be > 0"));
    chain.scaleMm = scaleM * kMetersToMm;

    const QJsonValue jointsVal = obj.value(QLatin1String("joints"));
    if (!jointsVal.isArray() || jointsVal.toArray().isEmpty())
        return fail(QStringLiteral("chain: \"joints\" must be a non-empty "
                                   "array"));
    const QJsonArray jointsArr = jointsVal.toArray();

    // First pass: parse every joint, remember the parent NAME for now.
    std::vector<Joint> parsed;
    QStringList parentNames;
    QMap<QString, int> byName;
    for (int i = 0; i < jointsArr.size(); ++i) {
        if (!jointsArr[i].isObject())
            return fail(QStringLiteral("chain: joint %1 is not an object")
                            .arg(i));
        const QJsonObject jo = jointsArr[i].toObject();
        Joint j;
        j.name = jo.value(QLatin1String("name")).toString();
        if (j.name.isEmpty())
            return fail(QStringLiteral("chain: joint %1 has no name").arg(i));
        if (byName.contains(j.name))
            return fail(QStringLiteral("chain: duplicate joint name \"%1\"")
                            .arg(j.name));
        byName.insert(j.name, i);

        const auto type =
            jointTypeFromString(jo.value(QLatin1String("type")).toString());
        if (!type)
            return fail(QStringLiteral("chain: joint \"%1\" has unknown type "
                                       "\"%2\"")
                            .arg(j.name,
                                 jo.value(QLatin1String("type")).toString()));
        j.type = *type;

        QString err;
        gp_Vec v;
        if (readVec3(jo, QStringLiteral("axis"), v, err)) {
            if (v.Magnitude() < 1e-12)
                return fail(QStringLiteral("chain: joint \"%1\" has a zero "
                                           "axis")
                                .arg(j.name));
            j.axis = v.Normalized();
        } else if (!err.isEmpty()) {
            return fail(QStringLiteral("chain: joint \"%1\": %2")
                            .arg(j.name, err));
        } else if (j.type == JointType::Revolute
                   || j.type == JointType::Prismatic) {
            return fail(QStringLiteral("chain: joint \"%1\" (%2) requires an "
                                       "\"axis\"")
                            .arg(j.name,
                                 jo.value(QLatin1String("type")).toString()));
        }

        if (readVec3(jo, QStringLiteral("attach"), v, err))
            j.attachMm = v * chain.scaleMm; // fractions of scale_reference
        else if (!err.isEmpty())
            return fail(QStringLiteral("chain: joint \"%1\": %2")
                            .arg(j.name, err));

        if (jo.contains(QLatin1String("length"))
            && !jo.value(QLatin1String("length")).isDouble())
            return fail(QStringLiteral("chain: joint \"%1\" length must be "
                                       "a number")
                            .arg(j.name));
        const double lengthFrac = jo.value(QLatin1String("length")).toDouble(0);
        if (lengthFrac < 0)
            return fail(QStringLiteral("chain: joint \"%1\" has a negative "
                                       "length")
                            .arg(j.name));
        j.lengthMm = lengthFrac * chain.scaleMm;

        if (readVec3(jo, QStringLiteral("rest_direction"), v, err)) {
            if (v.Magnitude() < 1e-12)
                return fail(QStringLiteral("chain: joint \"%1\" has a zero "
                                           "rest_direction")
                                .arg(j.name));
            j.restDirection = v.Normalized();
        } else if (!err.isEmpty()) {
            return fail(QStringLiteral("chain: joint \"%1\": %2")
                            .arg(j.name, err));
        } else if (j.lengthMm > 0) {
            return fail(QStringLiteral("chain: joint \"%1\" has a length but "
                                       "no rest_direction")
                            .arg(j.name));
        }

        const QJsonValue limitsVal = jo.value(QLatin1String("limits_deg"));
        if (limitsVal.isObject()) {
            const QJsonObject lo = limitsVal.toObject();
            for (auto it = lo.begin(); it != lo.end(); ++it) {
                const QJsonArray pair = it.value().toArray();
                if (pair.size() != 2 || !pair[0].isDouble()
                    || !pair[1].isDouble())
                    return fail(QStringLiteral("chain: joint \"%1\" limit "
                                               "\"%2\" must be [min, max]")
                                    .arg(j.name, it.key()));
                // Degrees for rotations, metres for prismatic (per schema).
                const double f = (j.type == JointType::Prismatic)
                                     ? kMetersToMm
                                     : kDegToRad;
                JointLimit lim;
                lim.min = pair[0].toDouble() * f;
                lim.max = pair[1].toDouble() * f;
                if (lim.min > lim.max)
                    return fail(QStringLiteral("chain: joint \"%1\" limit "
                                               "\"%2\" has min > max")
                                    .arg(j.name, it.key()));
                j.limits.insert(it.key().toLower(), lim);
            }
        }

        const QJsonValue parentVal = jo.value(QLatin1String("parent"));
        parentNames.append(parentVal.isString() ? parentVal.toString()
                                                : QString());
        parsed.push_back(j);
    }

    // Resolve parents and check for exactly one root.
    int rootCount = 0;
    std::vector<int> parentIdx(parsed.size(), -1);
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (parentNames[static_cast<int>(i)].isEmpty()) {
            ++rootCount;
            continue;
        }
        const auto it = byName.constFind(parentNames[static_cast<int>(i)]);
        if (it == byName.constEnd())
            return fail(QStringLiteral("chain: joint \"%1\" references "
                                       "unknown parent \"%2\"")
                            .arg(parsed[i].name,
                                 parentNames[static_cast<int>(i)]));
        if (*it == static_cast<int>(i))
            return fail(QStringLiteral("chain: joint \"%1\" is its own "
                                       "parent")
                            .arg(parsed[i].name));
        parentIdx[i] = *it;
    }
    if (rootCount != 1)
        return fail(QStringLiteral("chain: expected exactly one root joint "
                                   "(parent null), found %1")
                        .arg(rootCount));
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (parentIdx[i] == -1 && parsed[i].type != JointType::Free)
            res.warnings.append(
                QStringLiteral("chain: root joint \"%1\" is not of type "
                               "\"free\"; root_pos/root_rot compose with "
                               "its own channel")
                    .arg(parsed[i].name));
    }
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (parentIdx[i] != -1 && parsed[i].type == JointType::Free)
            return fail(QStringLiteral("chain: joint \"%1\" is \"free\" but "
                                       "is not the root")
                            .arg(parsed[i].name));
    }

    // Topological sort (Kahn, stable): parents always land before children.
    // Detects cycles at the same time.
    std::vector<int> order;
    order.reserve(parsed.size());
    std::vector<bool> placed(parsed.size(), false);
    bool progressed = true;
    while (order.size() < parsed.size() && progressed) {
        progressed = false;
        for (size_t i = 0; i < parsed.size(); ++i) {
            if (placed[i])
                continue;
            const int p = parentIdx[i];
            if (p == -1 || placed[static_cast<size_t>(p)]) {
                order.push_back(static_cast<int>(i));
                placed[i] = true;
                progressed = true;
            }
        }
    }
    if (order.size() < parsed.size()) {
        QStringList cycle;
        for (size_t i = 0; i < parsed.size(); ++i)
            if (!placed[i])
                cycle.append(parsed[i].name);
        return fail(QStringLiteral("chain: parent cycle involving: %1")
                        .arg(cycle.join(QLatin1String(", "))));
    }

    std::vector<int> newIndex(parsed.size(), -1);
    for (size_t k = 0; k < order.size(); ++k)
        newIndex[static_cast<size_t>(order[k])] = static_cast<int>(k);
    chain.joints.reserve(parsed.size());
    for (const int oldIdx : order) {
        Joint j = parsed[static_cast<size_t>(oldIdx)];
        const int p = parentIdx[static_cast<size_t>(oldIdx)];
        j.parent = (p == -1) ? -1 : newIndex[static_cast<size_t>(p)];
        chain.joints.push_back(j);
    }

    res.ok = true;
    return res;
}

ChainResult loadChainFile(const QString& path,
                          std::optional<double> scaleOverrideM)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("chain: cannot read %1").arg(path));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull())
        return fail(QStringLiteral("chain: %1: invalid JSON (%2)")
                        .arg(path, perr.errorString()));
    if (!doc.isObject())
        return fail(QStringLiteral("chain: %1: top level is not an object")
                        .arg(path));
    return chainFromJson(doc.object(), scaleOverrideM);
}

} // namespace anim
} // namespace viki
