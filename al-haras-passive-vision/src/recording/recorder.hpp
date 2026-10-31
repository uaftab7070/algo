#pragma once
// Writes one session: Y4M payload, per-frame index, event log and manifest.
// A storage guard stops new writes before the free-space reserve is consumed so
// the session can still be finalized and existing sessions are never touched.
#include "capture/source.hpp"
#include "common/fsutil.hpp"
#include "common/json.hpp"
#include "frames/frame.hpp"
#include "recording/session.hpp"

#include <cstdint>
#include <string>

namespace ahpv {

struct RecorderOptions {
    int fsync_interval_frames{30};
    bool frame_checksum{true};
    std::uint64_t max_session_bytes{0};    // 0 = unlimited.
    std::uint64_t warn_free_bytes{512ull << 20};
    std::uint64_t reserve_free_bytes{64ull << 20};
};

class Recorder {
public:
    // manifest_base must already contain the session, software, source,
    // configuration and provenance sections; status/files are maintained here.
    Recorder(std::string session_dir, json::Value manifest_base, const SourceInfo& source, RecorderOptions options);
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Returns false once recording has stopped (reason in stop_reason()).
    bool write_frame(const Frame& frame, const QualityStats& stats, std::int64_t processing_ns);
    // Events are retained even after frame writing stops, while space allows.
    void record_event(json::Value event);
    // Periodic resource/latency samples (metrics.jsonl), included in checksums.
    void write_metrics(const json::Value& sample);
    // Idempotent. status: complete | segment_complete | camera_lost | source_error |
    // stopped_storage | error. Returns the final manifest.
    json::Value finalize(const std::string& status, json::Value statistics);

    bool active() const { return active_; }
    const std::string& stop_reason() const { return stop_reason_; }
    const std::string& stop_status() const { return stop_status_; }
    std::uint64_t frames_written() const { return frames_written_; }
    std::uint64_t bytes_written() const;
    const std::string& directory() const { return dir_; }

private:
    std::string dir_;
    json::Value manifest_;
    RecorderOptions options_;
    FrameGeometry geometry_;
    AppendFile video_, frames_, events_, metrics_;
    bool active_{true};
    bool finalized_{false};
    bool storage_warned_{false};
    bool events_failed_{false};
    std::string stop_reason_;
    std::string stop_status_;
    std::uint64_t frames_written_{0};
    std::uint64_t since_sync_{0};

    void stop(const std::string& status, const std::string& reason);
    void write_manifest();
    void sync_all();
};

json::Value make_event(const std::string& type, json::Value details = json::Value());

}  // namespace ahpv
