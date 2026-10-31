#include "rv1106_tracker/nanotrack_decoder.hpp"

// Native port of precision_tracker.NanoMatcher's NanoTrack v2 decoder.
// Tensor/decoder references: models/nanotrack/README.md and UPSTREAM_LICENSE.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rv1106::nano {
namespace {
constexpr int side = 16;
constexpr int count = side * side;

std::size_t add(std::size_t a, std::size_t b) {
    if (a > std::numeric_limits<std::size_t>::max() - b)
        throw std::invalid_argument("Tensor stride overflow");
    return a + b;
}
std::size_t mul(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b)
        throw std::invalid_argument("Tensor stride overflow");
    return a * b;
}

class Reader {
public:
    Reader(const TensorView& view, int channels) : view_(view) {
        if (!view.data || view.channels != channels || view.height != side || view.width != side)
            throw std::invalid_argument("Expected a batch-one NanoTrack 16x16 head tensor");
        switch (view.type) {
            case ElementType::Float32: bytes_ = sizeof(float); break;
            case ElementType::Int8: bytes_ = 1; break;
            case ElementType::UInt8: bytes_ = 1; break;
            default: throw std::invalid_argument("Unsupported tensor element type");
        }
        if (view.type != ElementType::Float32 &&
            (!std::isfinite(view.scale) || view.scale <= 0 ||
             (view.type == ElementType::Int8 && (view.zero_point < -128 || view.zero_point > 127)) ||
             (view.type == ElementType::UInt8 && (view.zero_point < 0 || view.zero_point > 255))))
            throw std::invalid_argument("Invalid affine quantization parameters");
        if (view.layout == Layout::NCHW) {
            if (view.pixel_stride != 0 && view.pixel_stride != 1)
                throw std::invalid_argument("NCHW pixel stride must be one");
            pixel_ = 1;
            row_ = view.row_stride ? view.row_stride : side;
            channel_ = view.plane_stride ? view.plane_stride : mul(side, row_);
            if (row_ < side || channel_ < mul(side, row_))
                throw std::invalid_argument("Overlapping NCHW tensor strides");
        } else if (view.layout == Layout::NHWC) {
            if (view.plane_stride != 0 && view.plane_stride != 1)
                throw std::invalid_argument("NHWC channel stride must be one");
            channel_ = 1;
            pixel_ = view.pixel_stride ? view.pixel_stride : static_cast<std::size_t>(channels);
            row_ = view.row_stride ? view.row_stride : mul(side, pixel_);
            if (pixel_ < static_cast<std::size_t>(channels) || row_ < mul(side, pixel_))
                throw std::invalid_argument("Overlapping NHWC tensor strides");
        } else {
            throw std::invalid_argument("Unsupported tensor layout");
        }
        const auto last = add(add(mul(channels - 1, channel_), mul(side - 1, row_)), mul(side - 1, pixel_));
        if (mul(add(last, 1), bytes_) > view.byte_size)
            throw std::invalid_argument("Tensor buffer is shorter than its declared strides");
    }

    double at(int c, int y, int x) const {
        const auto index = static_cast<std::size_t>(c) * channel_ +
                           static_cast<std::size_t>(y) * row_ + static_cast<std::size_t>(x) * pixel_;
        const auto* source = static_cast<const unsigned char*>(view_.data) + index * bytes_;
        double value;
        if (view_.type == ElementType::Float32) {
            float f;
            std::memcpy(&f, source, sizeof(f)); // Runtime buffers need not be float-aligned.
            value = f;
        } else {
            int raw;
            if (view_.type == ElementType::Int8) {
                std::int8_t s;
                std::memcpy(&s, source, 1);
                raw = s;
            } else {
                raw = *source;
            }
            value = (raw - view_.zero_point) * static_cast<double>(view_.scale);
        }
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite model output");
        return value;
    }

private:
    const TensorView& view_;
    std::size_t bytes_{0}, channel_{0}, row_{0}, pixel_{0};
};

struct Cell {
    double x1, y1, x2, y2, width, height, score, penalty;
};

double padded_size(double w, double h) {
    const double padding = (w + h) * 0.5;
    return std::sqrt((w + padding) * (h + padding));
}
double change(double ratio) { return std::max(ratio, 1.0 / std::max(ratio, 1e-6)); }

