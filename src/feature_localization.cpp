#include "teyvat/localization.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace teyvat {
namespace {
cv::Mat gray(const QImage& image) {
    const auto g = image.convertToFormat(QImage::Format_Grayscale8);
    return cv::Mat(g.height(), g.width(), CV_8UC1, const_cast<uchar*>(g.constBits()), g.bytesPerLine()).clone();
}
cv::Mat terrain_mask(const QImage& image, bool circular) {
    cv::Mat mask(image.height(), image.width(), CV_8UC1, cv::Scalar(255));
    for (int y = 0; y < image.height(); ++y) { for (int x = 0; x < image.width(); ++x) {
        const auto c = image.pixelColor(x, y);
        // Keep snow and warm-colored terrain. Amber sand and rock can have the
        // same color as a HUD icon, while mismatched icons fail the geometry fit.
        const bool icon = (c.blue() > 170 && c.green() > 180 && c.red() < 150)
            || qGray(c.rgb()) < 35 || c.alpha() < 250;
        if (icon) { mask.at<uchar>(y, x) = 0; }
    } }
    cv::erode(mask, mask, cv::getStructuringElement(cv::MORPH_ELLIPSE, {13, 13}));
    if (circular) {
        const double cx = (image.width() - 1) * .5, cy = (image.height() - 1) * .5, r = image.width() * .5;
        for (int y = 0; y < image.height(); ++y) { for (int x = 0; x < image.width(); ++x) {
            const double d = std::hypot(x - cx, y - cy); if (d < r * .28 || d > r * .82) { mask.at<uchar>(y, x) = 0; }
        } }
    }
    return mask;
}
void cancelled(std::stop_token stop) { if (stop.stop_requested()) { throw std::runtime_error("Analysis cancelled."); } }
struct Model {
    cv::Mat matrix, inliers;
    cv::Point2d center;
    double rms = 0, area = 0, width = 0, height = 0, scale = 0, angle = 0;
    int count = 0;
};
Model fit(const std::vector<cv::Point2f>& source, const std::vector<cv::Point2f>& destination, QSize size) {
    Model result; if (source.size() < 12) { return result; }
    result.matrix = cv::estimateAffinePartial2D(source, destination, result.inliers, cv::RANSAC, 2.5, 2000, .999, 10);
    if (result.matrix.empty() || !cv::checkRange(result.matrix)) { return result; }
    const auto& m = result.matrix;
    const auto project = [&](cv::Point2d p) { return cv::Point2d(m.at<double>(0, 0) * p.x + m.at<double>(0, 1) * p.y + m.at<double>(0, 2), m.at<double>(1, 0) * p.x + m.at<double>(1, 1) * p.y + m.at<double>(1, 2)); };
    result.center = project({(size.width() - 1) * .5, (size.height() - 1) * .5});
    result.scale = std::hypot(m.at<double>(0, 0), m.at<double>(1, 0));
    result.angle = std::atan2(m.at<double>(1, 0), m.at<double>(0, 0)) * 180 / std::numbers::pi;
    std::vector<cv::Point2f> support; double square = 0;
    for (std::size_t i = 0; i < source.size(); ++i) { if (result.inliers.at<uchar>(static_cast<int>(i))) {
        const auto delta = project(source[i]) - cv::Point2d(destination[i]); square += delta.dot(delta); ++result.count; support.push_back(source[i]);
    } }
    if (support.size() >= 3) {
        std::vector<cv::Point2f> hull; cv::convexHull(support, hull); result.area = cv::contourArea(hull) / (size.width() * size.height());
        const auto bounds = cv::boundingRect(support); result.width = static_cast<double>(bounds.width) / size.width(); result.height = static_cast<double>(bounds.height) / size.height();
    }
    result.rms = result.count ? std::sqrt(square / result.count) : 0;
    return result;
}
bool supported(const Model& model, int matches) {
    return model.count >= 12 && model.count >= matches * .5 && model.area >= .06 && model.width >= .25 && model.height >= .25 && model.rms <= 1.5;
}
bool verified_coast(const Model& model, int matches, const QImage& crop, const QImage& reference) {
    if (model.count < 24 || model.count < matches * .70 || model.rms > 1.5 || std::max(model.width, model.height) < .18) { return false; }
    cv::Mat warped, valid;
    cv::warpAffine(gray(reference), warped, model.matrix, {crop.width(), crop.height()}, cv::INTER_LINEAR | cv::WARP_INVERSE_MAP);
    cv::warpAffine(terrain_mask(reference, false), valid, model.matrix, {crop.width(), crop.height()}, cv::INTER_NEAREST | cv::WARP_INVERSE_MAP);
    cv::bitwise_and(valid, terrain_mask(crop, true), valid);
    const int count = cv::countNonZero(valid);
    if (count < 2000) { return false; }
    const auto current = gray(crop); cv::Scalar a, sa, b, sb;
    cv::meanStdDev(current, a, sa, valid); cv::meanStdDev(warped, b, sb, valid);
    // Small islands can have low contrast across a broad water background.
    // Many geometric inliers plus dense, high-correlation terrain are required.
    if (sa[0] < 5 || sb[0] < 5) { return false; }
    double covariance = 0;
    for (int y = 0; y < current.rows; ++y) { for (int x = 0; x < current.cols; ++x) {
        if (valid.at<uchar>(y, x)) { covariance += (current.at<uchar>(y, x) - a[0]) * (warped.at<uchar>(y, x) - b[0]); }
    } }
    const auto score = covariance / count / sa[0] / sb[0];
    return score >= .9;
}
}

