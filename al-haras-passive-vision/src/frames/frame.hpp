#pragma once
// Frame model. Acquisition time (when this process obtained the frame) and the
// source's own timestamp are separate fields with an explicit basis; display or
// processing time is never written into either.
#include "common/json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ahpv {

enum class PixelFormat { Gray8, I420, I444 };

const char* pixel_format_name(PixelFormat format);
PixelFormat parse_pixel_format(const std::string& name);

struct FrameGeometry {
    int width{0};
    int height{0};
    PixelFormat format{PixelFormat::Gray8};
    std::size_t luma_bytes() const { return static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }
    std::size_t chroma_plane_bytes() const;
    std::size_t frame_bytes() const;
    bool valid() const { return width > 0 && height > 0 && width <= 8192 && height <= 8192; }
    bool operator==(const FrameGeometry& o) const { return width == o.width && height == o.height && format == o.format; }
    bool operator!=(const FrameGeometry& o) const { return !(*this == o); }
};

// Bit flags recorded per frame. Names are the JSON vocabulary (docs/data-schemas.md).
enum FrameFlag : std::uint32_t {
    kFlagCorrupted = 1u << 0,
    kFlagSequenceGap = 1u << 1,
    kFlagLate = 1u << 2,
    kFlagTimestampRegression = 1u << 3,
    kFlagRepeatedContent = 1u << 4,
    kFlagLowContrast = 1u << 5,
    kFlagVeryDark = 1u << 6,
    kFlagVeryBright = 1u << 7,
    kFlagAfterDiscontinuity = 1u << 8,
    kFlagAfterNoSignal = 1u << 9,
    kFlagFaultInjected = 1u << 10,
};
json::Value flag_names(std::uint32_t flags);
// By value, so it is safe as a range-for initializer.
std::vector<std::string> flag_name_list(std::uint32_t flags);
std::uint32_t parse_flag_names(const json::Value& names);

struct Frame {
    std::uint64_t frame_id{0};          // Assigned by the pipeline; unique and monotonic per run.
    std::uint64_t source_epoch{0};      // Incremented when the source is reopened.
    bool has_source_sequence{false};
    std::uint64_t source_sequence{0};   // Source-native counter (V4L2 sequence, Y4M frame index).
    std::int64_t acquisition_mono_ns{0};
    std::int64_t acquisition_wall_ns{0};
    std::int64_t source_timestamp_ns{0};
    FrameGeometry geometry;
    std::vector<std::uint8_t> data;     // Planar Y[, U, V].
    std::uint32_t flags{0};
    std::uint64_t missing_before{0};    // Source frames known missing immediately before this one.
    std::int64_t interval_ns{-1};       // Source-timestamp interval to the previous frame.
    std::string corruption_reason;
};

struct QualityStats {
    double luma_mean{0};
    double luma_std{0};
    std::uint32_t crc32{0};
};

struct ValidationThresholds {
    double late_factor{1.5};
    double very_dark_mean{16};
    double very_bright_mean{240};
    double low_contrast_std{2.0};
};

// Runs on the capture side in arrival order, before any queue drop, so source
// gaps are never confused with frames this application discarded.
class SequenceTimingAnalyzer {
public:
    // sequence_step > 1 when the pipeline keeps 1 of every N delivered frames;
    // the expected sequence step and interval scale with it, so configured
    // decimation is never reported as loss.
    SequenceTimingAnalyzer(std::int64_t nominal_interval_ns, double late_factor, std::uint64_t sequence_step = 1)
        : nominal_interval_ns_(nominal_interval_ns * static_cast<std::int64_t>(sequence_step)),
          late_factor_(late_factor), step_(sequence_step) {}
    void observe(Frame& frame);
    void mark_discontinuity() { discontinuity_ = true; }
    void mark_no_signal() { after_no_signal_ = true; }

private:
    std::int64_t nominal_interval_ns_;
    double late_factor_;
    std::uint64_t step_;
    bool has_previous_{false};
    std::uint64_t previous_epoch_{0};
    bool previous_has_sequence_{false};
    std::uint64_t previous_sequence_{0};
    std::int64_t previous_source_ns_{0};
    bool discontinuity_{false};
    bool after_no_signal_{false};
};

// Runs on the consumer side: checksum and coarse quality observations.
// Quality flags describe the image; they never make a frame invalid.
class ContentAnalyzer {
public:
    explicit ContentAnalyzer(ValidationThresholds thresholds) : thresholds_(thresholds) {}
    QualityStats analyze(Frame& frame, bool compute_crc);

private:
    ValidationThresholds thresholds_;
    bool has_previous_{false};
    std::uint32_t previous_crc_{0};
};

QualityStats luma_statistics(const Frame& frame);

}  // namespace ahpv