const std::array<float, count>& window() {
    static const auto value = [] {
        std::array<float, count> values{};
        constexpr double pi = 3.14159265358979323846;
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
                values[y * side + x] = static_cast<float>(
                    (0.5 - 0.5 * std::cos(2 * pi * x / (side - 1))) *
                    (0.5 - 0.5 * std::cos(2 * pi * y / (side - 1))));
        return values;
    }();
    return value;
}

float finite_float(double value) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        throw std::invalid_argument("Nonfinite decoded geometry");
    return static_cast<float>(value);
}
} // namespace

Candidate decode(const TensorView& logits, const TensorView& distances,
                 const Box& anchor, float expansion) {
    for (float value : {anchor.x, anchor.y, anchor.width, anchor.height, expansion})
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite anchor geometry");
    if (anchor.width <= 0 || anchor.height <= 0 || expansion <= 0)
        throw std::invalid_argument("Anchor extent and expansion must be positive");
    const Reader cls(logits, 2), dist(distances, 4);
    const double search_side = padded_size(anchor.width, anchor.height) * (255.0 / 127.0) * expansion;
    const double scale = 255.0 / search_side;
    const double anchor_size = padded_size(anchor.width * scale, anchor.height * scale);
    std::array<Cell, count> cells{};
    int best = 0;
    double best_rank = -1;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const int i = y * side + x;
            auto& c = cells[i];
            const double difference = std::clamp(cls.at(0, y, x) - cls.at(1, y, x), -50.0, 50.0);
            c.score = 1.0 / (1.0 + std::exp(difference));
            const double gx = (x - 8) * 16 + 127, gy = (y - 8) * 16 + 127;
            c.x1 = gx - dist.at(0, y, x); c.y1 = gy - dist.at(1, y, x);
            c.x2 = gx + dist.at(2, y, x); c.y2 = gy + dist.at(3, y, x);
            c.width = std::max(1.0, c.x2 - c.x1); c.height = std::max(1.0, c.y2 - c.y1);
            const double sc = change(padded_size(c.width, c.height) / anchor_size);
            const double rc = change((static_cast<double>(anchor.width) / anchor.height) / (c.width / c.height));
            c.penalty = std::exp(-(sc * rc - 1) * 0.055);
            const double rank = c.penalty * c.score * 0.70 + window()[i] * 0.30;
            if (rank > best_rank) { best = i; best_rank = rank; }
        }
    }
    const auto& peak = cells[best];
    const int py = best / side, px = best % side;
    double sum = 0, square_sum = 0, second = 0;
    int samples = 0;
    for (int i = 0; i < count; ++i) {
        const auto& c = cells[i];
        if (std::abs(i / side - py) > 2 || std::abs(i % side - px) > 2) {
            sum += c.score; square_sum += c.score * c.score; ++samples;
        }
        const double intersection = std::max(0.0, std::min(c.x2, peak.x2) - std::max(c.x1, peak.x1)) *
                                    std::max(0.0, std::min(c.y2, peak.y2) - std::max(c.y1, peak.y1));
        const double total = c.width * c.height + peak.width * peak.height - intersection;
        if (intersection / std::max(1.0, total) < 0.25) second = std::max(second, c.score);
    }
    const double mean = sum / samples;
    const double sd = std::sqrt(std::max(0.0, square_sum / samples - mean * mean));
    const double lr = peak.penalty * peak.score * 0.32;
    const double w = peak.width / scale * lr + anchor.width * (1 - lr);
    const double h = peak.height / scale * lr + anchor.height * (1 - lr);
    const double cx = anchor.x + anchor.width * 0.5 + ((peak.x1 + peak.x2) * 0.5 - 127) / scale;
    const double cy = anchor.y + anchor.height * 0.5 + ((peak.y1 + peak.y2) * 0.5 - 127) / scale;
    Candidate result;
    result.box = {finite_float(cx - w * 0.5), finite_float(cy - h * 0.5), finite_float(w), finite_float(h)};
    result.score = finite_float(peak.score);
    result.psr = finite_float((peak.score - mean) / std::max(0.025, sd));
    result.ambiguous = second > 0.85 && peak.score - second < 0.035;
    result.peak_x = px; result.peak_y = py;
    return result;
}
} // namespace rv1106::nano
