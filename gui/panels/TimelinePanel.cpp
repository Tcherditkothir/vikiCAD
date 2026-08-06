#include "panels/TimelinePanel.h"

#include <algorithm>

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include "anim/ForwardKinematics.h"

namespace viki {

namespace {
constexpr double kRadToDeg = 180.0 / M_PI;
constexpr double kDegToRad = M_PI / 180.0;
} // namespace

// Thin strip above the scrub slider: one triangle per keyframe, at the
// keyframe's position on the time axis. Clicking a marker selects that
// keyframe in the editor row below.
class KeyframeStrip : public QWidget {
public:
    explicit KeyframeStrip(TimelinePanel* panel)
        : QWidget(panel), m_panel(panel)
    {
        setFixedHeight(12);
        setToolTip(QStringLiteral("Keyframes — click a marker to edit it"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        if (!m_panel->m_loaded || m_panel->m_clip.keys.empty())
            return;
        const double duration = m_panel->m_clip.duration();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().highlight());
        for (size_t k = 0; k < m_panel->m_clip.keys.size(); ++k) {
            const int x = xForTime(m_panel->m_clip.keys[k].t, duration);
            QPolygon tri;
            tri << QPoint(x - 4, 1) << QPoint(x + 4, 1) << QPoint(x, 10);
            p.drawPolygon(tri);
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (!m_panel->m_loaded || m_panel->m_clip.keys.empty())
            return;
        const double duration = m_panel->m_clip.duration();
        int best = 0;
        int bestDist = 1 << 30;
        for (size_t k = 0; k < m_panel->m_clip.keys.size(); ++k) {
            const int x = xForTime(m_panel->m_clip.keys[k].t, duration);
            const int d = std::abs(x - event->pos().x());
            if (d < bestDist) {
                bestDist = d;
                best = static_cast<int>(k);
            }
        }
        m_panel->m_keyCombo->setCurrentIndex(best);
    }

private:
    int xForTime(double t, double duration) const
    {
        const int span = std::max(1, width() - 12);
        const double u = duration > 0 ? t / duration : 0.0;
        return 6 + static_cast<int>(u * span);
    }
    TimelinePanel* m_panel;
};

TimelinePanel::TimelinePanel(QWidget* parent) : QWidget(parent)
{
    buildUi();
    updateEnabled();
}

void TimelinePanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(3);

    auto* topRow = new QHBoxLayout;
    m_loadBtn = new QPushButton(QStringLiteral("Load pose…"), this);
    topRow->addWidget(m_loadBtn);
    topRow->addWidget(new QLabel(QStringLiteral("Avatar:"), this));
    m_avatarCombo = new QComboBox(this);
    m_avatarCombo->setMinimumWidth(150);
    topRow->addWidget(m_avatarCombo);
    m_playBtn = new QPushButton(QStringLiteral("Play"), this);
    m_playBtn->setCheckable(true);
    topRow->addWidget(m_playBtn);
    m_timeLabel = new QLabel(QStringLiteral("—"), this);
    topRow->addWidget(m_timeLabel);
    topRow->addStretch(1);
    m_exportBtn = new QPushButton(QStringLiteral("Export pose3d…"), this);
    topRow->addWidget(m_exportBtn);
    layout->addLayout(topRow);

    m_strip = new KeyframeStrip(this);
    layout->addWidget(m_strip);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 0);
    layout->addWidget(m_slider);

