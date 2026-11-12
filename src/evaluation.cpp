#include "teyvat/evaluation.hpp"
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace teyvat {
namespace {
[[noreturn]] void invalid() { throw std::runtime_error("Invalid evaluation labels: provide each frame exactly once, bounded coordinates/tolerances, and ordered opening labels with known marker IDs."); }
double number(const QJsonObject& object, const char* key, double low, double high) {
    const auto value = object.value(QLatin1String(key));
    const double n = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(n) || n < low || n > high) { invalid(); } return n;
}
qint64 time(const QJsonObject& object) {
    const double n = number(object, "time_ms", 0, 600000); if (n != std::floor(n)) { invalid(); } return static_cast<qint64>(n);
}
QJsonValue ratio(double numerator, double denominator) { return denominator > 0 ? QJsonValue(numerator / denominator) : QJsonValue(QJsonValue::Null); }
}

QJsonObject evaluate_recording(const RecordingAnalysis& analysis, const Dataset& dataset, const QByteArray& labels) {
    validate_analysis(analysis); validate_dataset(dataset);
    if (labels.isEmpty() || labels.size() > 1024 * 1024 || analysis.localizations.size() != analysis.frame_count) { invalid(); }
    const MapLayer* layer = nullptr;
    for (const auto& item : dataset.maps) { if (item.key == analysis.map && item.frame == analysis.frame) { layer = &item; break; } }
    if (!layer) { invalid(); }
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(labels, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) { invalid(); }
    const auto root = document.object();
    if (root.value(QStringLiteral("format")) != QStringLiteral("teyvat-evaluation-labels") || root.value(QStringLiteral("version")) != 1
        || !root.value(QStringLiteral("positions")).isArray() || !root.value(QStringLiteral("openings")).isArray()) { invalid(); }
    const double tolerance = number(root, "position_tolerance", 0.001, 100);
    const double event_tolerance = number(root, "event_tolerance_ms", 0, 1000);
    const auto positions = root.value(QStringLiteral("positions")).toArray(), openings = root.value(QStringLiteral("openings")).toArray();
    if (positions.size() != analysis.frame_count || openings.size() > 150) { invalid(); }
    QHash<qint64, LocalizationResult> estimates;
    for (const auto& item : analysis.localizations) { estimates.insert(item.time_ms, item.estimate); }
    QSet<qint64> seen; QJsonArray frame_results;
    int known = 0, accepted = 0, within = 0, missed = 0, unexpected = 0;
    double total_error = 0, square_error = 0, max_error = 0;
    for (const auto& value : positions) {
        if (!value.isObject()) { invalid(); } const auto label = value.toObject(); const auto t = time(label);
        if (!estimates.contains(t) || seen.contains(t) || !label.contains(QStringLiteral("position"))) { invalid(); } seen.insert(t);
        const auto estimate = estimates.value(t); const auto truth = label.value(QStringLiteral("position"));
        QJsonObject row{{QStringLiteral("time_ms"), t}, {QStringLiteral("similarity"), estimate.similarity}, {QStringLiteral("runner_up"), estimate.runner_up}};
        if (truth.isNull()) {
            if (estimate.position) { ++unexpected; }
            row[QStringLiteral("error")] = QJsonValue::Null;
        } else {
            if (!truth.isObject()) { invalid(); } ++known;
            const auto point = truth.toObject(); const double x = number(point, "x", 0, layer->width), y = number(point, "y", 0, layer->height);
            if (estimate.position) {
                ++accepted; const double error = std::hypot(x - estimate.position->x, y - estimate.position->y);
                total_error += error; square_error += error * error; max_error = std::max(max_error, error); within += error <= tolerance;
                row[QStringLiteral("error")] = error;
            } else { ++missed; row[QStringLiteral("error")] = QJsonValue::Null; }
        }
        row[QStringLiteral("position")] = estimate.position ? QJsonValue(QJsonObject{{QStringLiteral("x"), estimate.position->x}, {QStringLiteral("y"), estimate.position->y}}) : QJsonValue(QJsonValue::Null);
        frame_results.append(row);
    }
    struct Label { qint64 time; QString marker; }; QVector<Label> expected;
    qint64 previous = -1; QSet<QString> labeled_ids;
    for (const auto& value : openings) {
        if (!value.isObject()) { invalid(); } const auto entry = value.toObject(); const auto t = time(entry);
        const auto id = entry.value(QStringLiteral("marker_id")).toString(); bool exists = false;
        for (const auto& marker : dataset.markers) { if (marker.map == analysis.map && marker.id == id) { exists = true; break; } }
        const auto key = QString::number(t) + QLatin1Char(':') + id;
        if (!exists || t < previous || t > analysis.localizations.last().time_ms || labeled_ids.contains(key)) { invalid(); }
        expected.append({t, id}); labeled_ids.insert(key); previous = t;
    }
    // Ordered maximum matching, with minimum total timing error as a tie-breaker.
    // Greedy nearest-time matching can incorrectly steal another event's label.
    struct Cell { int matches = 0; qint64 error = 0; char action = 0; };
    const int rows = static_cast<int>(analysis.openings.size()) + 1, cols = static_cast<int>(expected.size()) + 1;
    QVector<Cell> dp(rows * cols);
    const auto better = [](const Cell& a, const Cell& b) { return a.matches > b.matches || (a.matches == b.matches && a.error < b.error); };
    for (int i = 1; i < rows; ++i) { dp[i * cols].action = 'p'; }
    for (int j = 1; j < cols; ++j) { dp[j].action = 'l'; }
    for (int i = 1; i < rows; ++i) {
        for (int j = 1; j < cols; ++j) {
            auto best = dp[(i - 1) * cols + j]; best.action = 'p';
            auto skip_label = dp[i * cols + j - 1]; skip_label.action = 'l'; if (better(skip_label, best)) { best = skip_label; }
            const auto delta = std::abs(analysis.openings[i - 1].opened_ms - expected[j - 1].time);
            if (delta <= event_tolerance) { auto match = dp[(i - 1) * cols + j - 1]; ++match.matches; match.error += delta; match.action = 'm'; if (better(match, best)) { best = match; } }
            dp[i * cols + j] = best;
        }
    }
    int correct_marker = 0, wrong_marker = 0, unresolved = 0, unmatched_automatic = 0; QSet<int> matched_events;
    for (int i = rows - 1, j = cols - 1; i > 0 || j > 0;) {
        const auto action = dp[i * cols + j].action;
        if (action == 'm') {
            matched_events.insert(i - 1);
            const auto result = match_opening(analysis.openings[i - 1], analysis, *layer, dataset.markers);
            if (result.outcome == DetectionOutcome::Applied || result.outcome == DetectionOutcome::AlreadyCollected) {
                if (result.candidates.first() == expected[j - 1].marker) { ++correct_marker; } else { ++wrong_marker; }
            } else { ++unresolved; }
            --i; --j;
        } else if (action == 'p') { --i; } else if (action == 'l') { --j; } else { invalid(); }
    }
    for (int i = 0; i < analysis.openings.size(); ++i) {
        if (!matched_events.contains(i)) { const auto result = match_opening(analysis.openings[i], analysis, *layer, dataset.markers);
            unmatched_automatic += result.outcome == DetectionOutcome::Applied || result.outcome == DetectionOutcome::AlreadyCollected; }
    }
    const int tp = dp.last().matches, fp = rows - 1 - tp, fn = cols - 1 - tp;
    return {{QStringLiteral("format"), QStringLiteral("teyvat-evaluation-report")}, {QStringLiteral("version"), 1},
        {QStringLiteral("recording_hash"), QString::fromLatin1(analysis.fingerprint.toHex())},
        {QStringLiteral("scope"), QStringLiteral("Results apply only to this recording, reference, calibration, dataset and supplied labels; no game-wide accuracy claim.")},
        {QStringLiteral("positions"), QJsonObject{{QStringLiteral("known_labels"), known}, {QStringLiteral("accepted_known"), accepted},
            {QStringLiteral("within_tolerance"), within}, {QStringLiteral("tolerance"), tolerance}, {QStringLiteral("missed_known"), missed},
            {QStringLiteral("unexpected_accepts_for_unknown_labels"), unexpected}, {QStringLiteral("mean_error"), ratio(total_error, accepted)},
            {QStringLiteral("rms_error"), accepted ? QJsonValue(std::sqrt(square_error / accepted)) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("max_error"), accepted ? QJsonValue(max_error) : QJsonValue(QJsonValue::Null)}}},
        {QStringLiteral("events"), QJsonObject{{QStringLiteral("true_positives"), tp}, {QStringLiteral("false_positives"), fp}, {QStringLiteral("false_negatives"), fn},
            {QStringLiteral("precision"), ratio(tp, tp + fp)}, {QStringLiteral("recall"), ratio(tp, tp + fn)},
            {QStringLiteral("correct_automatic_markers_on_matched_events"), correct_marker}, {QStringLiteral("wrong_automatic_markers_on_matched_events"), wrong_marker},
            {QStringLiteral("automatic_markers_on_unmatched_events"), unmatched_automatic},
            {QStringLiteral("unresolved_markers_on_matched_events"), unresolved}}}, {QStringLiteral("frames"), frame_results}};
}
}