bool minimap_present(const QImage& image, const LocalizationConfig& config) {
    if (config.presence_template.isNull()) { return true; }
    if (!image.rect().contains(config.presence_roi)) { throw std::runtime_error("HUD presence region exceeds the input image."); }
    const auto reference = gray(config.presence_template);
    cv::Scalar mean, deviation; cv::meanStdDev(reference, mean, deviation);
    if (deviation[0] < 10) { throw std::runtime_error("HUD presence template must contain texture."); }
    cv::Mat scores; cv::matchTemplate(gray(image.copy(config.presence_roi)), reference, scores, cv::TM_CCOEFF_NORMED);
    const double value = scores.at<float>(0, 0); return std::isfinite(value) && value >= config.presence_minimum;
}
static LocalizationResult feature_attempt(const QImage& image, const LocalizationConfig& config, std::stop_token stop, bool sift) {
    static const bool single_thread = [] { cv::setNumThreads(1); return true; }(); (void)single_thread;
    cancelled(stop); const auto crop = image.copy(config.roi);
    thread_local auto orb = cv::ORB::create(2000, 1.2f, 8, 15, 0, 2, cv::ORB::HARRIS_SCORE, 31, 7);
    thread_local auto scale_invariant = cv::SIFT::create(2000, 3, .02, 10, 1.2);
    thread_local auto reference_sift = cv::SIFT::create(400, 3, .02, 10, 1.2);
    const cv::Ptr<cv::Feature2D> detector = sift ? cv::Ptr<cv::Feature2D>(scale_invariant) : cv::Ptr<cv::Feature2D>(orb);
    struct Reference { qint64 key = 0; cv::Mat descriptors; std::vector<cv::KeyPoint> points; };
    thread_local Reference references[2]; auto& reference = references[sift ? 1 : 0];
    if (reference.key != config.reference.cacheKey()) {
        reference.key = config.reference.cacheKey();
        const auto raster = gray(config.reference), mask = terrain_mask(config.reference, false);
        reference.points.clear(); reference.descriptors.release();
        if (sift && config.reference.width() >= 512 && config.reference.height() >= 512) {
            // Dense cities must not consume the entire descriptor budget and
            // leave a sparse coastal/grassland part of the reference unindexed.
            for (int y = 0; y < raster.rows; y += 256) { for (int x = 0; x < raster.cols; x += 256) {
                const cv::Rect cell(x, y, std::min(256, raster.cols - x), std::min(256, raster.rows - y));
                const auto patch = (cell + cv::Size(64, 64) - cv::Point(32, 32)) & cv::Rect(0, 0, raster.cols, raster.rows);
                std::vector<cv::KeyPoint> points; cv::Mat descriptors;
                reference_sift->detectAndCompute(raster(patch), mask(patch), points, descriptors);
                for (std::size_t i = 0; i < points.size(); ++i) {
                    auto point = points[i]; point.pt += cv::Point2f(static_cast<float>(patch.x), static_cast<float>(patch.y));
                    if (point.pt.x < cell.x || point.pt.y < cell.y || point.pt.x >= cell.x + cell.width || point.pt.y >= cell.y + cell.height) { continue; }
                    reference.points.push_back(point); reference.descriptors.push_back(descriptors.row(static_cast<int>(i)));
                }
            } }
        } else { detector->detectAndCompute(raster, mask, reference.points, reference.descriptors); }
    }
    std::vector<cv::KeyPoint> points; cv::Mat descriptors;
    detector->detectAndCompute(gray(crop), terrain_mask(crop, true), points, descriptors); cancelled(stop);
    LocalizationResult result; result.features = static_cast<int>(points.size()); result.reason = LocalizationReason::InsufficientFeatures;
    if (descriptors.rows < 12 || reference.descriptors.rows < 12) { return result; }
    cv::BFMatcher matcher(sift ? cv::NORM_L2 : cv::NORM_HAMMING); std::vector<std::vector<cv::DMatch>> pairs; std::vector<cv::DMatch> reverse;
    matcher.knnMatch(descriptors, reference.descriptors, pairs, 2); matcher.match(reference.descriptors, descriptors, reverse);
    std::vector<cv::Point2f> source, destination;
    for (const auto& pair : pairs) {
        if (pair.size() == 2 && (sift || pair[0].distance < 72) && pair[0].distance < (sift ? .70 : .75) * pair[1].distance
            && reverse[static_cast<std::size_t>(pair[0].trainIdx)].trainIdx == pair[0].queryIdx) {
            source.push_back(points[static_cast<std::size_t>(pair[0].queryIdx)].pt); destination.push_back(reference.points[static_cast<std::size_t>(pair[0].trainIdx)].pt);
        }
    }
    result.matches = static_cast<int>(source.size()); if (result.matches < 12) { return result; }
    const auto model = fit(source, destination, crop.size()); cancelled(stop);
    result.inliers = model.count; result.reprojection_rms = model.rms; result.scale = model.scale; result.rotation = model.angle;
    result.similarity = static_cast<double>(model.count) / result.matches; result.reason = LocalizationReason::InconsistentGeometry;
    if (!supported(model, result.matches)) {
        if (!verified_coast(model, result.matches, crop, config.reference)) { return result; }
        result.limited_support = true;
    }
    const auto [low, high] = std::minmax_element(config.scales.cbegin(), config.scales.cend());
    if (model.scale < *low || model.scale > *high || !QRectF(config.reference.rect()).contains(QPointF(model.center.x, model.center.y))
        || (!config.allow_rotation && std::none_of(config.rotations.cbegin(), config.rotations.cend(), [&](double angle) { return std::abs(std::remainder(model.angle - angle, 360.0)) <= 5; }))) { return result; }
    std::vector<cv::Point2f> other_source, other_destination;
    for (std::size_t i = 0; i < source.size(); ++i) { if (!model.inliers.at<uchar>(static_cast<int>(i))) { other_source.push_back(source[i]); other_destination.push_back(destination[i]); } }
    const auto alternative = fit(other_source, other_destination, crop.size());
    result.runner_up = static_cast<double>(alternative.count) / result.matches;
    if ((supported(alternative, static_cast<int>(other_source.size())) || verified_coast(alternative, static_cast<int>(other_source.size()), crop, config.reference))
        && alternative.count >= model.count * .8 && cv::norm(alternative.center - model.center) > 8) {
        result.reason = LocalizationReason::Ambiguous; return result;
    }
    const double units = std::max(config.units_per_pixel.width(), config.units_per_pixel.height());
    result.position = RecordedPosition{config.origin.x() + model.center.x * config.units_per_pixel.width(), config.origin.y() + model.center.y * config.units_per_pixel.height(),
        std::max(result.limited_support ? 6.0 : 4.0, model.rms * 3 + 1) * units, .95 + .04 * std::min(1.0, model.count / 40.0)};
    result.reason = LocalizationReason::Matched; return result;
}
LocalizationResult feature_localization(const QImage& image, const LocalizationConfig& config, std::stop_token stop) {
    auto result = feature_attempt(image, config, stop, false);
    if (!result.position && result.reason != LocalizationReason::Ambiguous) {
        cancelled(stop);
        auto fallback = feature_attempt(image, config, stop, true);
        if (fallback.position || fallback.reason == LocalizationReason::Ambiguous) { return fallback; }
    }
    return result;
}
} // namespace teyvat
