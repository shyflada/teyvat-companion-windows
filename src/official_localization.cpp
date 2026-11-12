#include "teyvat/official_map.hpp"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QLineF>
#include <QSaveFile>
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>

namespace teyvat {
namespace {
void cancelled(std::stop_token stop) { if (stop.stop_requested()) { throw std::runtime_error("Official localization cancelled."); } }
cv::Mat gray(const QImage& image) {
    const auto g = image.convertToFormat(QImage::Format_Grayscale8);
    return cv::Mat(g.height(), g.width(), CV_8UC1, const_cast<uchar*>(g.constBits()), g.bytesPerLine()).clone();
}
cv::Mat terrain(const QImage& image) {
    const auto rgb = image.convertToFormat(QImage::Format_ARGB32);
    cv::Mat mask(rgb.height(), rgb.width(), CV_8UC1, cv::Scalar(255));
    for (int y = 0; y < rgb.height(); ++y) { const auto* row = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) { const auto c = row[x];
            // The reference includes snow and amber rock. Preserve both as
            // terrain so coarse search has enough landmarks in those regions.
            if (qAlpha(c) < 250 || qGray(c) < 30
                || (qBlue(c) > 170 && qGreen(c) > 180 && qRed(c) < 150)) { mask.at<uchar>(y, x) = 0; }
        }
    }
    cv::erode(mask, mask, cv::getStructuringElement(cv::MORPH_ELLIPSE, {9, 9})); return mask;
}
std::optional<cv::Point2d> player_arrow(const QImage& image) {
    const auto rgb = image.convertToFormat(QImage::Format_ARGB32); cv::Mat mask(rgb.height(), rgb.width(), CV_8UC1, cv::Scalar(0));
    for (int y = 0; y < rgb.height(); ++y) { const auto* row = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) { const auto c = row[x];
            if (qRed(c) < 80 && qGreen(c) > 170 && qBlue(c) > 190 && qGreen(c) - qRed(c) > 100) { mask.at<uchar>(y, x) = 255; }
        }
    }
    std::vector<std::vector<cv::Point>> contours; cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    double best = 0, second = 0; std::optional<cv::Point2d> result;
    for (const auto& contour : contours) {
        const double area = cv::contourArea(contour); const auto bounds = cv::boundingRect(contour);
        if (area < 180 || area > 3000 || bounds.width < 18 || bounds.height < 18 || bounds.width > 80 || bounds.height > 80
            || bounds.x <= 0 || bounds.y <= 0 || bounds.br().x >= image.width() || bounds.br().y >= image.height()
            || area < bounds.area() * .25) { continue; }
        if (area > best) { second = best; best = area; cv::Point2f center; float radius; cv::minEnclosingCircle(contour, center, radius); result = cv::Point2d(center); }
        else { second = std::max(second, area); }
    }
    if (second > best * .65) { return {}; } return result;
}
struct Fit {
    cv::Mat matrix, inliers;
    int count = 0, matches = 0;
    double rms = 0, scale = 0;
    cv::Point2d player;
};
cv::Point2d project(const cv::Mat& m, cv::Point2d p) {
    return {m.at<double>(0, 0) * p.x + m.at<double>(0, 1) * p.y + m.at<double>(0, 2),
        m.at<double>(1, 0) * p.x + m.at<double>(1, 1) * p.y + m.at<double>(1, 2)};
}
Fit fit(const std::vector<cv::Point2f>& a, const std::vector<cv::Point2f>& b, cv::Point2d player, QSize size,
    bool rotating = false, int minimum = 16) {
    Fit out; out.matches = static_cast<int>(a.size()); if (out.matches < minimum) { return out; }
    out.matrix = cv::estimateAffinePartial2D(a, b, out.inliers, cv::RANSAC, 2.5, 5000, .999, 10);
    if (out.matrix.empty() || !cv::checkRange(out.matrix)) { return out; }
    out.scale = std::hypot(out.matrix.at<double>(0, 0), out.matrix.at<double>(1, 0));
    const auto angle = std::atan2(out.matrix.at<double>(1, 0), out.matrix.at<double>(0, 0)) * 180 / std::numbers::pi;
    if ((!rotating && std::abs(angle) > 3) || out.scale < .03 || out.scale > 8) { return out; }
    std::vector<cv::Point2f> support; double square = 0;
    for (std::size_t i = 0; i < a.size(); ++i) { if (out.inliers.at<uchar>(static_cast<int>(i))) {
        support.push_back(a[i]); const auto delta = project(out.matrix, a[i]) - cv::Point2d(b[i]); square += delta.dot(delta);
    } }
    if (static_cast<int>(support.size()) < minimum || support.size() < a.size() * .5) { return out; }
    std::vector<cv::Point2f> hull; cv::convexHull(support, hull); const auto bounds = cv::boundingRect(support);
    out.rms = std::sqrt(square / support.size());
    const bool search_only = minimum < 16;
    if (out.rms > 1 || cv::contourArea(hull) < size.width() * size.height() * (search_only ? .015 : .06)
        || bounds.width < size.width() * (search_only ? .12 : .3) || bounds.height < size.height() * (search_only ? .12 : .3)
        || (!search_only && cv::pointPolygonTest(hull, player, true) < -8)) { return out; }
    out.count = static_cast<int>(support.size()); out.player = project(out.matrix, player); return out;
}
} // namespace

