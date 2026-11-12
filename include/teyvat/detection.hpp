#pragma once

#include "teyvat/map_data.hpp"
#include "teyvat/localization.hpp"
#include <QStringList>
#include <optional>

namespace teyvat {

// Offline research only. These are similarity scores, not measured probabilities.
struct OpeningAnimationProof {
    qint64 burst_ms = 0, dissolved_ms = 0;
    double ring_coverage = 0, gold_fraction = 0, blue_rise = 0;
    int interaction_frames = 0, dissolve_frames = 0;
};
struct OpeningEvidence {
    QByteArray event_id;
    qint64 closed_ms = 0, opened_ms = 0;
    double closed_similarity = 0, opened_similarity = 0;
    std::optional<RecordedPosition> position;
    bool position_stable = false;
    std::optional<OpeningAnimationProof> animation = std::nullopt;
};

struct OpeningCues { bool interaction = false; double gold = 0, blue = 0, ring = 0; };
[[nodiscard]] OpeningCues opening_scene_cues(const QImage& scene, bool measure_ring = false);
class ChestAnimationTracker final {
public:
    [[nodiscard]] std::optional<OpeningEvidence> push(const QImage& scene, qint64 time_ms,
        const std::optional<RecordedPosition>& position);
    void reset();
private:
    QVector<double> gold_history_, blue_history_;
    qint64 previous_ = -1, interaction_at_ = -1, burst_at_ = -1;
    int interaction_count_ = 0, dissolve_count_ = 0;
    double blue_baseline_ = 0, blue_rise_ = 1, ring_ = 0, gold_ = 0;
    std::optional<RecordedPosition> armed_position_;
    bool stable_ = true, latched_ = false;
};

struct RecordingAnalysis {
    QByteArray fingerprint;
    LayerKey map;
    QString frame, provenance, license;
    double radius = 0;
    int frame_count = 0;
    QVector<OpeningEvidence> openings;
    QVector<LocalizationSample> localizations;
    std::optional<QRectF> reference_bounds;
};

enum class DetectionOutcome { Applied, Review, Unmatched, AlreadyCollected, Duplicate };
enum class DetectionReason { UniqueMatch, AmbiguousMarkers, UncertainPosition, NoPosition,
    NoMarker, ArchivedMarker, UnknownCategory, AlreadyCollected, PreviouslyProcessed, TargetMismatch };

struct DetectionResult {
    qint64 time_ms = 0;
    DetectionOutcome outcome = DetectionOutcome::Unmatched;
    DetectionReason reason = DetectionReason::NoMarker;
    QStringList candidates;
};

struct DetectionRun {
    RecordingAnalysis analysis;
    QVector<DetectionResult> results;
    QString error;
    bool cancelled = false;
};

// Shared temporal detector for saved frames and explicitly started window sampling.
class OpeningTracker final {
public:
    OpeningTracker(QImage closed, QImage opened, double radius);
    [[nodiscard]] std::optional<OpeningEvidence> push(const QImage& crop, qint64 time_ms,
        const std::optional<RecordedPosition>& position);
    void reset();
    [[nodiscard]] bool armed() const { return armed_at_ >= 0 && !latched_; }
private:
    QImage closed_, opened_;
    double radius_ = 0, closed_score_ = 0, open_score_ = 0;
    int closed_count_ = 0, open_count_ = 0;
    qint64 previous_ = -1, armed_at_ = -1;
    bool latched_ = false, positions_stable_ = true;
    std::optional<RecordedPosition> armed_position_;
};

struct TrackingSetup {
    Dataset dataset;
    RecordingAnalysis analysis;
    LocalizationConfig localization;
    QSize capture_size;
    QRect opening_roi;
    QImage closed, opened;
    bool synthetic = false;
};
[[nodiscard]] TrackingSetup load_tracking_setup(const QString& manifest_path);

// Reads a bounded local JSON/PNG bundle. No capture, network, or database writes.
[[nodiscard]] RecordingAnalysis analyze_recording(const QString& manifest_path, std::stop_token stop = {});
[[nodiscard]] DetectionResult match_opening(const OpeningEvidence& evidence,
    const RecordingAnalysis& analysis, const MapLayer& layer, const QVector<MapMarker>& markers);
void validate_analysis(const RecordingAnalysis& analysis);

} // namespace teyvat
