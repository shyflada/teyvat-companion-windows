#include "teyvat/detection.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace teyvat {
namespace {
[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }
constexpr double minimum_similarity = 0.96;
constexpr double minimum_margin = 0.04;
constexpr qint64 maximum_gap_ms = 1000;
constexpr qint64 maximum_transition_ms = 5000;

double number(const QJsonObject& object, const char* key, double low, double high) {
    const auto value = object.value(QLatin1String(key));
    const double result = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(result) || result < low || result > high) {
        invalid("Recording contains a missing or out-of-range number.");
    }
    return result;
}
LocalizationMethod localization_method(const QJsonObject& options) {
    const auto method = options.value(QStringLiteral("method"));
    if (method.isUndefined() || method == QStringLiteral("correlation")) { return LocalizationMethod::Correlation; }
    if (method == QStringLiteral("orb")) { return LocalizationMethod::Orb; }
    invalid("Unknown image-localization method.");
}
bool allow_rotation(const QJsonObject& options) {
    const auto value = options.value(QStringLiteral("allow_rotation"));
    if (!value.isUndefined() && !value.isBool()) { invalid("allow_rotation must be a boolean."); }
    return value.toBool(false);
}
QString text(const QJsonObject& object, const char* key, int limit = 512) {
    const auto value = object.value(QLatin1String(key));
    const auto result = value.toString();
    if (!value.isString() || result.trimmed().isEmpty() || result.size() > limit) {
        invalid("Recording requires bounded source, rights, map identifiers and image filenames.");
    }
    for (const auto c : result) { if (c.isNull() || c.category() == QChar::Other_Control) { invalid("Recording text contains control characters."); } }
    return result;
}
QByteArray read_file(const QString& path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) { invalid("Recording file is missing, empty or oversized."); }
    const auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFile::NoError) { invalid("Cannot read a bounded recording file."); }
    return bytes;
}
QImage png_image(const QByteArray& bytes) {
    QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png"); reader.setAutoTransform(false);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 1920 || size.height() > 1080) { invalid("Recording PNGs must be at most 1920 x 1080."); }
    auto image = reader.read();
    if (image.isNull()) { invalid("Cannot decode recording PNG."); }
    image = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) { if (qAlpha(row[x]) != 255) { invalid("Recording PNGs must be opaque."); } }
    }
    return image;
}
double similarity(const QImage& a, const QImage& b, const QRect& area) {
    qint64 difference = 0;
    for (int y = 0; y < b.height(); ++y) {
        const auto* left = reinterpret_cast<const QRgb*>(a.constScanLine(y + area.y())) + area.x();
        const auto* right = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < b.width(); ++x) {
            difference += std::abs(qRed(left[x]) - qRed(right[x])) + std::abs(qGreen(left[x]) - qGreen(right[x])) + std::abs(qBlue(left[x]) - qBlue(right[x]));
        }
    }
    return 1.0 - static_cast<double>(difference) / (255.0 * 3 * b.width() * b.height());
}
bool textured(const QImage& image) {
    double sum = 0, square = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) { const double v = qGray(row[x]); sum += v; square += v * v; }
    }
    const double count = image.width() * image.height();
    return square / count - std::pow(sum / count, 2) >= 100;
}
std::optional<RecordedPosition> position(const QJsonObject& frame) {
    if (!frame.contains(QStringLiteral("position")) || frame.value(QStringLiteral("position")).isNull()) { return {}; }
    if (!frame.value(QStringLiteral("position")).isObject()) { invalid("Position must be an object or null."); }
    const auto p = frame.value(QStringLiteral("position")).toObject();
    return RecordedPosition{number(p, "x", 0, 1000000), number(p, "y", 0, 1000000),
        number(p, "uncertainty", 0, 10000), number(p, "confidence", 0, 1)};
}
bool stable(const std::optional<RecordedPosition>& a, const std::optional<RecordedPosition>& b, double radius) {
    return a && b && a->confidence >= 0.95 && b->confidence >= 0.95
        && a->uncertainty <= radius / 4 && b->uncertainty <= radius / 4
        && std::hypot(a->x - b->x, a->y - b->y) <= radius / 2;
}
QString sibling(const QString& base, const QString& name, const QString& extension) {
    if (QFileInfo(name).fileName() != name || name.contains('/') || name.contains('\\') || name.contains(':')
        || !name.endsWith(extension, Qt::CaseInsensitive)) { invalid("Bundle resources must be sibling filenames with the expected extension."); }
    const QFileInfo file(QDir(base).filePath(name));
    if (file.canonicalPath().compare(base, Qt::CaseInsensitive) != 0 || file.isSymLink()) { invalid("Bundle resources must stay inside their directory."); }
    return file.absoluteFilePath();
}
} // namespace

