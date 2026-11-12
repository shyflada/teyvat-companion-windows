#include "teyvat/walk_surface.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace teyvat {
namespace {
using Point = SurfacePoint;
constexpr double epsilon = 1e-7;
Point minus(Point a, Point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Point cross(Point a, Point b) { return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }
double dot(Point a, Point b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
bool finite(double value) { return std::isfinite(value) && std::abs(value) <= 1e6; }

struct Triangle {
    std::array<Point, 3> points;
    bool support;
    double projected_area;
    [[nodiscard]] std::array<double, 3> weights(double x, double z) const {
        const auto a = minus(points[1], points[0]);
        const auto b = minus(points[2], points[0]);
        const auto p = Point{x - points[0].x, 0, z - points[0].z};
        const auto u = (p.x*b.z - p.z*b.x) / projected_area;
        const auto v = (a.x*p.z - a.z*p.x) / projected_area;
        return {1 - u - v, u, v};
    }
    [[nodiscard]] double height(const std::array<double, 3>& w) const {
        return w[0]*points[0].y + w[1]*points[1].y + w[2]*points[2].y;
    }
};
struct Interval { double first = 0, last = 1; };
bool positive(Interval& interval, double a, double b) {
    // Keep the part of [0,1] where the line through a and b is nonnegative.
    if (a < 0 && b < 0) { return false; }
    if (a < 0) { interval.first = std::max(interval.first, -a / (b - a)); }
    if (b < 0) { interval.last = std::min(interval.last, a / (a - b)); }
    return interval.first <= interval.last + epsilon;
}

bool triangle_hits_box(std::array<Point, 3> points, Point center, Point half) {
    for (auto& p : points) { p = minus(p, center); }
    const std::array axes{Point{1, 0, 0}, Point{0, 1, 0}, Point{0, 0, 1}};
    const std::array edges{minus(points[1], points[0]), minus(points[2], points[1]), minus(points[0], points[2])};
    const auto separates = [&](Point axis) {
        if (dot(axis, axis) < 1e-18) { return false; }
        const auto extent = std::abs(axis.x)*half.x + std::abs(axis.y)*half.y + std::abs(axis.z)*half.z;
        const auto [low, high] = std::minmax({dot(axis, points[0]), dot(axis, points[1]), dot(axis, points[2])});
        return low > extent + epsilon || high < -extent - epsilon;
    };
    if (separates(cross(edges[0], edges[1]))) { return false; }
    for (const auto axis : axes) {
        if (separates(axis)) { return false; }
        for (const auto edge : edges) {
            if (separates(cross(axis, edge))) { return false; }
        }
    }
    return true;
}

// A uniform index limits each support and clearance query to nearby triangles.
class CollisionIndex {
public:
    CollisionIndex(std::span<const CollisionFace> faces, const WalkBakeSettings& settings)
        : origin_x_(settings.min_x - 2*settings.radius), origin_z_(settings.min_z - 2*settings.radius),
          width_(bucket(settings.max_x + 2*settings.radius, origin_x_) + 1),
          height_(bucket(settings.max_z + 2*settings.radius, origin_z_) + 1), buckets_(width_*height_) {
        const auto slope_cosine = std::cos(settings.max_slope_degrees * std::numbers::pi / 180);
        triangles_.reserve(faces.size());
        std::size_t references = 0;
        for (const auto& face : faces) {
            for (const auto p : face.corners) {
                if (!finite(p.x) || !finite(p.y) || !finite(p.z)) { throw std::invalid_argument("Nonfinite collision face"); }
            }
            const auto normal = cross(minus(face.corners[1], face.corners[0]), minus(face.corners[2], face.corners[0]));
            const double size = std::sqrt(dot(normal, normal));
            if (size <= epsilon) { continue; }
            const auto [a, b, c] = face.corners;
            const auto [min_x, max_x] = std::minmax({a.x, b.x, c.x});
            const auto [min_z, max_z] = std::minmax({a.z, b.z, c.z});
            const auto index = static_cast<std::uint32_t>(triangles_.size());
            triangles_.push_back({face.corners, face.supports_walking && normal.y >= slope_cosine*size, -normal.y});
            visit(min_x, max_x, min_z, max_z, [&](auto& bin) {
                if (++references > 12000000) { throw std::invalid_argument("Collision index exceeds the tile budget"); }
                bin.push_back(index);
            });
        }
    }
    [[nodiscard]] std::vector<double> heights(Point point) const {
        std::vector<double> result;
        for (const auto index : buckets_[clamped(point.z, origin_z_, height_)*width_ + clamped(point.x, origin_x_, width_)]) {
            const auto& triangle = triangles_[index];
            if (!triangle.support) { continue; }
            const auto w = triangle.weights(point.x, point.z);
            if (*std::min_element(w.begin(), w.end()) >= -epsilon) { result.push_back(triangle.height(w)); }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end(), [](double a, double b) { return std::abs(a-b) < .015; }), result.end());
        return result;
    }
    [[nodiscard]] bool supported(Point a, Point b, double step) const {
        std::vector<Interval> support;
        visit(std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.z, b.z), std::max(a.z, b.z), [&](const auto& bin) {
            for (const auto index : bin) {
                const auto& triangle = triangles_[index];
                if (!triangle.support) { continue; }
                const auto w = triangle.weights(a.x, a.z), v = triangle.weights(b.x, b.z);
                Interval interval;
                if (!positive(interval, w[0] + epsilon, v[0] + epsilon)
                    || !positive(interval, w[1] + epsilon, v[1] + epsilon)
                    || !positive(interval, w[2] + epsilon, v[2] + epsilon)) { continue; }
                const auto da = triangle.height(w) - a.y, db = triangle.height(v) - b.y;
                if (positive(interval, step + da, step + db) && positive(interval, step - da, step - db)) { support.push_back(interval); }
            }
        });
        std::sort(support.begin(), support.end(), [](auto a, auto b) { return a.first < b.first; });
        double covered = 0;
        for (const auto interval : support) {
            if (interval.first > covered + epsilon) { return false; }
            covered = std::max(covered, interval.last);
            if (covered >= 1 - epsilon) { return true; }
        }
        return false;
    }
    [[nodiscard]] bool clear(Point a, Point b, const WalkBakeSettings& s) const {
        const auto length = std::hypot(b.x - a.x, b.z - a.z);
        const auto ux = length > epsilon ? (b.x - a.x) / length : 1.;
        const auto uz = length > epsilon ? (b.z - a.z) / length : 0.;
        const auto rise = length > epsilon ? (b.y - a.y) / length : 0.;
        const Point center{length / 2, (s.height + s.step) / 2, 0};
        const Point half{length / 2 + s.radius, (s.height - s.step) / 2, s.radius};
        const auto margin = s.radius*std::numbers::sqrt2;
        bool clear = true;
        visit(std::min(a.x, b.x) - margin, std::max(a.x, b.x) + margin,
            std::min(a.z, b.z) - margin, std::max(a.z, b.z) + margin, [&](const auto& bin) {
                if (!clear) { return; }
                for (const auto index : bin) {
                    auto points = triangles_[index].points;
                    for (auto& p : points) {
                        const auto d = minus(p, a);
                        const auto along = d.x*ux + d.z*uz;
                        p = {along, d.y - rise*along, -d.x*uz + d.z*ux};
                    }
                    if (triangle_hits_box(points, center, half)) { clear = false; break; }
                }
            });
        return clear;
    }
private:
    static int bucket(double value, double origin) { return static_cast<int>(std::floor((value - origin)/8.)); }
    static int clamped(double value, double origin, int count) { return std::clamp(bucket(value, origin), 0, count - 1); }
    template<class F> void visit(double x0, double x1, double z0, double z1, F use) {
        for (int z = clamped(z0, origin_z_, height_); z <= clamped(z1, origin_z_, height_); ++z) {
            for (int x = clamped(x0, origin_x_, width_); x <= clamped(x1, origin_x_, width_); ++x) { use(buckets_[z*width_ + x]); }
        }
    }
    template<class F> void visit(double x0, double x1, double z0, double z1, F use) const {
        for (int z = clamped(z0, origin_z_, height_); z <= clamped(z1, origin_z_, height_); ++z) {
            for (int x = clamped(x0, origin_x_, width_); x <= clamped(x1, origin_x_, width_); ++x) { use(buckets_[z*width_ + x]); }
        }
    }
    double origin_x_, origin_z_;
    int width_, height_;
    std::vector<std::vector<std::uint32_t>> buckets_;
    std::vector<Triangle> triangles_;
};
}