struct OfficialWorldLocator::Data {
    OfficialMap map;
    std::vector<cv::KeyPoint> points;
    cv::Mat descriptors;
    cv::Ptr<cv::DescriptorMatcher> matcher;
    QRectF area;
    std::vector<cv::KeyPoint> area_points;
    cv::Mat area_descriptors;
    cv::Ptr<cv::DescriptorMatcher> area_matcher;
};
OfficialWorldLocator::OfficialWorldLocator(OfficialMap map, std::stop_token stop) : data_(std::make_unique<Data>()) {
    cv::setNumThreads(1); data_->map = std::move(map); auto& d = *data_; cancelled(stop);
    const auto path = QDir(d.map.directory).filePath(QStringLiteral("sift-v3-") + d.map.tile_version + QStringLiteral(".bin"));
    QFile file(path); bool loaded = false;
    if (file.open(QIODevice::ReadOnly) && file.size() > 40 && file.size() < 64 * 1024 * 1024) {
        const auto bytes = file.readAll(), body = bytes.mid(32);
        if (QCryptographicHash::hash(body, QCryptographicHash::Sha256) == bytes.first(32)) {
            QDataStream in(body); in.setFloatingPointPrecision(QDataStream::SinglePrecision);
            QString magic, revision; qint32 id = 0; QSize size; quint32 count = 0; in >> magic >> revision >> id >> size >> count;
            if (magic == QStringLiteral("teyvat-sift-v3") && revision == d.map.tile_version && id == d.map.id && size == d.map.raster_size
                && count >= 16 && count <= 100000 && body.size() - in.device()->pos() == static_cast<qint64>(count) * (2 + 128) * 4) {
                bool valid = true;
                for (quint32 i = 0; i < count; ++i) { float x, y; in >> x >> y;
                    valid = valid && std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0 && x < size.width() / 4. && y < size.height() / 4.;
                    d.points.emplace_back(cv::Point2f{x, y}, 1);
                }
                d.descriptors.create(static_cast<int>(count), 128, CV_32F);
                const auto length = static_cast<qint64>(d.descriptors.total() * sizeof(float));
                loaded = valid && in.readRawData(reinterpret_cast<char*>(d.descriptors.data), length) == length
                    && in.status() == QDataStream::Ok && cv::checkRange(d.descriptors, true, nullptr, 0, 10000);
            }
        }
    }
    if (!loaded) {
        d.points.clear(); d.descriptors.release(); const auto sift = cv::SIFT::create(500, 3, .012, 10, 1.2);
        const int columns = (d.map.raster_size.width() / 4 + 255) / 256, rows = (d.map.raster_size.height() / 4 + 255) / 256;
        for (int y = 0; y < rows; ++y) { for (int x = 0; x < columns; ++x) {
            cancelled(stop); const auto tile = official_tile(d.map, x, y, -2, stop);
            std::vector<cv::KeyPoint> points; cv::Mat descriptors;
            sift->detectAndCompute(gray(tile), terrain(tile), points, descriptors);
            for (auto& p : points) { p.pt += cv::Point2f(static_cast<float>(x * 256), static_cast<float>(y * 256)); d.points.push_back(p); }
            d.descriptors.push_back(descriptors);
            if (d.points.size() > 100000) { throw std::runtime_error("Official feature index exceeds the memory limit."); }
        } }
        if (d.points.size() < 16) { throw std::runtime_error("Official map has insufficient visible terrain."); }
        QByteArray body; QDataStream out(&body, QIODevice::WriteOnly); out.setFloatingPointPrecision(QDataStream::SinglePrecision);
        out << QStringLiteral("teyvat-sift-v3") << d.map.tile_version << qint32(d.map.id) << d.map.raster_size << quint32(d.points.size());
        for (const auto& point : d.points) { out << point.pt.x << point.pt.y; }
        out.writeRawData(reinterpret_cast<const char*>(d.descriptors.data), static_cast<qint64>(d.descriptors.total() * sizeof(float)));
        QSaveFile saved(path); const auto bytes = QCryptographicHash::hash(body, QCryptographicHash::Sha256) + body; cancelled(stop);
        if (!saved.open(QIODevice::WriteOnly) || saved.write(bytes) != bytes.size() || !saved.commit()) { throw std::runtime_error("Cannot save the official feature index."); }
    }
    cancelled(stop); d.matcher = cv::makePtr<cv::FlannBasedMatcher>(cv::makePtr<cv::flann::KDTreeIndexParams>(4), cv::makePtr<cv::flann::SearchParams>(96));
    d.matcher->add({d.descriptors}); d.matcher->train();
}
OfficialWorldLocator::~OfficialWorldLocator() = default;
bool official_world_map_marker_visible(const QImage& image) {
    return image.width() >= 256 && image.height() >= 256 && image.width() <= 640 && image.height() <= 640
        && player_arrow(image).has_value();
}
LocalizationResult OfficialWorldLocator::locate(const QImage& image, std::stop_token stop) const {
    LocalizationResult result; result.reason = LocalizationReason::InsufficientFeatures; cancelled(stop);
    if (image.width() < 256 || image.height() < 256 || image.width() > 640 || image.height() > 640) { return result; }
    const auto arrow = player_arrow(image); if (!arrow) { result.reason = LocalizationReason::MinimapAbsent; return result; }
    const auto input = image.scaled(image.size() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const auto sift = cv::SIFT::create(4000, 3, .012, 10, 1.2); std::vector<cv::KeyPoint> points; cv::Mat descriptors;
    sift->detectAndCompute(gray(input), terrain(input), points, descriptors); result.features = static_cast<int>(points.size());
    if (descriptors.rows < 16) { return result; }
    std::vector<std::vector<cv::DMatch>> pairs; data_->matcher->knnMatch(descriptors, pairs, 2); cancelled(stop);
    std::vector<cv::Point2f> a, b; std::map<std::pair<int, int>, int> bins;
    for (const auto& pair : pairs) { if (pair.size() == 2 && pair[0].distance < .72 * pair[1].distance) {
        const auto s = points[static_cast<std::size_t>(pair[0].queryIdx)].pt, t = data_->points[static_cast<std::size_t>(pair[0].trainIdx)].pt;
        a.push_back(s); b.push_back(t); ++bins[{static_cast<int>(t.x / 512), static_cast<int>(t.y / 512)}];
    } }
    result.matches = static_cast<int>(a.size()); std::vector<Fit> fits;
    for (const auto& [bin, count] : bins) {
        cancelled(stop); if (count < 3) { continue; } std::vector<cv::Point2f> ca, cb;
        for (std::size_t i = 0; i < a.size(); ++i) { if (std::abs(b[i].x - (bin.first + .5) * 512) < 768 && std::abs(b[i].y - (bin.second + .5) * 512) < 768) {
            ca.push_back(a[i]); cb.push_back(b[i]);
        } }
        auto model = fit(ca, cb, *arrow * .5, input.size()); if (model.count >= 16) { fits.push_back(std::move(model)); }
    }
    if (fits.empty()) { result.reason = LocalizationReason::InconsistentGeometry; return result; }
    std::sort(fits.begin(), fits.end(), [](const auto& l, const auto& r) { return l.count > r.count; }); const auto& best = fits.front();
    for (const auto& next : fits) { if (cv::norm(next.player - best.player) > 8 && next.count >= best.count * .8) { result.reason = LocalizationReason::Ambiguous; return result; } }
    const auto p = data_->map.world({best.player.x, best.player.y}, -2);
    if (p.x() < 0 || p.y() < 0 || p.x() > 1000000 || p.y() > 1000000) { result.reason = LocalizationReason::OutsideReference; return result; }
    result.position = RecordedPosition{p.x(), p.y(), std::max(4., 3 * best.rms + 1) * 4, .97};
    result.inliers = best.count; result.reprojection_rms = best.rms; result.scale = best.scale * .5 * 4;
    result.similarity = static_cast<double>(best.count) / best.matches; result.rotation = 0; result.reason = LocalizationReason::Matched; return result;
}
LocalizationConfig official_local_reference(const OfficialMap& map, QPointF position, const LocalizationConfig& hud, std::stop_token stop) {
    auto config = hud; const auto center = map.raster(position);
    const auto x = std::clamp(static_cast<int>(center.x()) - 512, 0, map.raster_size.width() - 1024);
    const auto y = std::clamp(static_cast<int>(center.y()) - 512, 0, map.raster_size.height() - 1024);
    config.reference = official_region(map, {x, y, 1024, 1024}, 0, stop); config.origin = map.world({double(x), double(y)});
    config.units_per_pixel = {1, 1}; config.scales = {.5, 4}; config.rotations = {0}; config.allow_rotation = true; config.method = LocalizationMethod::Orb;
    validate_localization(config); return config;
}
bool global_recovery_due(bool anchored, int misses, qint64 since_attempt, bool resumed, bool ambiguous_plane) {
    return !ambiguous_plane && (resumed || ((!anchored || misses >= 3) && since_attempt >= 3000));
}
std::optional<OfficialAcquisition> OfficialWorldLocator::locate_minimap(const QImage& image, const LocalizationConfig& hud, std::stop_token stop, const QRectF& preferred_area) const {
    cancelled(stop);
    if (!image.rect().contains(hud.roi) || !minimap_present(image, hud)) { return {}; }
    const auto crop = image.copy(hud.roi);
    if (crop.width() < 96 || crop.height() != crop.width() || crop.width() > 640) { return {}; }
    struct Candidate { QPointF world; int count; };
    auto& data = *data_; bool scoped = false;
    if (preferred_area.isValid()) {
        if (data.area != preferred_area) {
            data.area = preferred_area; data.area_points.clear(); data.area_descriptors.release(); data.area_matcher.release();
            for (std::size_t i = 0; i < data.points.size(); ++i) {
                const auto& point = data.points[i];
                if (preferred_area.contains(data.map.world({point.pt.x, point.pt.y}, -2))) { data.area_points.push_back(point); data.area_descriptors.push_back(data.descriptors.row(static_cast<int>(i))); }
            }
            if (data.area_descriptors.rows >= 16) {
                data.area_matcher = cv::makePtr<cv::FlannBasedMatcher>(cv::makePtr<cv::flann::KDTreeIndexParams>(4), cv::makePtr<cv::flann::SearchParams>(96));
                data.area_matcher->add({data.area_descriptors}); data.area_matcher->train();
            }
        }
        scoped = !data.area_matcher.empty();
    }
    const auto matcher = scoped ? data.area_matcher : data.matcher;
    const auto& indexed = scoped ? data.area_points : data.points;
    const auto global_fallback = [&]() -> std::optional<OfficialAcquisition> { return scoped ? locate_minimap(image, hud, stop) : std::nullopt; };
    std::vector<Candidate> candidates, sparse;
    const auto sift = cv::SIFT::create(2500, 3, .012, 10, 1.2);
    for (double shrink : {1., .5, .25}) {
        cancelled(stop); const auto input = crop.scaled(crop.size() * shrink, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        auto mask = terrain(input); const double radius = input.width() * .5, center = (input.width() - 1) * .5;
        for (int y = 0; y < input.height(); ++y) { for (int x = 0; x < input.width(); ++x) {
            const double r = std::hypot(x - center, y - center);
            if (r < radius * .2 || r > radius * .9) { mask.at<uchar>(y, x) = 0; }
        } }
        std::vector<cv::KeyPoint> points; cv::Mat descriptors; sift->detectAndCompute(gray(input), mask, points, descriptors);
        if (descriptors.rows < 2) { continue; }
        std::vector<std::vector<cv::DMatch>> pairs; matcher->knnMatch(descriptors, pairs, 2);
        std::vector<cv::Point2f> a, b; std::map<std::pair<int, int>, int> bins;
        for (const auto& pair : pairs) { if (pair.size() == 2 && pair[0].distance < .75 * pair[1].distance) {
            const auto s = points[static_cast<std::size_t>(pair[0].queryIdx)].pt, t = indexed[static_cast<std::size_t>(pair[0].trainIdx)].pt;
            a.push_back(s); b.push_back(t); ++bins[{static_cast<int>(t.x / 512), static_cast<int>(t.y / 512)}];
        } }
        for (const auto& [bin, count] : bins) {
            cancelled(stop); if (count < 2) { continue; } std::vector<cv::Point2f> ca, cb;
            for (std::size_t i = 0; i < a.size(); ++i) { if (std::abs(b[i].x - (bin.first + .5) * 512) < 768 && std::abs(b[i].y - (bin.second + .5) * 512) < 768) { ca.push_back(a[i]); cb.push_back(b[i]); } }
            // A sparse coarse tile can nominate an area, never a pose. The
            // independent detailed matcher below requires stronger evidence.
            cv::Point2d average;
            for (const auto& p : cb) {
                if (static_cast<int>(p.x / 512) == bin.first && static_cast<int>(p.y / 512) == bin.second) { average += cv::Point2d(p); }
            }
            average *= 1. / count;
            sparse.push_back({data.map.world({average.x, average.y}, -2), count});
            const auto model = fit(ca, cb, {center, center}, input.size(), true, 4);
            const double scale = model.scale * shrink * 4;
            if (model.count < 4 || scale < .5 || scale > 4) { continue; }
            const auto world = data_->map.world({model.player.x, model.player.y}, -2);
            if (!QRectF(0, 0, 1000000, 1000000).contains(world)) { continue; }
            auto found = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& c) { return QLineF(c.world, world).length() < 128; });
            if (found == candidates.end()) { candidates.push_back({world, model.count}); }
            else if (model.count > found->count) { *found = {world, model.count}; }
        }
    }
    if (candidates.empty()) {
        std::sort(sparse.begin(), sparse.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
        for (const auto& area : sparse) {
            if (std::none_of(candidates.begin(), candidates.end(), [&](const auto& c) { return QLineF(c.world, area.world).length() < 256; })) { candidates.push_back({area.world, 0}); }
            if (candidates.size() == 4) { break; }
        }
    }
    if (candidates.empty()) { return global_fallback(); }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
    const int best_count = candidates.front().count;
    std::erase_if(candidates, [best_count](const auto& c) { return c.count < best_count * .65; });
    if (candidates.size() > 4) { return global_fallback(); }
    std::vector<OfficialAcquisition> verified;
    for (const auto& candidate : candidates) {
        cancelled(stop);
        LocalizationConfig reference;
        try { reference = official_local_reference(data_->map, candidate.world, hud, stop); }
        catch (const std::exception&) { cancelled(stop); continue; }
        auto estimate = localize_minimap(image, reference, stop);
        const int required = candidate.count >= 6 ? 16 : candidate.count >= 4 ? 20 : 24;
        if (estimate.position && estimate.inliers >= required && estimate.similarity >= (candidate.count ? .6 : .75)) { verified.push_back({estimate, std::move(reference)}); }
    }
    if (verified.empty()) { return global_fallback(); }
    std::sort(verified.begin(), verified.end(), [](const auto& a, const auto& b) { return a.estimate.inliers > b.estimate.inliers; });
    const auto& best = verified.front(); const auto& position = *best.estimate.position;
    for (const auto& next : verified) {
        const auto& p = *next.estimate.position;
        if (std::hypot(p.x - position.x, p.y - position.y) > 8 && next.estimate.inliers >= best.estimate.inliers * .8) { return {}; }
    }
    return best;
}
OfficialPlaneSearch match_official_planes(const OfficialMap& map, const QImage& image, const LocalizationConfig& hud,
    QPointF hint, const QString& current_layer, bool world_map, std::stop_token stop, bool allow_download) {
    cancelled(stop); auto calibration = hud; std::optional<cv::Point2d> arrow;
    if (world_map) {
        arrow = player_arrow(image); if (!arrow) { return {}; }
        const int cx = qRound(arrow->x), cy = qRound(arrow->y);
        const int edge = std::min({282, cx * 2 - 2, cy * 2 - 2, (image.width() - cx) * 2 - 2, (image.height() - cy) * 2 - 2});
        if (edge < 96) { return {}; }
        calibration.roi = {cx - edge / 2, cy - edge / 2, edge, edge}; calibration.presence_template = {}; calibration.presence_roi = {};
    } else if (!minimap_present(image, hud)) { return {}; }
    QStringList layers{QStringLiteral("surface")};
    for (const auto& floor : map.floors) {
        if (floor.layer == current_layer || floor.bounds.adjusted(-96, -96, 96, 96).contains(hint)) { layers.append(floor.layer); }
    }
    if (layers.size() > 13) { return {{}, true}; } // Do not pick an arbitrary level from a large stack.
    std::vector<OfficialAcquisition> matches;
    for (const auto& layer : layers) {
        cancelled(stop);
        try {
            std::optional<LocalizationConfig> reference;
            if (layer == QStringLiteral("surface")) { reference = official_local_reference(map, hint, calibration, stop); }
            else { reference = official_floor_reference(map, layer, hint, calibration, stop, allow_download); }
            if (!reference) { continue; }
            if (world_map) { reference->scales = {.1, 8}; reference->allow_rotation = false; }
            auto estimate = localize_minimap(image, *reference, stop);
            if (!estimate.position || estimate.inliers < 16 || estimate.similarity < .6) { continue; }
            if (arrow) {
                const auto center = QPointF(reference->roi.x() + (reference->roi.width() - 1) * .5, reference->roi.y() + (reference->roi.height() - 1) * .5);
                const double x = arrow->x - center.x(), y = arrow->y - center.y(), a = estimate.rotation * std::numbers::pi / 180;
                estimate.position->x += estimate.scale * (std::cos(a) * x - std::sin(a) * y);
                estimate.position->y += estimate.scale * (std::sin(a) * x + std::cos(a) * y);
            }
            if (layer != QStringLiteral("surface")) {
                const auto f = std::find_if(map.floors.cbegin(), map.floors.cend(), [&](const auto& floor) { return floor.layer == layer; });
                if (f == map.floors.cend() || !f->bounds.contains({estimate.position->x, estimate.position->y})) { continue; }
            }
            reference->roi = hud.roi; reference->presence_roi = hud.presence_roi; reference->presence_template = hud.presence_template;
            reference->presence_minimum = hud.presence_minimum; reference->scales = {.5, 4}; reference->allow_rotation = true;
            matches.push_back({estimate, std::move(*reference), layer});
        } catch (const std::exception&) { if (stop.stop_requested()) { throw; } }
    }
    if (matches.empty()) { return {}; }
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) { return a.estimate.inliers > b.estimate.inliers; });
    if (matches.size() > 1 && matches[1].estimate.inliers >= matches[0].estimate.inliers * .8) { return {{}, true}; }
    return {matches.front(), false};
}
std::optional<OfficialAcquisition> verify_official_waypoint(const OfficialMap& map,
    const QImage& image, const LocalizationConfig& hud, const OfficialWaypoint& waypoint, std::stop_token stop) {
    auto result = match_official_planes(map, image, hud, waypoint.position, waypoint.layer, false, stop);
    if (result.ambiguous || !result.match || result.match->layer != waypoint.layer) { return {}; }
    const auto& p = *result.match->estimate.position;
    if (QLineF(waypoint.position, {p.x, p.y}).length() > 64) { return {}; }
    return std::move(result.match);
}
} // namespace teyvat