OpeningTracker::OpeningTracker(QImage closed, QImage opened, double radius)
    : closed_(closed.convertToFormat(QImage::Format_ARGB32)), opened_(opened.convertToFormat(QImage::Format_ARGB32)), radius_(radius) {
    if (closed_.width() < 16 || closed_.height() < 16 || closed_.width() > 128 || closed_.height() > 128
        || closed_.size() != opened_.size() || !textured(closed_) || !textured(opened_)
        || similarity(closed_, opened_, closed_.rect()) > .90 || !std::isfinite(radius) || radius <= 0 || radius > 1000) {
        invalid("Opening tracker needs distinct textured 16..128 pixel templates and a bounded match radius.");
    }
}
void OpeningTracker::reset() {
    closed_count_ = open_count_ = 0; previous_ = armed_at_ = -1; latched_ = false;
    armed_position_.reset(); positions_stable_ = true;
}
std::optional<OpeningEvidence> OpeningTracker::push(const QImage& input, qint64 time, const std::optional<RecordedPosition>& position) {
    if (input.size() != closed_.size() || time < 0 || time <= previous_) { invalid("Opening samples require the calibrated size and increasing timestamps."); }
    const auto crop = input.convertToFormat(QImage::Format_ARGB32);
    if (previous_ >= 0 && time - previous_ > maximum_gap_ms) { reset(); }
    previous_ = time;
    const double c = similarity(crop, closed_, crop.rect()), o = similarity(crop, opened_, crop.rect());
    const bool is_closed = c >= minimum_similarity && c - o >= minimum_margin;
    const bool is_open = o >= minimum_similarity && o - c >= minimum_margin;
    if (armed_at_ >= 0 && !latched_) { positions_stable_ &= stable(armed_position_, position, radius_); }
    if (is_closed) {
        open_count_ = 0;
        if (closed_count_ == 0) { armed_at_ = -1; closed_score_ = c; armed_position_ = position; positions_stable_ = true; }
        else { closed_score_ = std::min(closed_score_, c); positions_stable_ &= stable(armed_position_, position, radius_); }
        if (++closed_count_ >= 2) { armed_at_ = time; latched_ = false; }
    } else {
        closed_count_ = 0;
        if (armed_at_ < 0 || time - armed_at_ > maximum_transition_ms || !is_open || latched_) { open_count_ = 0; return {}; }
        positions_stable_ &= stable(armed_position_, position, radius_);
        open_score_ = open_count_ == 0 ? o : std::min(open_score_, o);
        if (++open_count_ >= 2) {
            latched_ = true; open_count_ = 0;
            return OpeningEvidence{{}, armed_at_, time, closed_score_, open_score_, position, positions_stable_};
        }
    }
    return {};
}

