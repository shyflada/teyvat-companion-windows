#pragma once
#include "teyvat/navigation.hpp"
#include "teyvat/store.hpp"
#include <QSettings>
#include <QSet>
#include <memory>

namespace teyvat {
enum class TrackingState { Paused, PositionUnknown, NoTarget, NearTarget, Guiding, Review, ProfileChanged };
struct TrackingView {
    TrackingState state = TrackingState::Paused;
    std::optional<Guidance> guidance;
    QString target_label;
    int automatic_changes = 0;
    std::optional<DetectionReason> review_reason;
    LocalizationReason position_reason = LocalizationReason::WeakMatch;
    std::optional<QPointF> last_position; // Preview only; never used as a live pose.
    bool near_target = false;
    int confirmed_changes = 0;
    bool limited_position = false;
};
class TrackingSession final {
public:
    TrackingSession(Store& store, TrackingSetup setup);
    [[nodiscard]] const TrackingSetup& setup() const { return setup_; }
    [[nodiscard]] const TrackingView& view() const { return view_; }
    [[nodiscard]] const QString& settings_error() const { return settings_error_; }
    [[nodiscard]] std::optional<MarkerKey> target() const { return selector_.target(); }
    void observe(const LocalizationResult& estimate, const QImage& opening_crop, qint64 time_ms);
    void observe_scene(const QImage& scene, qint64 time_ms);
    // A world-map acquisition is preview evidence until gameplay is matched.
    // Reconsider a locked target after a substantial relocation, preserving progress.
    void reanchor(QPointF position, bool preserve_target = false);
    void pause();
    void skip();
    [[nodiscard]] std::optional<MarkerKey> confirm_target();
    void set_near_distance(double distance);
    [[nodiscard]] double near_distance() const { return near_distance_; }
    void set_remote_collected(const QSet<QString>& ids);
    void set_allowed_targets(std::optional<QSet<QString>> ids);
    void undo();
private:
    void refresh();
    void save_target();
    void reset_opening();
    void apply_opening(OpeningEvidence event, const MarkerKey& expected);
    Store& store_;
    TrackingSetup setup_;
    QString profile_, prefix_, settings_error_;
    QSettings options_;
    TargetSelector selector_;
    std::unique_ptr<OpeningTracker> detector_;
    ChestAnimationTracker animation_;
    std::optional<OpeningEvidence> pending_animation_;
    std::optional<MarkerKey> scene_target_;
    std::optional<MarkerKey> armed_target_;
    std::optional<NavigationPose> pose_;
    QVector<LocalizationSample> recent_;
    TrackingView view_;
    QByteArray run_id_;
    qint64 last_time_ = -1, bucket_ = -1, last_scene_time_ = -1;
    bool writable_ = true;
    double near_distance_ = 20;
    QSet<QString> remote_collected_;
    std::optional<QSet<QString>> allowed_targets_;
};
} // namespace teyvat