WalkSurface bake_walk_surface(std::span<const CollisionFace> faces, const WalkBakeSettings& s, std::stop_token stop) {
    for (const auto value : {s.min_x, s.max_x, s.min_z, s.max_z, s.spacing, s.radius, s.height, s.step, s.max_slope_degrees, s.metres_per_second}) {
        if (!finite(value)) { throw std::invalid_argument("Nonfinite walk surface settings"); }
    }
    if (faces.size() > 1000000 || s.spacing < .2 || s.spacing > 8 || s.radius <= 0 || s.radius > 3
        || s.height <= s.step || s.height > 8 || s.step < 0 || s.step > 1 || s.max_slope_degrees <= 0 || s.max_slope_degrees >= 75
        || s.metres_per_second <= 0 || s.metres_per_second > 30 || s.max_x <= s.min_x || s.max_z <= s.min_z
        || s.scene.empty() || s.floor.empty() || s.scene.size() > 128 || s.floor.size() > 128) {
        throw std::invalid_argument("Invalid walk surface settings");
    }
    const auto columns = static_cast<int>(std::floor((s.max_x - s.min_x) / s.spacing));
    const auto rows = static_cast<int>(std::floor((s.max_z - s.min_z) / s.spacing));
    if (columns < 1 || rows < 1 || static_cast<long long>(columns)*rows > 250000) {
        throw std::invalid_argument("Bake a smaller navigation tile");
    }
    if (stop.stop_requested()) { return {}; }
    const CollisionIndex index(faces, s);
    WalkSurface result;
    std::vector<std::vector<std::uint32_t>> cells(columns*rows);
    const auto add = [&](std::uint32_t a, std::uint32_t b) {
        const auto& u = result.nodes[a]; const auto& v = result.nodes[b];
        const Point p{u.x, u.y, u.z}, q{v.x, v.y, v.z};
        const auto slope = std::tan(s.max_slope_degrees * std::numbers::pi / 180);
        if (std::abs(u.y - v.y) > s.step + s.spacing*slope || !index.supported(p, q, s.step) || !index.clear(p, q, s)) { return; }
        const double time = std::hypot(u.x - v.x, u.y - v.y, u.z - v.z) / s.metres_per_second;
        if (result.passages.size() >= 1999999) { throw std::invalid_argument("Too many walk passages in one tile"); }
        result.passages.push_back({a, b, Traversal::Walk, PassageState::Open, time});
        result.passages.push_back({b, a, Traversal::Walk, PassageState::Open, time});
    };
    for (int z = 0; z < rows; ++z) {
        if (stop.stop_requested()) { return {}; }
        for (int x = 0; x < columns; ++x) {
            const auto cell = z*columns + x;
            Point point{s.min_x + (x + .5)*s.spacing, 0, s.min_z + (z + .5)*s.spacing};
            for (const auto y : index.heights(point)) {
                point.y = y;
                if (!index.clear(point, point, s)) { continue; }
                if (result.nodes.size() >= 500000) { throw std::invalid_argument("Too many surfaces in one tile"); }
                const auto node = static_cast<std::uint32_t>(result.nodes.size());
                result.nodes.push_back({point.x, y, point.z, s.scene, s.floor});
                cells[cell].push_back(node);
                if (x > 0) { for (const auto neighbour : cells[cell - 1]) { add(neighbour, node); } }
                if (z > 0) { for (const auto neighbour : cells[cell - columns]) { add(neighbour, node); } }
            }
        }
    }
    return result;
}
} // namespace teyvat