TrackingSetup load_tracking_setup(const QString& path) {
    const auto bytes = read_file(path, 1024 * 1024); QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) { invalid("Invalid tracking setup JSON."); }
    const auto root = document.object();
    if (root.value(QStringLiteral("format")) != QStringLiteral("teyvat-tracking-session") || root.value(QStringLiteral("version")) != 1
        || !root.value(QStringLiteral("synthetic")).isBool() || root.contains(QStringLiteral("position"))) { invalid("Unsupported tracking setup."); }
    TrackingSetup setup; setup.synthetic = root.value(QStringLiteral("synthetic")).toBool();
    const auto base = QFileInfo(path).canonicalPath();
    setup.dataset = decode_json(read_file(sibling(base, text(root, "dataset", 128), QStringLiteral(".json")), max_transfer_bytes));
    if (setup.dataset.maps.size() != 1) { invalid("A tracking session requires exactly one calibrated region/level."); }
    for (const auto& marker : setup.dataset.markers) { if (marker.status != CollectionStatus::NotCollected) { invalid("Tracking data must contain definitions, not imported collected statuses."); } }
    const auto& layer = setup.dataset.maps.first(); setup.analysis.map = layer.key; setup.analysis.frame = layer.frame;
    setup.analysis.provenance = text(root, "provenance"); setup.analysis.license = text(root, "license");
    setup.analysis.radius = number(root, "match_radius", .001, 1000);
    const auto size = root.value(QStringLiteral("capture_size")).toObject();
    const auto integer = [](const QJsonObject& o, const char* key, int low, int high) {
        const double v = number(o, key, low, high); if (v != std::floor(v)) { invalid("Capture dimensions and regions must be integer pixels."); } return static_cast<int>(v);
    };
    setup.capture_size = {integer(size, "width", 160, 7680), integer(size, "height", 100, 4320)};
    QCryptographicHash hash(QCryptographicHash::Sha256); hash.addData(bytes); hash.addData(encode_json(setup.dataset));
    const auto image = [&](const QJsonObject& object, const char* key) {
        const auto data = read_file(sibling(base, text(object, key, 128), QStringLiteral(".png")), 4 * 1024 * 1024);
        hash.addData(data); return png_image(data);
    };
    const auto options = root.value(QStringLiteral("localization")).toObject(); auto& config = setup.localization;
    config.method = localization_method(options);
    config.allow_rotation = allow_rotation(options);
    config.reference = image(options, "reference");
    const auto roi = options.value(QStringLiteral("roi")).toObject();
    const int roi_limit = config.method == LocalizationMethod::Orb ? 512 : 128;
    config.roi = {integer(roi, "x", 0, 7679), integer(roi, "y", 0, 4319), integer(roi, "width", 48, roi_limit), integer(roi, "height", 48, roi_limit)};
    const auto origin = options.value(QStringLiteral("origin")).toObject(), units = options.value(QStringLiteral("units_per_pixel")).toObject();
    config.origin = {number(origin, "x", 0, 1000000), number(origin, "y", 0, 1000000)};
    config.units_per_pixel = {number(units, "x", .001, 1000), number(units, "y", .001, 1000)};
    for (const auto* key : {"scales", "rotations"}) {
        const auto value = options.value(QLatin1String(key));
        if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 16) { invalid("Tracking pose search requires bounded arrays."); }
        QVector<double> values;
        for (const auto& entry : value.toArray()) {
            if (!entry.isDouble() || !std::isfinite(entry.toDouble()) || values.contains(entry.toDouble())) { invalid("Invalid tracking pose search value."); }
            values.append(entry.toDouble());
        }
        if (QString::fromLatin1(key) == QStringLiteral("scales")) { config.scales = values; } else { config.rotations = values; }
    }
    if (options.contains(QStringLiteral("presence_template"))) {
        config.presence_template = image(options, "presence_template"); const auto area = options.value(QStringLiteral("presence_roi")).toObject();
        config.presence_roi = {integer(area, "x", 0, 7679), integer(area, "y", 0, 4319), integer(area, "width", 16, 128), integer(area, "height", 16, 128)};
        config.presence_minimum = number(options, "presence_minimum", .7, 1);
    }
    if (!setup.synthetic && config.presence_template.isNull()) { invalid("Game tracking requires a calibrated HUD presence guard."); }
    validate_localization(config);
    setup.analysis.reference_bounds = QRectF(config.origin, QSizeF((config.reference.width() - 1) * config.units_per_pixel.width(), (config.reference.height() - 1) * config.units_per_pixel.height()));
    if (!QRect(QPoint(0, 0), setup.capture_size).contains(config.roi)
        || (!config.presence_template.isNull() && !QRect(QPoint(0, 0), setup.capture_size).contains(config.presence_roi))
        || !QRectF(0, 0, layer.width, layer.height).contains(*setup.analysis.reference_bounds)) { invalid("Tracking calibration exceeds capture or map bounds."); }
    const auto opening = root.value(QStringLiteral("opening"));
    if (!opening.isUndefined()) {
        if (!opening.isObject()) { invalid("Opening configuration must be an object."); }
        const auto object = opening.toObject(), area = object.value(QStringLiteral("roi")).toObject();
        setup.opening_roi = {integer(area, "x", 0, 7679), integer(area, "y", 0, 4319), integer(area, "width", 16, 128), integer(area, "height", 16, 128)};
        setup.closed = image(object, "closed_template"); setup.opened = image(object, "opened_template");
        if (!QRect(QPoint(0, 0), setup.capture_size).contains(setup.opening_roi) || setup.opening_roi.intersects(config.roi.united(config.presence_roi))
            || setup.closed.size() != setup.opening_roi.size()) { invalid("Opening region must fit capture, match templates and avoid the minimap."); }
        (void)OpeningTracker(setup.closed, setup.opened, setup.analysis.radius);
    }
    setup.analysis.fingerprint = hash.result();
    return setup;
}