    auto* editRow = new QHBoxLayout;
    editRow->addWidget(new QLabel(QStringLiteral("Keyframe:"), this));
    m_keyCombo = new QComboBox(this);
    editRow->addWidget(m_keyCombo);
    editRow->addWidget(new QLabel(QStringLiteral("t (s):"), this));
    m_keyTime = new QDoubleSpinBox(this);
    m_keyTime->setDecimals(3);
    m_keyTime->setRange(0.0, 300.0);
    m_keyTime->setSingleStep(0.05);
    editRow->addWidget(m_keyTime);
    editRow->addWidget(new QLabel(QStringLiteral("Joint:"), this));
    m_jointCombo = new QComboBox(this);
    editRow->addWidget(m_jointCombo);
    for (int c = 0; c < 3; ++c) {
        m_chan[c] = new QDoubleSpinBox(this);
        m_chan[c]->setDecimals(1);
        m_chan[c]->setRange(-10000.0, 10000.0);
        editRow->addWidget(m_chan[c]);
    }
    m_applyBtn = new QPushButton(QStringLiteral("Apply"), this);
    editRow->addWidget(m_applyBtn);
    editRow->addStretch(1);
    layout->addLayout(editRow);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (m_frameTimes.empty())
            return;
        applyFrame((m_frame + 1) % static_cast<int>(m_frameTimes.size()));
    });

    connect(m_loadBtn, &QPushButton::clicked, this, [this] {
        const QString pose = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load pose3d"),
            m_posePath.isEmpty() ? QString()
                                 : QFileInfo(m_posePath).path(),
            QStringLiteral("pose3d (*.json)"));
        if (pose.isEmpty())
            return;
        QString avatar = m_avatarPath;
        if (avatar.isEmpty()) {
            const QDir poseDir = QFileInfo(pose).dir();
            const QString hint =
                poseDir.filePath(QStringLiteral("../avatars"));
            avatar = QFileDialog::getOpenFileName(
                this, QStringLiteral("Load avatar"),
                QFileInfo::exists(hint) ? hint : poseDir.path(),
                QStringLiteral("avatar (*.json)"));
            if (avatar.isEmpty())
                return;
        }
        loadFiles(pose, avatar);
    });
    connect(m_playBtn, &QPushButton::toggled, this,
            [this](bool on) { setPlaying(on); });
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if (!m_refreshing)
            applyFrame(value);
    });
    connect(m_keyCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { syncKeyEditors(); });
    connect(m_jointCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { syncKeyEditors(); });
    connect(m_keyTime, &QDoubleSpinBox::editingFinished, this,
            [this] { applyKeyTime(); });
    connect(m_applyBtn, &QPushButton::clicked, this,
            [this] { applyJointChannels(); });
    connect(m_avatarCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                if (m_refreshing || idx < 0)
                    return;
                const QString path = m_avatarCombo->itemData(idx).toString();
                if (!path.isEmpty() && path != m_avatarPath)
                    reloadAvatar(path);
            });
    connect(m_exportBtn, &QPushButton::clicked, this,
            [this] { exportPose(); });
}

void TimelinePanel::updateEnabled()
{
    const bool on = m_loaded;
    m_playBtn->setEnabled(on);
    m_slider->setEnabled(on);
    m_keyCombo->setEnabled(on);
    m_keyTime->setEnabled(on);
    m_jointCombo->setEnabled(on);
    for (auto* c : m_chan)
        c->setEnabled(on);
    m_applyBtn->setEnabled(on);
    m_exportBtn->setEnabled(on);
    m_avatarCombo->setEnabled(on);
}

