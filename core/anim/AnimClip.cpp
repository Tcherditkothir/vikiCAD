#include "anim/AnimClip.h"

#include <algorithm>
#include <cmath>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <gp_QuaternionSLerp.hxx>

namespace viki {
namespace anim {

namespace {

constexpr double kMetersToMm = 1000.0;
constexpr double kDegToRad = M_PI / 180.0;

// LESSONS: every multiplicative input deserves a numbered cap, refused
// BEFORE the work. duration x fps x 2 (pingpong) bounds the frame count;
// 300 s at the schema's 60 fps ceiling is 36 000 frames — already minutes
// of rendering, and far beyond any pose loop.
constexpr double kMaxClipSeconds = 300.0;

// Defensive ceiling for programmatic (non-parsed) clips; unreachable
// through clipFromJson thanks to kMaxClipSeconds.
constexpr size_t kMaxFrameCount = 200000;

// QJsonValue::toDouble() silently reads strings as 0.0 — every element of
// a numeric array must really be a number or a generator glitch becomes a
// silently wrong pose with exit code 0.
bool allNumbers(const QJsonArray& a)
{
    for (const QJsonValue& v : a)
        if (!v.isDouble())
            return false;
    return true;
}

ClipResult fail(const QString& message)
{
    ClipResult r;
    r.error = message;
    return r;
}

gp_Quaternion eulerXyzDeg(double rxDeg, double ryDeg, double rzDeg)
{
    gp_Quaternion q;
    // Schema convention: extrinsic X→Y→Z (fixed axes, X applied first).
    q.SetEulerAngles(gp_Extrinsic_XYZ, rxDeg * kDegToRad, ryDeg * kDegToRad,
                     rzDeg * kDegToRad);
    return q;
}

// The stop declared for a scalar joint: key "x" by convention, or the sole
// entry whatever its key.
const JointLimit* scalarLimit(const Joint& j)
{
    const auto it = j.limits.constFind(QStringLiteral("x"));
    if (it != j.limits.constEnd())
        return &it.value();
    if (j.limits.size() == 1)
        return &j.limits.first();
    return nullptr;
}

void warnIfBeyond(QStringList& warnings, const QString& jointName,
                  const QString& channel, double value, const JointLimit& lim,
                  double t, bool millimetres)
{
    if (value >= lim.min - 1e-9 && value <= lim.max + 1e-9)
        return;
    const double f = millimetres ? 1.0 : 1.0 / kDegToRad;
    const QString unit = millimetres ? QStringLiteral("mm")
                                     : QStringLiteral("deg");
    warnings.append(QStringLiteral("t=%1: %2.%3 = %4 %5 exceeds stop "
                                   "[%6, %7]")
                        .arg(t)
                        .arg(jointName, channel)
                        .arg(value * f, 0, 'f', 1)
                        .arg(unit)
                        .arg(lim.min * f, 0, 'f', 1)
                        .arg(lim.max * f, 0, 'f', 1));
}

} // namespace

PoseSample AnimClip::sampleAt(double t, bool applyBreath) const
{
    PoseSample out;
    if (keys.empty())
        return out;

    const DenseKey* a = &keys.front();
    const DenseKey* b = &keys.front();
    double u = 0.0;
    if (t <= keys.front().t) {
        // clamp before the first key
    } else if (t >= keys.back().t) {
        a = b = &keys.back();
    } else {
        // last key with key.t <= t, and its successor
        size_t hi = 1;
        while (keys[hi].t <= t)
            ++hi;
        a = &keys[hi - 1];
        b = &keys[hi];
        u = (t - a->t) / (b->t - a->t);
    }

    out.rootPosMm = a->rootPosMm * (1.0 - u) + b->rootPosMm * u;
    if (a == b || u <= 0.0) {
        out.rootRot = a->rootRot;
        out.values = a->values;
    } else {
        gp_QuaternionSLerp rootLerp(a->rootRot, b->rootRot);
        rootLerp.Interpolate(u, out.rootRot);
        out.values.resize(a->values.size());
        for (size_t i = 0; i < a->values.size(); ++i) {
            gp_QuaternionSLerp lerp(a->values[i].rot, b->values[i].rot);
            lerp.Interpolate(u, out.values[i].rot);
            out.values[i].scalar =
                a->values[i].scalar * (1.0 - u) + b->values[i].scalar * u;
        }
    }

    if (applyBreath && hasBreath && breathAmplitudeRad > 0.0) {
        // Cycle loops jump straight from the end frame back to the start:
        // a sine on absolute time pops at the seam whenever the window is
        // not a multiple of the period. Fold the phase onto a whole number
        // of periods per loop so the oscillation closes exactly.
        double phase = 2.0 * M_PI * t / breathPeriodS;
        if (loop == LoopMode::Cycle) {
            const double window = loopEnd - loopStart;
            if (window > 1e-9) {
                const int cycles = std::max(
                    1, static_cast<int>(
                           std::llround(window / breathPeriodS)));
                phase = 2.0 * M_PI * cycles * (t - loopStart) / window;
            }
        }
        const double delta = breathAmplitudeRad * std::sin(phase);
        for (const int idx : breathJoints) {
            JointChannel& ch = out.values[static_cast<size_t>(idx)];
            gp_Quaternion extra;
            extra.SetVectorAndAngle(gp_Vec(1, 0, 0), delta);
            ch.rot = ch.rot * extra; // extra local X rotation
            ch.scalar += delta;      // harmless for ball, used by revolute
        }
    }
    return out;
}

std::vector<double> AnimClip::frameTimes() const
{
    std::vector<double> times;
    if (keys.empty() || fps <= 0)
        return times;
    const double step = 1.0 / fps;
    const double start = (loop == LoopMode::Hold) ? 0.0 : loopStart;
    const double end = loopEnd;
    if (end - start < step * 0.5) {
        times.push_back(end);
        return times;
    }
    const long long wanted = std::llround((end - start) * fps);
    if (wanted > static_cast<long long>(kMaxFrameCount))
        return times; // empty — callers refuse an empty frame list
    const int n = std::max(1, static_cast<int>(wanted));
    switch (loop) {
    case LoopMode::Hold:
        for (int i = 0; i < n; ++i)
            times.push_back(start + i * step);
        times.push_back(end);
        break;
    case LoopMode::Cycle:
        // [start, end): the end frame equals the start by construction, so
        // rendering it would stutter the loop.
        for (int i = 0; i < n; ++i)
            times.push_back(start + i * step);
        break;
    case LoopMode::PingPong:
        for (int i = 0; i < n; ++i)
            times.push_back(start + i * step);
        times.push_back(end);
        for (int i = n - 1; i >= 1; --i)
            times.push_back(start + i * step);
        break;
    }
    return times;
}

ClipResult clipFromJson(const QJsonObject& obj, const Chain& chain)
{
    if (obj.value(QLatin1String("schema_version")).toString()
        != QLatin1String("1"))
        return fail(QStringLiteral("pose3d: unsupported schema_version "
                                   "(expected \"1\")"));

    ClipResult res;
    AnimClip& clip = res.clip;
    clip.id = obj.value(QLatin1String("id")).toString();
    // Schema pattern, enforced: the id names the output files, so this is
    // also the guard against ../ traversal out of --out.
    static const QRegularExpression kIdPattern(
        QStringLiteral("^[a-z0-9-]+$"));
    if (!kIdPattern.match(clip.id).hasMatch())
        return fail(QStringLiteral("pose3d: \"id\" must match [a-z0-9-]+"));
    clip.chainId = obj.value(QLatin1String("chain")).toString();
    if (clip.chainId != chain.id)
        return fail(QStringLiteral("pose3d \"%1\" animates chain \"%2\" but "
                                   "chain \"%3\" was loaded")
                        .arg(clip.id, clip.chainId, chain.id));
    clip.nameFr = obj.value(QLatin1String("name_fr")).toString();
    clip.nameEn = obj.value(QLatin1String("name_en")).toString();
    clip.source = obj.value(QLatin1String("source")).toString();

    const int fps = obj.value(QLatin1String("fps")).toInt(-1);
    if (fps < 12 || fps > 60)
        return fail(QStringLiteral("pose3d: fps must be an integer in "
                                   "[12, 60]"));
    clip.fps = fps;

    const QJsonValue kfVal = obj.value(QLatin1String("keyframes"));
    if (!kfVal.isArray() || kfVal.toArray().isEmpty())
        return fail(QStringLiteral("pose3d: \"keyframes\" must be a "
                                   "non-empty array"));

    // Sort keyframes by time before densifying (the schema does not promise
    // an ordered file).
    std::vector<QJsonObject> kfs;
    for (const QJsonValue& v : kfVal.toArray()) {
        if (!v.isObject())
            return fail(QStringLiteral("pose3d: keyframe is not an object"));
        kfs.push_back(v.toObject());
    }
    std::stable_sort(kfs.begin(), kfs.end(),
                     [](const QJsonObject& a, const QJsonObject& b) {
                         return a.value(QLatin1String("t")).toDouble()
                                < b.value(QLatin1String("t")).toDouble();
                     });

    // Rest pose = carry-forward source before the first keyframe.
    DenseKey cur;
    cur.rootPosMm = chain.root().attachMm;
    cur.values.resize(chain.joints.size());

    for (const QJsonObject& kf : kfs) {
        const QJsonValue tVal = kf.value(QLatin1String("t"));
        if (!tVal.isDouble() || tVal.toDouble() < 0)
            return fail(QStringLiteral("pose3d: keyframe \"t\" must be a "
                                       "number >= 0"));
        DenseKey key = cur;
        key.t = tVal.toDouble();
        if (!clip.keys.empty()
            && key.t <= clip.keys.back().t + 1e-9)
            return fail(QStringLiteral("pose3d: duplicate or non-increasing "
                                       "keyframe at t=%1")
                            .arg(key.t));

        const QJsonValue rp = kf.value(QLatin1String("root_pos"));
        if (rp.isArray()) {
            const QJsonArray a = rp.toArray();
            if (a.size() != 3 || !allNumbers(a))
                return fail(QStringLiteral("pose3d: root_pos must have 3 "
                                           "numbers (t=%1)")
                                .arg(key.t));
            key.rootPosMm = gp_Vec(a[0].toDouble(), a[1].toDouble(),
                                   a[2].toDouble())
                            * kMetersToMm;
        } else if (!rp.isUndefined()) {
            return fail(QStringLiteral("pose3d: root_pos must be an array "
                                       "(t=%1)")
                            .arg(key.t));
        }

        const QJsonValue rr = kf.value(QLatin1String("root_rot"));
        if (rr.isArray()) {
            const QJsonArray a = rr.toArray();
            if (a.size() != 3 || !allNumbers(a))
                return fail(QStringLiteral("pose3d: root_rot must have 3 "
                                           "numbers (t=%1)")
                                .arg(key.t));
            key.rootRot = eulerXyzDeg(a[0].toDouble(), a[1].toDouble(),
                                      a[2].toDouble());
        } else if (!rr.isUndefined()) {
            return fail(QStringLiteral("pose3d: root_rot must be an array "
                                       "(t=%1)")
                            .arg(key.t));
        }

        const QJsonValue jointsVal = kf.value(QLatin1String("joints"));
        if (jointsVal.isObject()) {
            const QJsonObject jo = jointsVal.toObject();
            for (auto it = jo.begin(); it != jo.end(); ++it) {
                const int idx = chain.indexOf(it.key());
                if (idx < 0)
                    return fail(QStringLiteral("pose3d: unknown joint \"%1\" "
                                               "(t=%2)")
                                    .arg(it.key())
                                    .arg(key.t));
                const Joint& joint = chain.joints[static_cast<size_t>(idx)];
                JointChannel& ch = key.values[static_cast<size_t>(idx)];
                switch (joint.type) {
                case JointType::Ball: {
                    if (!it.value().isArray()
                        || it.value().toArray().size() != 3
                        || !allNumbers(it.value().toArray()))
                        return fail(QStringLiteral(
                                        "pose3d: joint \"%1\" is a ball and "
                                        "needs [rx, ry, rz] numbers, deg "
                                        "(t=%2)")
                                        .arg(it.key())
                                        .arg(key.t));
                    const QJsonArray a = it.value().toArray();
                    const double deg[3] = {a[0].toDouble(), a[1].toDouble(),
                                           a[2].toDouble()};
                    static const QString axes[3] = {QStringLiteral("x"),
                                                    QStringLiteral("y"),
                                                    QStringLiteral("z")};
                    for (int c = 0; c < 3; ++c) {
                        const auto lim = joint.limits.constFind(axes[c]);
                        if (lim != joint.limits.constEnd())
                            warnIfBeyond(res.warnings, joint.name, axes[c],
                                         deg[c] * kDegToRad, lim.value(),
                                         key.t, false);
                    }
                    ch.rot = eulerXyzDeg(deg[0], deg[1], deg[2]);
                    break;
                }
                case JointType::Revolute:
                case JointType::Prismatic: {
                    if (!it.value().isDouble())
                        return fail(QStringLiteral(
                                        "pose3d: joint \"%1\" is a %2 and "
                                        "needs a single number (t=%3)")
                                        .arg(it.key(),
                                             joint.type
                                                     == JointType::Revolute
                                                 ? QStringLiteral("revolute")
                                                 : QStringLiteral(
                                                       "prismatic"))
                                        .arg(key.t));
                    const bool prismatic =
                        joint.type == JointType::Prismatic;
                    const double value = it.value().toDouble()
                                         * (prismatic ? kMetersToMm
                                                      : kDegToRad);
                    if (const JointLimit* lim = scalarLimit(joint))
                        warnIfBeyond(res.warnings, joint.name,
                                     QStringLiteral("x"), value, *lim, key.t,
                                     prismatic);
                    ch.scalar = value;
                    break;
                }
                case JointType::Fixed:
                    return fail(QStringLiteral("pose3d: joint \"%1\" is "
                                               "fixed and takes no value "
                                               "(t=%2)")
                                    .arg(it.key())
                                    .arg(key.t));
                case JointType::Free:
                    return fail(QStringLiteral(
                                    "pose3d: joint \"%1\" is the free root — "
                                    "drive it with root_pos/root_rot (t=%2)")
                                    .arg(it.key())
                                    .arg(key.t));
                }
            }
        } else if (!jointsVal.isUndefined() && !jointsVal.isNull()) {
            return fail(QStringLiteral("pose3d: \"joints\" must be an object "
                                       "(t=%1)")
                            .arg(key.t));
        }

        clip.keys.push_back(key);
        cur = key;
    }

    if (clip.duration() > kMaxClipSeconds)
        return fail(QStringLiteral("pose3d: last keyframe at %1 s exceeds "
                                   "the %2 s cap")
                        .arg(clip.duration())
                        .arg(kMaxClipSeconds));

    clip.loopEnd = clip.duration();
    const QJsonValue loopVal = obj.value(QLatin1String("loop"));
    if (loopVal.isObject()) {
        const QJsonObject lo = loopVal.toObject();
        const QString mode = lo.value(QLatin1String("mode")).toString();
        if (mode == QLatin1String("pingpong"))
            clip.loop = LoopMode::PingPong;
        else if (mode == QLatin1String("cycle"))
            clip.loop = LoopMode::Cycle;
        else if (mode == QLatin1String("hold"))
            clip.loop = LoopMode::Hold;
        else
            return fail(QStringLiteral("pose3d: loop.mode must be pingpong, "
                                       "cycle or hold"));
        if ((lo.contains(QLatin1String("start"))
             && !lo.value(QLatin1String("start")).isDouble())
            || (lo.contains(QLatin1String("end"))
                && !lo.value(QLatin1String("end")).isDouble()))
            return fail(QStringLiteral("pose3d: loop.start/loop.end must "
                                       "be numbers"));
        clip.loopStart = lo.value(QLatin1String("start")).toDouble(0.0);
        clip.loopEnd =
            lo.value(QLatin1String("end")).toDouble(clip.duration());
        if (clip.loopStart < 0 || clip.loopStart > clip.loopEnd)
            return fail(QStringLiteral("pose3d: loop.start must be within "
                                       "[0, loop.end]"));
        if (clip.loopEnd > clip.duration() + 1e-9)
            return fail(QStringLiteral("pose3d: loop.end (%1 s) is beyond "
                                       "the last keyframe (%2 s)")
                            .arg(clip.loopEnd)
                            .arg(clip.duration()));
    }

    const QJsonValue breathVal = obj.value(QLatin1String("breath"));
    if (breathVal.isObject()) {
        const QJsonObject bo = breathVal.toObject();
        clip.breathPeriodS =
            bo.value(QLatin1String("period_s")).toDouble(4.0);
        if (!(clip.breathPeriodS > 0))
            return fail(QStringLiteral("pose3d: breath.period_s must be "
                                       "> 0"));
        clip.breathAmplitudeRad =
            bo.value(QLatin1String("amplitude_deg")).toDouble(0.0)
            * kDegToRad;
        for (const QJsonValue& v :
             bo.value(QLatin1String("joints")).toArray()) {
            const int idx = chain.indexOf(v.toString());
            if (idx < 0) {
                res.warnings.append(
                    QStringLiteral("breath: unknown joint \"%1\" ignored")
                        .arg(v.toString()));
                continue;
            }
            const JointType t =
                chain.joints[static_cast<size_t>(idx)].type;
            if (t != JointType::Ball && t != JointType::Revolute) {
                res.warnings.append(
                    QStringLiteral("breath: joint \"%1\" is not ball or "
                                   "revolute, ignored")
                        .arg(v.toString()));
                continue;
            }
            clip.breathJoints.push_back(idx);
        }
        clip.hasBreath = clip.breathAmplitudeRad > 0.0
                         && !clip.breathJoints.empty();
    }

    res.ok = true;
    return res;
}

QJsonObject clipToJson(const AnimClip& clip, const Chain& chain)
{
    constexpr double kMmToMeters = 1.0 / 1000.0;
    constexpr double kRadToDeg = 180.0 / M_PI;

    QJsonObject obj;
    obj.insert(QLatin1String("id"), clip.id);
    obj.insert(QLatin1String("schema_version"), QStringLiteral("1"));
    obj.insert(QLatin1String("chain"), clip.chainId);
    if (!clip.nameFr.isEmpty())
        obj.insert(QLatin1String("name_fr"), clip.nameFr);
    if (!clip.nameEn.isEmpty())
        obj.insert(QLatin1String("name_en"), clip.nameEn);
    if (!clip.source.isEmpty())
        obj.insert(QLatin1String("source"), clip.source);
    obj.insert(QLatin1String("fps"), clip.fps);

    QJsonArray keyframes;
    for (const DenseKey& key : clip.keys) {
        QJsonObject kf;
        kf.insert(QLatin1String("t"), key.t);
        QJsonArray rootPos;
        rootPos.append(key.rootPosMm.X() * kMmToMeters);
        rootPos.append(key.rootPosMm.Y() * kMmToMeters);
        rootPos.append(key.rootPosMm.Z() * kMmToMeters);
        kf.insert(QLatin1String("root_pos"), rootPos);
        {
            Standard_Real rx = 0, ry = 0, rz = 0;
            key.rootRot.GetEulerAngles(gp_Extrinsic_XYZ, rx, ry, rz);
            QJsonArray rootRot;
            rootRot.append(rx * kRadToDeg);
            rootRot.append(ry * kRadToDeg);
            rootRot.append(rz * kRadToDeg);
            kf.insert(QLatin1String("root_rot"), rootRot);
        }
        QJsonObject joints;
        for (size_t j = 0; j < chain.joints.size(); ++j) {
            const Joint& joint = chain.joints[j];
            if (joint.parent < 0 && joint.type == JointType::Free)
                continue; // driven by root_pos/root_rot
            const JointChannel& ch = key.values[j];
            switch (joint.type) {
            case JointType::Ball: {
                Standard_Real rx = 0, ry = 0, rz = 0;
                ch.rot.GetEulerAngles(gp_Extrinsic_XYZ, rx, ry, rz);
                QJsonArray a;
                a.append(rx * kRadToDeg);
                a.append(ry * kRadToDeg);
                a.append(rz * kRadToDeg);
                joints.insert(joint.name, a);
                break;
            }
            case JointType::Revolute:
                joints.insert(joint.name, ch.scalar * kRadToDeg);
                break;
            case JointType::Prismatic:
                joints.insert(joint.name, ch.scalar * kMmToMeters);
                break;
            case JointType::Fixed:
            case JointType::Free:
                break;
            }
        }
        if (!joints.isEmpty())
            kf.insert(QLatin1String("joints"), joints);
        keyframes.append(kf);
    }
    obj.insert(QLatin1String("keyframes"), keyframes);

    QJsonObject loop;
    switch (clip.loop) {
    case LoopMode::PingPong:
        loop.insert(QLatin1String("mode"), QStringLiteral("pingpong"));
        break;
    case LoopMode::Cycle:
        loop.insert(QLatin1String("mode"), QStringLiteral("cycle"));
        break;
    case LoopMode::Hold:
        loop.insert(QLatin1String("mode"), QStringLiteral("hold"));
        break;
    }
    loop.insert(QLatin1String("start"), clip.loopStart);
    loop.insert(QLatin1String("end"), clip.loopEnd);
    obj.insert(QLatin1String("loop"), loop);

    if (clip.hasBreath) {
        QJsonObject breath;
        breath.insert(QLatin1String("period_s"), clip.breathPeriodS);
        breath.insert(QLatin1String("amplitude_deg"),
                      clip.breathAmplitudeRad * kRadToDeg);
        QJsonArray joints;
        for (const int idx : clip.breathJoints)
            joints.append(chain.joints[static_cast<size_t>(idx)].name);
        breath.insert(QLatin1String("joints"), joints);
        obj.insert(QLatin1String("breath"), breath);
    }
    return obj;
}

ClipResult loadClipFile(const QString& path, const Chain& chain)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("pose3d: cannot read %1").arg(path));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull())
        return fail(QStringLiteral("pose3d: %1: invalid JSON (%2)")
                        .arg(path, perr.errorString()));
    if (!doc.isObject())
        return fail(QStringLiteral("pose3d: %1: top level is not an object")
                        .arg(path));
    return clipFromJson(doc.object(), chain);
}

} // namespace anim
} // namespace viki