RecordingAnalysis analyze_recording(const QString& manifest_path, std::stop_token stop) {
    const auto bytes = read_file(manifest_path, 1024 * 1024);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) { invalid("Invalid recording JSON."); }
    const auto root = document.object();
    const auto version = root.value(QStringLiteral("version"));
    if (root.value(QStringLiteral("format")) != QStringLiteral("teyvat-offline-recording") || (version != 1 && version != 2)) { invalid("Unsupported offline recording format."); }
    const bool from_minimap = version == 2;
    if (!from_minimap && root.contains(QStringLiteral("localization"))) { invalid("Image-derived positions require recording version 2."); }
    RecordingAnalysis analysis;
    const auto map = root.value(QStringLiteral("map")).toObject();
    analysis.map = {text(map, "source", 128), text(map, "region", 128), text(map, "layer", 128)};
    analysis.frame = text(map, "frame", 128);
    analysis.provenance = text(root, "provenance"); analysis.license = text(root, "license");
    analysis.radius = number(root, "match_radius", 0.001, 1000);
    const auto roi = root.value(QStringLiteral("roi")).toObject();
    const auto integral = [&](const char* key, int low, int high) {
        const double value = number(roi, key, low, high);
        if (value != std::floor(value)) { invalid("Recording ROI must use integer pixels."); }
        return static_cast<int>(value);
    };
    const QRect area(integral("x", 0, 1919), integral("y", 0, 1079), integral("width", 16, 128), integral("height", 16, 128));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(from_minimap ? QByteArrayLiteral("teyvat-offline-localization-v1\0") : QByteArrayLiteral("teyvat-offline-template-v1\0"));
    hash.addData(QJsonDocument(root).toJson(QJsonDocument::Compact));
    const auto base = QFileInfo(manifest_path).canonicalPath();
    qint64 total_bytes = bytes.size();
    const auto image = [&](const QString& name) {
        const auto data = read_file(sibling(base, name, QStringLiteral(".png")), 4 * 1024 * 1024);
        total_bytes += data.size();
        if (total_bytes > 64 * 1024 * 1024) { invalid("Recording exceeds the 64 MiB compressed-input budget."); }
        hash.addData(QByteArray::number(data.size())); hash.addData(QByteArrayLiteral(":")); hash.addData(data);
        return png_image(data);
    };
    const auto closed = image(text(root, "closed_template", 128));
    const auto opened = image(text(root, "opened_template", 128));
    if (closed.size() != area.size() || opened.size() != area.size() || !textured(closed) || !textured(opened)
        || similarity(closed, opened, closed.rect()) > 0.90) { invalid("Templates must match ROI size, contain texture and represent distinct states."); }
    std::optional<LocalizationConfig> localization;
    if (from_minimap) {
        const auto options = root.value(QStringLiteral("localization")).toObject();
        LocalizationConfig config;
        config.method = localization_method(options);
        config.allow_rotation = allow_rotation(options);
        config.reference = image(text(options, "reference", 128));
        const auto loc_roi = options.value(QStringLiteral("roi")).toObject();
        const auto pixel = [&](const char* key, int low, int high) {
            const double value = number(loc_roi, key, low, high);
            if (value != std::floor(value)) { invalid("Minimap ROI must use integer pixels."); }
            return static_cast<int>(value);
        };
        const int roi_limit = config.method == LocalizationMethod::Orb ? 512 : 128;
        config.roi = QRect(pixel("x", 0, 1919), pixel("y", 0, 1079), pixel("width", 48, roi_limit), pixel("height", 48, roi_limit));
        const auto origin = options.value(QStringLiteral("origin")).toObject(), units = options.value(QStringLiteral("units_per_pixel")).toObject();
        config.origin = {number(origin, "x", 0, 1000000), number(origin, "y", 0, 1000000)};
        config.units_per_pixel = {number(units, "x", 0.001, 1000), number(units, "y", 0.001, 1000)};
        const auto list = [&](const char* key, double low, double high) {
            const auto value = options.value(QLatin1String(key));
            if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 16) { invalid("Minimap calibration needs bounded scale and rotation arrays."); }
            QVector<double> result;
            for (const auto& entry : value.toArray()) {
                if (!entry.isDouble() || !std::isfinite(entry.toDouble()) || entry.toDouble() < low || entry.toDouble() > high || result.contains(entry.toDouble())) { invalid("Minimap pose arrays contain invalid or duplicate values."); }
                result.append(entry.toDouble());
            }
            return result;
        };
        config.scales = list("scales", 0.5, 4); config.rotations = list("rotations", -180, 360);
        if (options.contains(QStringLiteral("presence_template"))) {
            config.presence_template = image(text(options, "presence_template", 128));
            const auto area = options.value(QStringLiteral("presence_roi")).toObject();
            const auto pixel_value = [&](const char* key, int low, int high) { const double value = number(area, key, low, high); if (value != std::floor(value)) { invalid("HUD presence ROI must use integer pixels."); } return static_cast<int>(value); };
            config.presence_roi = {pixel_value("x", 0, 1919), pixel_value("y", 0, 1079), pixel_value("width", 16, 128), pixel_value("height", 16, 128)};
            config.presence_minimum = number(options, "presence_minimum", .7, 1);
        }
        validate_localization(config);
        analysis.provenance += QStringLiteral("\nReference: ") + text(options, "provenance");
        analysis.license += QStringLiteral("\nReference: ") + text(options, "license");
        analysis.reference_bounds = QRectF(config.origin, QSizeF((config.reference.width() - 1) * config.units_per_pixel.width(), (config.reference.height() - 1) * config.units_per_pixel.height()));
        localization = std::move(config);
    }
    if (!root.value(QStringLiteral("frames")).isArray()) { invalid("Recording frames must be an array."); }
    const auto frames = root.value(QStringLiteral("frames")).toArray();
    if (frames.size() < 4 || frames.size() > (from_minimap ? 60 : 600)) { invalid("Recording requires 4..600 frames, or at most 60 with image localization."); }
    struct Frame { qint64 time; QImage image; std::optional<RecordedPosition> position; };
    // ponytail: bounded sequential decoding; fixed-ROI matching deliberately does not track camera motion.
    OpeningTracker tracker(closed, opened, analysis.radius);
    qint64 previous = -1;
    QSize frame_size;
    QHash<QByteArray, LocalizationResult> position_cache;
    for (const auto& value : frames) {
        if (stop.stop_requested()) { invalid("Analysis cancelled. No progress was changed."); }
        if (!value.isObject()) { invalid("Recording frame must be an object."); }
        const auto entry = value.toObject();
        const double raw_time = number(entry, "time_ms", 0, 600000);
        if (raw_time != std::floor(raw_time)) { invalid("Frame times must be integer milliseconds."); }
        if (from_minimap && entry.contains(QStringLiteral("position"))) { invalid("Version 2 derives positions from pixels; supplied positions are rejected. Put evaluation labels in a separate file."); }
        Frame f{static_cast<qint64>(raw_time), image(text(entry, "image", 128)), from_minimap ? std::nullopt : position(entry)};
        if (f.time <= previous) { invalid("Recording frame times must strictly increase."); }
        if (frame_size.isEmpty()) { frame_size = f.image.size(); }
        if (f.image.size() != frame_size || !f.image.rect().contains(area)) { invalid("Recording resolution/ROI changed or exceeds image bounds."); }
        if (localization) {
            if (!f.image.rect().contains(localization->roi) || localization->roi.united(localization->presence_roi).intersects(area)) { invalid("Minimap ROI must fit the frame and must not overlap chest evidence."); }
            const auto patch = f.image.copy(localization->roi);
            const auto key = QCryptographicHash::hash(QByteArray::fromRawData(reinterpret_cast<const char*>(patch.constBits()), patch.sizeInBytes()), QCryptographicHash::Sha256);
            auto estimate = position_cache.constFind(key);
            if (estimate == position_cache.cend()) { position_cache.insert(key, localize_minimap(f.image, *localization, stop)); estimate = position_cache.constFind(key); }
            f.position = estimate->position;
            analysis.localizations.append({f.time, *estimate});
        }
        previous = f.time;
        if (auto event = tracker.push(f.image.copy(area), f.time, f.position)) { analysis.openings.append(*event); }
    }
    analysis.frame_count = frames.size(); analysis.fingerprint = hash.result();
    for (auto& event : analysis.openings) {
        event.event_id = QCryptographicHash::hash(analysis.fingerprint + QByteArray::number(event.closed_ms) + ':' + QByteArray::number(event.opened_ms), QCryptographicHash::Sha256);
    }
    validate_analysis(analysis);
    return analysis;
}