bool TimelinePanel::loadFiles(const QString& posePath,
                              const QString& avatarPath,
                              const QString& chainPath)
{
    const anim::AvatarResult avatar = anim::loadAvatarFile(avatarPath);
    if (!avatar.ok) {
        Q_EMIT feedback(QStringLiteral("timeline: %1").arg(avatar.error));
        return false;
    }

    QFile poseFile(posePath);
    if (!poseFile.open(QIODevice::ReadOnly)) {
        Q_EMIT feedback(QStringLiteral("timeline: cannot read %1")
                            .arg(posePath));
        return false;
    }
    const QJsonDocument poseDoc =
        QJsonDocument::fromJson(poseFile.readAll());
    if (!poseDoc.isObject()) {
        Q_EMIT feedback(QStringLiteral("timeline: %1 is not valid JSON")
                            .arg(posePath));
        return false;
    }
    const QString chainId =
        poseDoc.object().value(QLatin1String("chain")).toString();
    QString resolvedChain = chainPath;
    if (resolvedChain.isEmpty()) {
        const QDir poseDir = QFileInfo(posePath).dir();
        const QStringList candidates = {
            poseDir.filePath(chainId + QStringLiteral(".json")),
            poseDir.filePath(QStringLiteral("../chains/") + chainId
                             + QStringLiteral(".json")),
        };
        for (const QString& c : candidates)
            if (QFileInfo::exists(c)) {
                resolvedChain = c;
                break;
            }
        if (resolvedChain.isEmpty()) {
            Q_EMIT feedback(
                QStringLiteral("timeline: chain \"%1\" not found near %2")
                    .arg(chainId, posePath));
            return false;
        }
    }
    const anim::ChainResult chain =
        anim::loadChainFile(resolvedChain, avatar.spec.heightM);
    if (!chain.ok) {
        Q_EMIT feedback(QStringLiteral("timeline: %1").arg(chain.error));
        return false;
    }
    if (avatar.spec.chainId != chain.chain.id) {
        Q_EMIT feedback(
            QStringLiteral("timeline: avatar \"%1\" dresses chain \"%2\", "
                           "not \"%3\"")
                .arg(avatar.spec.id, avatar.spec.chainId, chain.chain.id));
        return false;
    }
    const anim::ClipResult clip =
        anim::clipFromJson(poseDoc.object(), chain.chain);
    if (!clip.ok) {
        Q_EMIT feedback(QStringLiteral("timeline: %1").arg(clip.error));
        return false;
    }
    anim::ProviderResult provider =
        anim::makeAvatarProvider(avatar.spec);
    if (!provider.ok) {
        Q_EMIT feedback(QStringLiteral("timeline: %1")
                            .arg(provider.error));
        return false;
    }

    setPlaying(false);
    m_chain = chain.chain;
    m_clip = clip.clip;
    m_avatar = avatar.spec;
    m_provider = std::move(provider.provider);
    m_posePath = posePath;
    m_avatarPath = avatarPath;
    m_chainPath = resolvedChain;
    m_loaded = true;
    for (const QString& w : chain.warnings + avatar.warnings + clip.warnings
             + anim::avatarChainWarnings(avatar.spec, chain.chain))
        Q_EMIT feedback(QStringLiteral("timeline: %1").arg(w));

    rebuildAvatarCombo();
    rebuildKeyEditors();
    updateEnabled();
    Q_EMIT animationReady(buildParts());
    rebuildFrames();
    applyFrame(0);
    Q_EMIT feedback(QStringLiteral("timeline: %1 loaded — %2 frames at "
                                   "%3 fps")
                        .arg(m_clip.id)
                        .arg(frameCount())
                        .arg(m_clip.fps));
    return true;
}

void TimelinePanel::clearAnimation()
{
    setPlaying(false);
    m_loaded = false;
    m_provider.reset();
    m_frameTimes.clear();
    m_frame = 0;
    m_timeLabel->setText(QStringLiteral("—"));
    updateEnabled();
    m_strip->update();
    Q_EMIT animationCleared();
}

void TimelinePanel::setPlaying(bool on)
{
    if (!m_loaded || m_frameTimes.empty())
        on = false;
    if (on)
        m_timer->start(std::max(1, 1000 / std::max(1, m_clip.fps)));
    else
        m_timer->stop();
    m_refreshing = true;
    m_playBtn->setChecked(on);
    m_playBtn->setText(on ? QStringLiteral("Pause")
                          : QStringLiteral("Play"));
    m_refreshing = false;
}

bool TimelinePanel::isPlaying() const
{
    return m_timer->isActive();
}

void TimelinePanel::seekFrame(int frame)
{
    if (!m_loaded || m_frameTimes.empty())
        return;
    applyFrame(std::clamp(frame, 0, frameCount() - 1));
}

void TimelinePanel::applyFrame(int frame)
{
    if (!m_loaded || m_frameTimes.empty())
        return;
    m_frame = std::clamp(frame, 0, frameCount() - 1);
    const double t = m_frameTimes[static_cast<size_t>(m_frame)];
    const anim::PoseSample pose = m_clip.sampleAt(t);
    Q_EMIT posed(anim::worldTransforms(m_chain, pose));
    m_refreshing = true;
    m_slider->setValue(m_frame);
    m_refreshing = false;
    m_timeLabel->setText(QStringLiteral("%1 / %2 s")
                             .arg(t, 0, 'f', 2)
                             .arg(m_clip.duration(), 0, 'f', 2));
}

void TimelinePanel::rebuildFrames()
{
    m_frameTimes = m_clip.frameTimes();
    m_refreshing = true;
    m_slider->setRange(0, std::max(0, frameCount() - 1));
    m_refreshing = false;
    m_strip->update();
}

