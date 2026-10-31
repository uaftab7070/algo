#include "frames/frame.hpp"

#include "common/hash.hpp"

#include <cmath>
#include <stdexcept>

namespace ahpv {
namespace {
struct FlagName {
    std::uint32_t bit;
    const char* name;
};
constexpr FlagName kFlagNames[] = {
    {kFlagCorrupted, "corrupted"},
    {kFlagSequenceGap, "sequence_gap"},
    {kFlagLate, "late"},
    {kFlagTimestampRegression, "timestamp_regression"},
    {kFlagRepeatedContent, "repeated_content"},
    {kFlagLowContrast, "low_contrast"},
    {kFlagVeryDark, "very_dark"},
    {kFlagVeryBright, "very_bright"},
    {kFlagAfterDiscontinuity, "after_discontinuity"},
    {kFlagAfterNoSignal, "after_no_signal"},
    {kFlagFaultInjected, "fault_injected"},
};
}  // namespace

const char* pixel_format_name(PixelFormat format) {
    switch (format) {
        case PixelFormat::Gray8: return "gray8";
        case PixelFormat::I420: return "i420";
        case PixelFormat::I444: return "i444";
    }
    return "gray8";
}

PixelFormat parse_pixel_format(const std::string& name) {
    if (name == "gray8") return PixelFormat::Gray8;
    if (name == "i420") return PixelFormat::I420;
    if (name == "i444") return PixelFormat::I444;
    throw std::invalid_argument("unknown pixel format '" + name + "'");
}

std::size_t FrameGeometry::chroma_plane_bytes() const {
    switch (format) {
        case PixelFormat::Gray8: return 0;
        case PixelFormat::I420:
            return static_cast<std::size_t>((width + 1) / 2) * static_cast<std::size_t>((height + 1) / 2);
        case PixelFormat::I444: return luma_bytes();
    }
    return 0;
}

std::size_t FrameGeometry::frame_bytes() const { return luma_bytes() + 2 * chroma_plane_bytes(); }

json::Value flag_names(std::uint32_t flags) {
    json::Array names;
    for (const auto& entry : kFlagNames)
        if (flags & entry.bit) names.emplace_back(entry.name);
    return json::Value(std::move(names));
}

std::vector<std::string> flag_name_list(std::uint32_t flags) {
    std::vector<std::string> names;
    for (const auto& entry : kFlagNames)
        if (flags & entry.bit) names.emplace_back(entry.name);
    return names;
}

std::uint32_t parse_flag_names(const json::Value& names) {
    std::uint32_t flags = 0;
    for (const auto& item : names.as_array()) {
        bool known = false;
        for (const auto& entry : kFlagNames) {
            if (item.as_string() == entry.name) {
                flags |= entry.bit;
                known = true;
            }
        }
        if (!known) throw std::invalid_argument("unknown frame flag '" + item.as_string() + "'");
    }
    return flags;
}

void SequenceTimingAnalyzer::observe(Frame& frame) {
    if (discontinuity_) frame.flags |= kFlagAfterDiscontinuity;
    if (after_no_signal_) frame.flags |= kFlagAfterNoSignal;
    discontinuity_ = after_no_signal_ = false;
    const bool same_epoch = has_previous_ && previous_epoch_ == frame.source_epoch;
    if (same_epoch) {
        if (frame.has_source_sequence && previous_has_sequence_) {
            if (frame.source_sequence > previous_sequence_ + step_) {
                frame.flags |= kFlagSequenceGap;
                frame.missing_before = frame.source_sequence - previous_sequence_ - step_;
            } else if (frame.source_sequence <= previous_sequence_) {
                frame.flags |= kFlagTimestampRegression;
            }
        }
        frame.interval_ns = frame.source_timestamp_ns - previous_source_ns_;
        if (frame.interval_ns <= 0) {
            frame.flags |= kFlagTimestampRegression;
        } else if (nominal_interval_ns_ > 0 &&
                   static_cast<double>(frame.interval_ns) > late_factor_ * static_cast<double>(nominal_interval_ns_)) {
            frame.flags |= kFlagLate;
            // Without source sequence numbers, a long interval is the only gap evidence.
            if (!frame.has_source_sequence) frame.flags |= kFlagSequenceGap;
        }
    }
    has_previous_ = true;
    previous_epoch_ = frame.source_epoch;
    previous_has_sequence_ = frame.has_source_sequence;
    previous_sequence_ = frame.source_sequence;
    previous_source_ns_ = frame.source_timestamp_ns;
}

QualityStats luma_statistics(const Frame& frame) {
    QualityStats stats;
    const int w = frame.geometry.width, h = frame.geometry.height;
    if (frame.data.size() < frame.geometry.luma_bytes() || w <= 0 || h <= 0) return stats;
    // A 4-pixel grid keeps this affordable on a single Cortex-A7 core.
    const int step = (w >= 64 && h >= 64) ? 4 : 1;
    double sum = 0, sum_sq = 0;
    std::uint64_t count = 0;
    for (int y = 0; y < h; y += step) {
        const std::uint8_t* row = frame.data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = 0; x < w; x += step) {
            const double v = row[x];
            sum += v;
            sum_sq += v * v;
            ++count;
        }
    }
    stats.luma_mean = sum / static_cast<double>(count);
    stats.luma_std = std::sqrt(std::max(0.0, sum_sq / static_cast<double>(count) - stats.luma_mean * stats.luma_mean));
    return stats;
}

QualityStats ContentAnalyzer::analyze(Frame& frame, bool compute_crc) {
    QualityStats stats = luma_statistics(frame);
    if (compute_crc) {
        stats.crc32 = crc32(frame.data.data(), frame.data.size());
        if (has_previous_ && stats.crc32 == previous_crc_) frame.flags |= kFlagRepeatedContent;
        previous_crc_ = stats.crc32;
        has_previous_ = true;
    }
    if (stats.luma_std < thresholds_.low_contrast_std) frame.flags |= kFlagLowContrast;
    if (stats.luma_mean < thresholds_.very_dark_mean) frame.flags |= kFlagVeryDark;
    if (stats.luma_mean > thresholds_.very_bright_mean) frame.flags |= kFlagVeryBright;
    return stats;
}

}  // namespace ahpv