void validate_analysis(const RecordingAnalysis& analysis) {
    if (analysis.fingerprint.size() != 32 || analysis.frame_count < 4 || analysis.frame_count > 600
        || !std::isfinite(analysis.radius) || analysis.radius <= 0 || analysis.radius > 1000
        || analysis.openings.size() > 150 || analysis.frame.isEmpty() || analysis.provenance.isEmpty() || analysis.license.isEmpty()) { invalid("Invalid recording analysis."); }
    if (analysis.reference_bounds && (!analysis.reference_bounds->isValid() || analysis.localizations.size() != analysis.frame_count || analysis.frame_count > 60)) { invalid("Invalid image-localization analysis."); }
    QSet<QByteArray> ids;
    qint64 previous = -1;
    for (const auto& event : analysis.openings) {
        const auto expected = QCryptographicHash::hash(analysis.fingerprint + QByteArray::number(event.closed_ms) + ':' + QByteArray::number(event.opened_ms), QCryptographicHash::Sha256);
        if (event.event_id != expected || ids.contains(event.event_id) || event.closed_ms < 0 || event.opened_ms <= event.closed_ms
            || event.opened_ms > 600000 || event.opened_ms <= previous || event.opened_ms - event.closed_ms > (event.animation ? 7000 : maximum_transition_ms)
            || !std::isfinite(event.closed_similarity) || !std::isfinite(event.opened_similarity)
            || event.closed_similarity < (event.animation ? 0 : minimum_similarity) || event.closed_similarity > 1
            || event.opened_similarity < (event.animation ? 0 : minimum_similarity) || event.opened_similarity > 1) { invalid("Invalid opening evidence."); }
        if (event.animation) {
            const auto& proof = *event.animation;
            if (proof.burst_ms < event.closed_ms || proof.dissolved_ms <= proof.burst_ms || proof.dissolved_ms > event.opened_ms
                || event.opened_ms - proof.dissolved_ms > 1000 || proof.interaction_frames < 2 || proof.dissolve_frames < 2
                || !std::isfinite(proof.ring_coverage) || proof.ring_coverage < .70 || proof.ring_coverage > 1
                || !std::isfinite(proof.gold_fraction) || proof.gold_fraction < .12 || proof.gold_fraction > 1
                || !std::isfinite(proof.blue_rise) || proof.blue_rise < .02 || proof.blue_rise > 1) { invalid("Incomplete chest animation evidence."); }
        }
        if (event.position) {
            const auto& p = *event.position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.uncertainty) || !std::isfinite(p.confidence)
                || p.x < 0 || p.y < 0 || p.x > 1000000 || p.y > 1000000 || p.uncertainty < 0 || p.uncertainty > 10000
                || p.confidence < 0 || p.confidence > 1) { invalid("Invalid recorded position."); }
        }
        ids.insert(event.event_id); previous = event.opened_ms;
    }
}