void TimelinePanel::rebuildAvatarCombo()
{
    m_refreshing = true;
    m_avatarCombo->clear();
    const QDir dir = QFileInfo(m_avatarPath).dir();
    const QStringList files = dir.entryList(
        QStringList() << QStringLiteral("*.json"), QDir::Files,
        QDir::Name);
    for (const QString& f : files) {
        m_avatarCombo->addItem(QFileInfo(f).completeBaseName(),
                               dir.filePath(f));
        if (dir.filePath(f) == m_avatarPath)
            m_avatarCombo->setCurrentIndex(m_avatarCombo->count() - 1);
    }
    m_refreshing = false;
}

void TimelinePanel::rebuildKeyEditors()
{
    m_refreshing = true;
    m_keyCombo->clear();
    for (size_t k = 0; k < m_clip.keys.size(); ++k)
        m_keyCombo->addItem(QStringLiteral("#%1 @ %2 s")
                                .arg(k)
                                .arg(m_clip.keys[k].t, 0, 'f', 2));
    m_jointCombo->clear();
    for (size_t j = 0; j < m_chain.joints.size(); ++j) {
        const anim::Joint& joint = m_chain.joints[j];
        const bool editable =
            joint.type == anim::JointType::Ball
            || joint.type == anim::JointType::Revolute
            || joint.type == anim::JointType::Prismatic;
        if (editable)
            m_jointCombo->addItem(joint.name, static_cast<int>(j));
    }
    m_refreshing = false;
    syncKeyEditors();
}

void TimelinePanel::syncKeyEditors()
{
    if (m_refreshing || !m_loaded)
        return;
    const int k = m_keyCombo->currentIndex();
    if (k < 0 || static_cast<size_t>(k) >= m_clip.keys.size())
        return;
    m_refreshing = true;
    m_keyTime->setValue(m_clip.keys[static_cast<size_t>(k)].t);
    const int jointIdx = m_jointCombo->currentData().toInt();
    if (jointIdx >= 0
        && static_cast<size_t>(jointIdx) < m_chain.joints.size()) {
        const anim::Joint& joint =
            m_chain.joints[static_cast<size_t>(jointIdx)];
        const anim::JointChannel& ch =
            m_clip.keys[static_cast<size_t>(k)]
                .values[static_cast<size_t>(jointIdx)];
        if (joint.type == anim::JointType::Ball) {
            Standard_Real rx = 0, ry = 0, rz = 0;
            ch.rot.GetEulerAngles(gp_Extrinsic_XYZ, rx, ry, rz);
            const double deg[3] = {rx * kRadToDeg, ry * kRadToDeg,
                                   rz * kRadToDeg};
            for (int c = 0; c < 3; ++c) {
                m_chan[c]->setEnabled(true);
                m_chan[c]->setValue(deg[c]);
                m_chan[c]->setSuffix(QStringLiteral("°"));
            }
        } else {
            const bool prismatic = joint.type == anim::JointType::Prismatic;
            m_chan[0]->setEnabled(true);
            m_chan[0]->setValue(prismatic ? ch.scalar
                                          : ch.scalar * kRadToDeg);
            m_chan[0]->setSuffix(prismatic ? QStringLiteral(" mm")
                                           : QStringLiteral("°"));
            for (int c = 1; c < 3; ++c) {
                m_chan[c]->setEnabled(false);
                m_chan[c]->setValue(0);
                m_chan[c]->setSuffix(QString());
            }
        }
    }
    m_refreshing = false;
}

void TimelinePanel::applyKeyTime()
{
    if (m_refreshing || !m_loaded)
        return;
    const int k = m_keyCombo->currentIndex();
    if (k < 0 || static_cast<size_t>(k) >= m_clip.keys.size())
        return;
    const double gap = 1.0 / std::max(1, m_clip.fps);
    double t = m_keyTime->value();
    const double oldDuration = m_clip.duration();
    if (k > 0)
        t = std::max(t, m_clip.keys[static_cast<size_t>(k - 1)].t + gap);
    if (static_cast<size_t>(k) + 1 < m_clip.keys.size())
        t = std::min(t, m_clip.keys[static_cast<size_t>(k + 1)].t - gap);
    t = std::max(0.0, t);
    m_clip.keys[static_cast<size_t>(k)].t = t;
    // A loop window that tracked the whole clip keeps tracking it.
    if (m_clip.loopEnd == oldDuration)
        m_clip.loopEnd = m_clip.duration();
    m_clip.loopEnd = std::min(m_clip.loopEnd, m_clip.duration());
    m_clip.loopStart = std::min(m_clip.loopStart, m_clip.loopEnd);
    rebuildFrames();
    rebuildKeyEditors();
    m_refreshing = true;
    m_keyCombo->setCurrentIndex(k);
    m_refreshing = false;
    syncKeyEditors();
    applyFrame(std::min(m_frame, frameCount() - 1));
    Q_EMIT feedback(QStringLiteral("timeline: keyframe #%1 moved to %2 s")
                        .arg(k)
                        .arg(t, 0, 'f', 3));
}

