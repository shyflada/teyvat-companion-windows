#include "teyvat/tracking.hpp"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace teyvat {
TrackingSession::TrackingSession(Store& store, TrackingSetup setup)
    : store_(store), setup_(std::move(setup)), profile_(store.active_profile().id),
      options_(QFileInfo(store.path()).absolutePath() + QStringLiteral("/navigation.ini"), QSettings::IniFormat),
      run_id_(QUuid::createUuid().toRfc4122()) {
    validate_dataset(setup_.dataset); validate_localization(setup_.localization);
    if (setup_.dataset.maps.size() != 1) { throw std::invalid_argument("Tracking requires one calibrated layer."); }
    if (setup_.analysis.fingerprint.size() != 32 || setup_.analysis.map != setup_.dataset.maps.first().key
        || setup_.analysis.frame != setup_.dataset.maps.first().frame || !setup_.analysis.reference_bounds
        || !std::isfinite(setup_.analysis.radius) || setup_.analysis.radius <= 0 || setup_.analysis.radius > 1000
        || setup_.analysis.provenance.isEmpty() || setup_.analysis.license.isEmpty()) { throw std::invalid_argument("Invalid tracking calibration metadata."); }
    const auto current = store_.export_layer(profile_, setup_.analysis.map);
    if (current.maps.size() != 1 || current.maps.first().frame != setup_.analysis.frame
        || current.maps.first().width != setup_.dataset.maps.first().width || current.maps.first().height != setup_.dataset.maps.first().height) {
        throw std::invalid_argument("The tracking setup and current local layer do not match.");
    }
    if (!setup_.closed.isNull()) { detector_ = std::make_unique<OpeningTracker>(setup_.closed, setup_.opened, setup_.analysis.radius); }
    prefix_ = QString::fromLatin1(QCryptographicHash::hash(profile_.toUtf8() + '\0' + layer_identity(setup_.analysis.map).toUtf8() + '\0' + setup_.analysis.frame.toUtf8(), QCryptographicHash::Sha256).toHex()) + QLatin1Char('/');
    options_.setFallbacksEnabled(false);
    const auto target = options_.value(prefix_ + QStringLiteral("target")).toStringList();
    const auto encoded = options_.value(prefix_ + QStringLiteral("skipped")).toByteArray();
    std::optional<MarkerKey> restored;
    if (target.size() == 2 && target[0].size() <= 128 && target[1].size() <= 128) { restored = MarkerKey{target[0].toStdString(), target[1].toStdString()}; }
    std::vector<MarkerKey> skipped;
    if (encoded.size() <= 8 * 1024 * 1024) {
        const auto entries = QJsonDocument::fromJson(encoded).array();
        if (entries.size() <= 5000) {
            for (const auto& entry : entries) { const auto key = entry.toArray(); if (key.size() == 2 && key[0].isString() && key[1].isString()
                && key[0].toString().size() <= 128 && key[1].toString().size() <= 128) { skipped.push_back({key[0].toString().toStdString(), key[1].toString().toStdString()}); } }
        }
    }
    selector_.restore(restored, std::move(skipped)); writable_ = options_.status() == QSettings::NoError;
    const auto hint = options_.value(prefix_ + QStringLiteral("searchHint"));
    if (hint.metaType() == QMetaType::fromType<QPointF>()) {
        const auto point = hint.toPointF();
        if (std::isfinite(point.x()) && std::isfinite(point.y()) && setup_.analysis.reference_bounds->contains(point)) { view_.last_position = point; }
    }
    if (!writable_) { settings_error_ = QStringLiteral("Navigation settings could not be read; the existing file is preserved."); }
}
void TrackingSession::save_target() {
    if (!writable_) { return; }
    QStringList target;
    if (selector_.target()) { target = {QString::fromStdString(selector_.target()->source), QString::fromStdString(selector_.target()->id)}; }
    options_.setValue(prefix_ + QStringLiteral("target"), target);
    if (view_.last_position) { options_.setValue(prefix_ + QStringLiteral("searchHint"), *view_.last_position); }
    QJsonArray skipped;
    for (const auto& key : selector_.skipped()) { skipped.append(QJsonArray{QString::fromStdString(key.source), QString::fromStdString(key.id)}); }
    options_.setValue(prefix_ + QStringLiteral("skipped"), QJsonDocument(skipped).toJson(QJsonDocument::Compact)); options_.sync();
    if (options_.status() != QSettings::NoError) { writable_ = false; settings_error_ = QStringLiteral("Navigation settings could not be saved."); }
}
void TrackingSession::refresh() {
    view_.guidance.reset(); view_.target_label.clear(); view_.near_target = false;
    if (store_.active_profile().id != profile_) { view_.state = TrackingState::ProfileChanged; pose_.reset(); return; }
    if (!pose_) { view_.state = TrackingState::PositionUnknown; return; }
    const auto data = store_.export_layer(profile_, setup_.analysis.map); const auto& layer = data.maps.first();
    if (layer.frame != setup_.analysis.frame || layer.width != setup_.dataset.maps.first().width || layer.height != setup_.dataset.maps.first().height
        || !setup_.analysis.reference_bounds || !setup_.analysis.reference_bounds->contains(QPointF(pose_->position.x, pose_->position.y))) {
        pose_.reset(); view_.state = TrackingState::PositionUnknown; return;
    }
    view_.last_position = QPointF(pose_->position.x, pose_->position.y);
    std::vector<NavigationChest> chests; chests.reserve(static_cast<std::size_t>(data.markers.size()));
    for (const auto& marker : data.markers) {
        chests.push_back({{{marker.map.source.toStdString(), marker.id.toStdString()}, marker.category.toStdString(),
            {marker.x, marker.y, layer.frame.toStdString(), marker.map.region.toStdString(), marker.map.layer.toStdString()}},
            remote_collected_.contains(marker.id) ? CollectionStatus::Collected : marker.status,
            marker.retired || (allowed_targets_ && !allowed_targets_->contains(marker.id))});
    }
    const auto previous = selector_.target(); view_.guidance = selector_.update(pose_, chests);
    if (previous != selector_.target()) { save_target(); }
    if (!view_.guidance) { view_.state = TrackingState::NoTarget; return; }
    const auto marker = std::find_if(data.markers.cbegin(), data.markers.cend(), [&](const auto& value) { return value.id.toStdString() == view_.guidance->target.id; });
    if (marker != data.markers.cend()) { view_.target_label = marker->label; }
    view_.near_target = view_.guidance->map_units <= near_distance_;
    view_.state = view_.near_target ? TrackingState::NearTarget : TrackingState::Guiding;
    if (view_.review_reason) { view_.state = TrackingState::Review; }
}
void TrackingSession::observe(const LocalizationResult& estimate, const QImage& crop, qint64 time) {
    if (time < 0 || time <= last_time_) { throw std::invalid_argument("Tracking observations must use increasing monotonic times."); }
    last_time_ = time;
    view_.position_reason = estimate.reason;
    view_.limited_position = estimate.position.has_value() && estimate.limited_support;
    if (store_.active_profile().id != profile_) { pause(); view_.state = TrackingState::ProfileChanged; return; }
    pose_.reset();
    if (estimate.position && estimate.reason == LocalizationReason::Matched) {
        const auto& p = *estimate.position;
        pose_ = NavigationPose{{p.x, p.y, setup_.analysis.frame.toStdString(), setup_.analysis.map.region.toStdString(), setup_.analysis.map.layer.toStdString()},
            p.confidence, p.uncertainty, estimate.rotation, setup_.localization.units_per_pixel.width(), setup_.localization.units_per_pixel.height()};
    }
    refresh();
    if (bucket_ != time / 540000) { bucket_ = time / 540000; reset_opening(); }
    recent_.append({time % 540000, estimate});
    while (recent_.size() > 60 || (!recent_.isEmpty() && recent_.first().time_ms < time % 540000 - 6000)) { recent_.removeFirst(); }
    if (!pose_) {
        const bool transient = estimate.reason == LocalizationReason::WeakMatch || estimate.reason == LocalizationReason::InsufficientFeatures
            || estimate.reason == LocalizationReason::InconsistentGeometry || estimate.reason == LocalizationReason::LowTexture;
        if (!transient) { animation_.reset(); pending_animation_.reset(); scene_target_.reset(); }
    }
    if (pending_animation_ && pose_ && pose_->uncertainty <= 5 && scene_target_ && selector_.target() == scene_target_) {
        const auto age = time % 540000 - pending_animation_->animation->dissolved_ms;
        // A result captured before the scene event can finish afterwards.
        // Keep waiting for a genuinely newer pose instead of dropping the event.
        if (age >= 0) {
            auto event = *pending_animation_; pending_animation_.reset();
            if (age <= 1000 && event.position) {
                event.position_stable &= std::hypot(event.position->x - estimate.position->x, event.position->y - estimate.position->y) <= 2;
                event.position = estimate.position; event.opened_ms = time % 540000;
                apply_opening(std::move(event), *scene_target_);
            }
        }
    }
    if (!detector_) { return; }
    if (crop.isNull()) { detector_->reset(); armed_target_.reset(); return; }
    const auto event = detector_->push(crop, time % 540000, pose_ ? estimate.position : std::nullopt);
    if (detector_->armed() && !armed_target_) { armed_target_ = selector_.target(); }
    if (!event) { if (!detector_->armed()) { armed_target_.reset(); } return; }
    if (armed_target_ && selector_.target() == armed_target_) { apply_opening(*event, *armed_target_); }
    armed_target_.reset();
}
void TrackingSession::observe_scene(const QImage& scene, qint64 time) {
    if (time < 0 || time <= last_scene_time_) { return; } last_scene_time_ = time;
    if (setup_.synthetic || !setup_.analysis.map.source.startsWith(QStringLiteral("hoyolab-map-"))) { return; }
    // A brief terrain miss clears live guidance, but need not erase visual
    // evidence. The animation tracker still expires gaps over one second;
    // pause/menu/plane changes reset it and commits require a fresh good pose.
    if (!pose_) { return; }
    if (!view_.guidance || time < last_time_ || view_.guidance->map_units > setup_.analysis.radius
        || bucket_ != time / 540000 || store_.active_profile().id != profile_) {
        animation_.reset(); pending_animation_.reset(); scene_target_.reset(); return;
    }
    if (time - last_time_ > 900 || pose_->uncertainty > 5) { return; }
    if (scene_target_ != selector_.target()) { animation_.reset(); pending_animation_.reset(); scene_target_ = selector_.target(); }
    const RecordedPosition p{pose_->position.x, pose_->position.y, pose_->uncertainty, pose_->confidence};
    if (auto event = animation_.push(scene, time % 540000, p)) { pending_animation_ = std::move(event); }
}
void TrackingSession::apply_opening(OpeningEvidence event, const MarkerKey& expected) {
    if (selector_.target() != expected || recent_.size() < 4) { return; }
    auto analysis = setup_.analysis; analysis.localizations = recent_; analysis.frame_count = recent_.size();
    const auto method = event.animation ? QByteArrayLiteral("animation") : QByteArrayLiteral("template");
    analysis.fingerprint = QCryptographicHash::hash(setup_.analysis.fingerprint + run_id_ + QByteArray::number(bucket_) + method, QCryptographicHash::Sha256);
    event.event_id = QCryptographicHash::hash(analysis.fingerprint + QByteArray::number(event.closed_ms) + ':' + QByteArray::number(event.opened_ms), QCryptographicHash::Sha256);
    if (event.animation) { analysis.provenance += QStringLiteral("; interaction, gold-ring burst and blue dissolution; locally verified position"); }
    analysis.openings = {event};
    const auto results = store_.apply_detections(profile_, analysis, QString::fromStdString(expected.id));
    if (!results.isEmpty() && results[0].outcome == DetectionOutcome::Applied) { ++view_.automatic_changes; view_.review_reason.reset(); reset_opening(); refresh(); }
    else if (!results.isEmpty() && results[0].outcome == DetectionOutcome::Review) { view_.state = TrackingState::Review; view_.review_reason = results[0].reason; }
}
void TrackingSession::reset_opening() {
    if (detector_) { detector_->reset(); } armed_target_.reset(); recent_.clear();
    animation_.reset(); scene_target_.reset(); pending_animation_.reset();
}
void TrackingSession::reanchor(QPointF position, bool preserve_target) {
    if (!std::isfinite(position.x()) || !std::isfinite(position.y()) || !setup_.analysis.reference_bounds->contains(position)) { throw std::invalid_argument("Reacquired position is outside the map bounds."); }
    if (store_.active_profile().id != profile_) { pause(); view_.state = TrackingState::ProfileChanged; return; }
    if (!preserve_target && (!view_.last_position || std::hypot(position.x() - view_.last_position->x(), position.y() - view_.last_position->y()) > 64)) {
        selector_.restore({}, selector_.skipped()); view_.review_reason.reset(); save_target();
    }
    pause(); view_.last_position = position;
}
void TrackingSession::pause() { if (pose_) { save_target(); } pose_.reset(); reset_opening(); view_.guidance.reset(); view_.target_label.clear(); view_.near_target = false; view_.limited_position = false; view_.state = TrackingState::Paused; }
void TrackingSession::set_near_distance(double distance) {
    if (!std::isfinite(distance) || distance < 5 || distance > 100) { throw std::invalid_argument("Near distance must be between 5 and 100 map units."); }
    near_distance_ = distance; refresh();
}
void TrackingSession::set_remote_collected(const QSet<QString>& ids) {
    if (ids.size() > max_markers) { throw std::invalid_argument("Too many remote chest IDs."); }
    if (ids == remote_collected_) { return; }
    const auto previous = selector_.target(); remote_collected_ = ids; refresh();
    if (previous != selector_.target()) { reset_opening(); }
}
void TrackingSession::set_allowed_targets(std::optional<QSet<QString>> ids) {
    if (ids && ids->size() > max_markers) { throw std::invalid_argument("Too many regional target IDs."); }
    if (ids == allowed_targets_) { return; } allowed_targets_ = std::move(ids);
    selector_.restore({}, selector_.skipped()); reset_opening(); view_.review_reason.reset(); refresh(); save_target();
}
std::optional<MarkerKey> TrackingSession::confirm_target() {
    if (store_.active_profile().id != profile_) { pause(); view_.state = TrackingState::ProfileChanged; return {}; }
    const auto key = selector_.target(); if (!key) { return {}; }
    if (!pose_ || !view_.guidance) { return {}; }
    if (allowed_targets_ && !allowed_targets_->contains(QString::fromStdString(key->id))) { return {}; }
    const auto data = store_.export_layer(profile_, setup_.analysis.map);
    const auto found = std::find_if(data.markers.cbegin(), data.markers.cend(), [&](const auto& m) {
        return m.map.source.toStdString() == key->source && m.id.toStdString() == key->id && !m.retired;
    });
    if (found == data.markers.cend()) { selector_.restore({}, selector_.skipped()); refresh(); return {}; }
    if (!is_confirmed(found->status)) { store_.set_status(profile_, {*found}, CollectionStatus::ManuallyConfirmed); ++view_.confirmed_changes; }
    selector_.restore({}, selector_.skipped()); save_target();
    reset_opening(); view_.review_reason.reset(); refresh(); return key;
}
void TrackingSession::skip() { selector_.skip(); save_target(); reset_opening(); view_.review_reason.reset(); refresh(); }
void TrackingSession::undo() { if (store_.active_profile().id != profile_) { pause(); return; } store_.undo_last(profile_); selector_.restore({}, selector_.skipped()); reset_opening(); view_.review_reason.reset(); refresh(); }
} // namespace teyvat
