#pragma once
// Capture adapter interface. Adapters only report frames they actually
// obtained; no adapter may synthesise a substitute frame on timeout or error.
#include "common/json.hpp"
#include "frames/frame.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ahpv {

struct SourceInfo {
    std::string kind;              // y4m_file | y4m_stream | v4l2
    std::string uri;
    bool live{false};
    std::string content_origin;    // camera | recorded_file | synthetic_fixture | unknown_stream
    FrameGeometry geometry;
    int fps_num{0};
    int fps_den{1};
    std::string timestamp_basis;
    std::vector<std::string> transformations;  // Applied before frames leave the adapter.
    json::Value requested;         // What configuration asked for.
    json::Value effective;         // What the device/file actually reports.
    json::Value details;

    std::int64_t nominal_interval_ns() const;
    json::Value to_json() const;
};

enum class ReadStatus {
    Frame,         // out holds a valid frame.
    Corrupted,     // out holds metadata of a frame that must be rejected.
    NoSignal,      // Nothing arrived within the no-signal timeout.
    EndOfStream,   // Clean end of a finite file.
    Disconnected,  // Device/stream went away; reopen may be possible.
    Error,         // Unrecoverable for this source (malformed or truncated input).
};
const char* read_status_name(ReadStatus status);

struct ReadResult {
    ReadStatus status{ReadStatus::Error};
    std::string message;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual SourceInfo info() const = 0;
    virtual ReadResult read(Frame& out) = 0;
    virtual bool reopenable() const { return false; }
    // Throws std::runtime_error when the source cannot be reopened.
    virtual void reopen() { throw std::runtime_error("source cannot be reopened"); }
};

struct SourceOptions {
    std::string kind;
    std::string path;
    int no_signal_timeout_ms{2000};
    bool realtime{false};
    // V4L2 request (verified against the driver's reply; see v4l2_source.cpp).
    int width{0};
    int height{0};
    std::string pixel_format{"NV12"};
    int fps{0};
    int buffers{4};
};

std::unique_ptr<FrameSource> open_source(const SourceOptions& options);

}  // namespace ahpv