void TimelinePanel::applyJointChannels()
{
    if (!m_loaded)
        return;
    const int k = m_keyCombo->currentIndex();
    const int jointIdx = m_jointCombo->currentData().toInt();
    if (k < 0 || static_cast<size_t>(k) >= m_clip.keys.size()
        || jointIdx < 0
        || static_cast<size_t>(jointIdx) >= m_chain.joints.size())
        return;
    const anim::Joint& joint = m_chain.joints[static_cast<size_t>(jointIdx)];
    anim::JointChannel& ch = m_clip.keys[static_cast<size_t>(k)]
                                 .values[static_cast<size_t>(jointIdx)];
    if (joint.type == anim::JointType::Ball) {
        gp_Quaternion q;
        q.SetEulerAngles(gp_Extrinsic_XYZ,
                         m_chan[0]->value() * kDegToRad,
                         m_chan[1]->value() * kDegToRad,
                         m_chan[2]->value() * kDegToRad);
        ch.rot = q;
    } else if (joint.type == anim::JointType::Revolute) {
        ch.scalar = m_chan[0]->value() * kDegToRad;
    } else if (joint.type == anim::JointType::Prismatic) {
        ch.scalar = m_chan[0]->value(); // already mm
    }
    applyFrame(m_frame);
    Q_EMIT feedback(QStringLiteral("timeline: %1 adjusted at keyframe #%2")
                        .arg(joint.name)
                        .arg(k));
}

void TimelinePanel::exportPose()
{
    if (!m_loaded)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export pose3d"), m_posePath,
        QStringLiteral("pose3d (*.json)"));
    if (path.isEmpty())
        return;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        Q_EMIT feedback(QStringLiteral("timeline: cannot write %1")
                            .arg(path));
        return;
    }
    out.write(QJsonDocument(anim::clipToJson(m_clip, m_chain))
                  .toJson(QJsonDocument::Indented));
    Q_EMIT feedback(QStringLiteral("timeline: pose3d exported to %1")
                        .arg(path));
}

bool TimelinePanel::reloadAvatar(const QString& avatarPath)
{
    // The avatar's height_m rescales the chain, so the whole scene
    // reloads from the same pose file with the new dressing.
    return loadFiles(m_posePath, avatarPath, m_chainPath);
}

std::vector<AnimPartDisplay> TimelinePanel::buildParts() const
{
    std::vector<AnimPartDisplay> parts;
    if (!m_provider)
        return parts;
    for (int j = 0; j < static_cast<int>(m_chain.joints.size()); ++j) {
        for (const anim::AvatarPart& part :
             m_provider->partsForJoint(m_chain, j)) {
            if (part.shape.IsNull())
                continue;
            AnimPartDisplay d;
            d.shape = part.shape;
            d.joint = j;
            d.rgb = part.accent ? m_avatar.accentColor
                                : m_avatar.baseColor;
            parts.push_back(d);
        }
    }
    return parts;
}

QJsonObject TimelinePanel::statusJson() const
{
    QJsonObject status;
    status.insert(QLatin1String("loaded"), m_loaded);
    if (m_loaded) {
        status.insert(QLatin1String("pose"), m_clip.id);
        status.insert(QLatin1String("avatar"), m_avatar.id);
        status.insert(QLatin1String("chain"), m_chain.id);
        status.insert(QLatin1String("fps"), m_clip.fps);
        status.insert(QLatin1String("frames"), frameCount());
        status.insert(QLatin1String("frame"), m_frame);
        status.insert(QLatin1String("playing"), isPlaying());
        status.insert(QLatin1String("keyframes"),
                      static_cast<int>(m_clip.keys.size()));
        status.insert(QLatin1String("durationS"), m_clip.duration());
    }
    return status;
}

} // namespace viki
