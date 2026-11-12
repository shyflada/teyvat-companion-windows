#include "teyvat/detection.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace teyvat {
OpeningCues opening_scene_cues(const QImage& input, bool measure_ring) {
    OpeningCues cues;
    if (input.isNull() || input.width() < 128 || input.height() < 128 || input.width() > 640 || input.height() > 640) { return cues; }
    const auto image = input.convertToFormat(QImage::Format_ARGB32).scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    cv::Mat gold(256, 256, CV_8UC1, cv::Scalar(0)), white = gold.clone(); int ng = 0, nb = 0;
    for (int y = 0; y < 256; ++y) { const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < 256; ++x) { const auto c = row[x];
            if (qRed(c) > 225 && qGreen(c) > 185 && qRed(c) > qBlue(c) * 1.08) { gold.at<uchar>(y, x) = 255; ++ng; }
            if (qRed(c) > 225 && qGreen(c) > 225 && qBlue(c) > 225) { white.at<uchar>(y, x) = 255; }
            if (x > 128 && y > 64 && qBlue(c) > 160 && qGreen(c) > 130 && qBlue(c) > qRed(c) * 1.35 && qGreen(c) > qRed(c) * 1.2) { ++nb; }
        }
    }
    cues.gold = ng / 65536.; cues.blue = nb / 65536.;
    std::vector<std::vector<cv::Point>> contours; cv::findContours(white, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    for (const auto& c : contours) { const auto b = cv::boundingRect(c); const double area = cv::contourArea(c);
        if (b.x > 175 && b.y > 30 && b.y < 210 && b.width >= 15 && b.width <= 45 && b.height >= 12 && b.height <= 32
            && b.width > b.height && area > b.area() * .65) { cues.interaction = true; }
    }
    if (!measure_ring) { return cues; }
    cv::Mat blur; cv::GaussianBlur(gold, blur, {5, 5}, 1); std::vector<cv::Vec3f> circles;
    cv::HoughCircles(blur, circles, cv::HOUGH_GRADIENT, 1, 30, 80, 18, 75, 145);
    for (const auto& c : circles) {
        if (c[0] < 40 || c[0] > 220 || c[1] < 50 || c[1] > 245) { continue; }
        int visible = 0, hits = 0;
        for (int a = 0; a < 180; ++a) { const double t = a * std::numbers::pi / 90;
            const int x = qRound(c[0] + c[2] * std::cos(t)), y = qRound(c[1] + c[2] * std::sin(t));
            if (x < 3 || y < 3 || x >= 253 || y >= 253) { continue; } ++visible; bool found = false;
            for (int dy = -2; dy <= 2; ++dy) { for (int dx = -2; dx <= 2; ++dx) { found |= gold.at<uchar>(y + dy, x + dx) > 0; } }
            if (found) { ++hits; }
        }
        if (visible >= 70) { cues.ring = std::max(cues.ring, static_cast<double>(hits) / visible); }
    }
    return cues;
}
void ChestAnimationTracker::reset() {
    gold_history_.clear(); blue_history_.clear(); previous_ = interaction_at_ = burst_at_ = -1;
    interaction_count_ = dissolve_count_ = 0; blue_baseline_ = ring_ = gold_ = 0; blue_rise_ = 1;
    armed_position_.reset(); stable_ = true; latched_ = false;
}
std::optional<OpeningEvidence> ChestAnimationTracker::push(const QImage& scene, qint64 time, const std::optional<RecordedPosition>& position) {
    if (time < 0 || time <= previous_) { throw std::invalid_argument("Animation samples require increasing timestamps."); }
    if (previous_ >= 0 && time - previous_ > 1000) { reset(); } previous_ = time;
    if (!position || !std::isfinite(position->x) || !std::isfinite(position->y)
        || !std::isfinite(position->confidence) || position->confidence < .95 || position->confidence > 1
        || !std::isfinite(position->uncertainty) || position->uncertainty < 0 || position->uncertainty > 5 || scene.isNull()) { reset(); return {}; }
    auto cues = opening_scene_cues(scene);
    const auto average = [](const QVector<double>& values) { return values.isEmpty() ? 0. : std::accumulate(values.cbegin(), values.cend(), 0.) / values.size(); };
    if (burst_at_ < 0) {
        if (interaction_count_ >= 2 && time - interaction_at_ <= 1000 && cues.gold >= .12 && cues.gold - average(gold_history_) >= .08) {
            cues = opening_scene_cues(scene, true);
            if (cues.ring >= .70) { burst_at_ = time; ring_ = cues.ring; gold_ = cues.gold; blue_baseline_ = average(blue_history_); }
        }
        if (burst_at_ < 0 && cues.interaction) {
            if (time - interaction_at_ > 1000) { interaction_count_ = 0; }
            ++interaction_count_; interaction_at_ = time; armed_position_ = position;
        }
        gold_history_.append(cues.gold); blue_history_.append(cues.blue);
        if (gold_history_.size() > 6) { gold_history_.removeFirst(); blue_history_.removeFirst(); }
    }
    if (burst_at_ < 0 || latched_) { return {}; }
    stable_ &= armed_position_ && std::hypot(position->x - armed_position_->x, position->y - armed_position_->y) <= 2;
    if (time - burst_at_ > 5000) { reset(); return {}; }
    if (time <= burst_at_ || cues.blue - blue_baseline_ < .02) { dissolve_count_ = 0; blue_rise_ = 1; return {}; }
    blue_rise_ = std::min(blue_rise_, cues.blue - blue_baseline_);
    if (++dissolve_count_ < 2) { return {}; }
    latched_ = true; OpeningEvidence event; event.closed_ms = interaction_at_; event.opened_ms = time;
    event.position = position; event.position_stable = stable_;
    event.animation = OpeningAnimationProof{burst_at_, time, ring_, gold_, blue_rise_, interaction_count_, dissolve_count_}; return event;
}
} // namespace teyvat
