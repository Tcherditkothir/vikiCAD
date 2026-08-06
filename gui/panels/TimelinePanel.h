#pragma once

#include <memory>
#include <vector>

#include <QJsonObject>
#include <QWidget>

#include <gp_Trsf.hxx>

#include "anim/AnimClip.h"
#include "anim/Avatar.h"
#include "anim/Chain.h"
#include "occview/OcctViewWidget.h" // AnimPartDisplay

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace viki {

// Timeline dock: load a pose3d + avatar, play/scrub the animation in the
// 3D view, nudge keyframes (shift a time, adjust one joint's channels) and
// re-export the pose3d. STRICTLY the same core as `vikicad-cli anim
// render` — the panel only samples the clip and runs the forward
// kinematics; no animation math lives in the GUI.
//
// Keyframe markers ride on the scrub slider as a thin strip above it;
// clicking a marker selects that keyframe in the editor row.
class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(QWidget* parent = nullptr);

    // Load a pose3d + avatar (chain resolved exactly like the CLI: next to
    // the pose, then ../chains/<id>.json, unless `chainPath` says
    // otherwise). Emits animationReady + the first pose on success.
    bool loadFiles(const QString& posePath, const QString& avatarPath,
                   const QString& chainPath = QString());
    void clearAnimation();
    bool isLoaded() const { return m_loaded; }

    void setPlaying(bool on);
    bool isPlaying() const;
    void seekFrame(int frame);
    int frameCount() const { return static_cast<int>(m_frameTimes.size()); }
    int currentFrame() const { return m_frame; }

    // State for the `anim status` IPC verb (and gui-smoke).
    QJsonObject statusJson() const;
    // Warnings from the last loadFiles (chain/avatar/clip parses + the
    // superset-chain compat notice): surfaced in the `anim load` IPC reply
    // so headless callers see them, not only the command-bar history.
    QStringList lastLoadWarnings() const { return m_lastLoadWarnings; }

signals:
    // A new chain+avatar is ready: the host switches to 3D and hands the
    // parts to the view.
    void animationReady(const std::vector<viki::AnimPartDisplay>& parts);
    // A frame was sampled: joint world transforms, chain order.
    void posed(const std::vector<gp_Trsf>& world);
    void animationCleared();
    // One-line user feedback (load errors, refused edits) — routed to the
    // command bar history by the host, never a message box.
    void feedback(const QString& message);

private:
    // The keyframe-marker strip (defined in the .cpp).
    friend class KeyframeStrip;

    void buildUi();
    void updateEnabled();
    void applyFrame(int frame);
    void rebuildFrames();     // frame times + slider range + time label
    void rebuildAvatarCombo();
    void rebuildKeyEditors(); // keyframe combo + joint combo
    void syncKeyEditors();    // spinboxes <- selected keyframe/joint
    void applyKeyTime();
    void applyJointChannels();
    void exportPose();
    bool reloadAvatar(const QString& avatarPath);
    std::vector<AnimPartDisplay> buildParts() const;

    anim::Chain m_chain;
    anim::AnimClip m_clip;
    anim::AvatarSpec m_avatar;
    std::unique_ptr<anim::AvatarProvider> m_provider;
    bool m_loaded = false;
    QStringList m_lastLoadWarnings;
    QString m_posePath, m_avatarPath, m_chainPath;

    std::vector<double> m_frameTimes;
    int m_frame = 0;
    bool m_refreshing = false;

    QTimer* m_timer = nullptr;
    QPushButton* m_loadBtn = nullptr;
    QComboBox* m_avatarCombo = nullptr;
    QPushButton* m_playBtn = nullptr;
    QLabel* m_timeLabel = nullptr;
    QSlider* m_slider = nullptr;
    QWidget* m_strip = nullptr; // KeyframeStrip
    QComboBox* m_keyCombo = nullptr;
    QDoubleSpinBox* m_keyTime = nullptr;
    QComboBox* m_jointCombo = nullptr;
    QDoubleSpinBox* m_chan[3] = {nullptr, nullptr, nullptr};
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_exportBtn = nullptr;
};

} // namespace viki