DetectionResult match_opening(const OpeningEvidence& event, const RecordingAnalysis& analysis,
                             const MapLayer& layer, const QVector<MapMarker>& markers) {
    DetectionResult result; result.time_ms = event.opened_ms;
    if (analysis.map != layer.key || analysis.frame != layer.frame) { invalid("Recording map/frame does not match the selected local layer."); }
    if (!event.position) { result.reason = DetectionReason::NoPosition; return result; }
    const auto& p = *event.position;
    if (p.x > layer.width || p.y > layer.height) { result.reason = DetectionReason::NoPosition; return result; }
    QVector<const MapMarker*> candidates;
    for (const auto& marker : markers) {
        if (marker.map == layer.key && std::hypot(marker.x - p.x, marker.y - p.y) <= analysis.radius + p.uncertainty) { candidates.append(&marker); result.candidates.append(marker.id); }
    }
    if (candidates.isEmpty()) { return result; }
    result.outcome = DetectionOutcome::Review;
    if (candidates.size() > 1) { result.reason = DetectionReason::AmbiguousMarkers; return result; }
    const auto& marker = *candidates.first();
    if (marker.retired) { result.outcome = DetectionOutcome::Unmatched; result.reason = DetectionReason::ArchivedMarker; return result; }
    const QStringList chest_categories{QStringLiteral("common"), QStringLiteral("exquisite"), QStringLiteral("precious"), QStringLiteral("luxurious"), QStringLiteral("remarkable")};
    if (!chest_categories.contains(marker.category)) { result.reason = DetectionReason::UnknownCategory; return result; }
    if (!event.position_stable || p.confidence < 0.95 || p.uncertainty > analysis.radius / 4
        || std::hypot(marker.x - p.x, marker.y - p.y) + p.uncertainty > analysis.radius) { result.reason = DetectionReason::UncertainPosition; return result; }
    result.outcome = is_confirmed(marker.status) ? DetectionOutcome::AlreadyCollected : DetectionOutcome::Applied;
    result.reason = is_confirmed(marker.status) ? DetectionReason::AlreadyCollected : DetectionReason::UniqueMatch;
    return result;
}
} // namespace teyvat
