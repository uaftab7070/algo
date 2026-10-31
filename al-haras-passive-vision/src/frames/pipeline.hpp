#pragma once
// Capture -> validation/IDs -> bounded queue -> recorder / preview / metrics.
// Output is observational data only; there is no outbound control path.
#include "capture/source.hpp"
#include "frames/bounded_queue.hpp"
#include "frames/frame.hpp"
#include "metrics/histogram.hpp"
#include "recording/recorder.hpp"
#include "recording/session.hpp"
#include "security/config.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ahpv {

enum class ServiceState { Starting, Running, NoSignal, CameraLost, SourceError, Finished, Stopped };
const char* service_state_name(ServiceState state);

struct PipelineOptions {
    Config config;
    Provenance provenance;
    std::string fault_spec;            // Empty: no fault injection.
    std::uint64_t max_frames{0};       // 0: unlimited.
    double duration_s{0};              // 0: unlimited.
    std::string inventory_path;        // Optional board inventory JSON to reference.
};

struct DroppedFrame {
    std::uint64_t frame_id;
    std::uint64_t source_epoch;
    bool has_source_sequence;
    std::uint64_t source_sequence;
    std::int64_t acquisition_mono_ns;
};

class Pipeline {
public:
    explicit Pipeline(PipelineOptions options);
    ~Pipeline();
    // Opens the source; throws with an actionable message (e.g. camera absent at boot).
    void open();
    // Runs until end of input, unrecoverable source loss, limits, or stop_requested.
    // Returns the process exit status (see docs/operating-guide.md).
    int run(const std::atomic<bool>& stop_requested);

    // Newest acquired frame, published by the capture thread (at most every 100 ms,
    // only with the preview enabled) so recording stalls never delay the preview.
    std::shared_ptr<const Frame> latest_frame() const;
    bool source_full_range() const;
    ServiceState state() const { return state_.load(); }
    json::Value status() const;
    const Config& config() const { return options_.config; }
    std::vector<std::string> session_ids() const;

private:
    PipelineOptions options_;
    std::unique_ptr<FrameSource> source_;
    SourceInfo info_;
    std::unique_ptr<SessionStore> store_;
    std::unique_ptr<Recorder> recorder_;
    std::string run_id_;
    std::string session_id_;
    std::string previous_session_id_;
    int segment_index_{-1};
    std::uint64_t segment_accounted_{0};

    std::atomic<ServiceState> state_{ServiceState::Starting};
    mutable std::mutex mutex_;  // Guards the fields below.
    std::shared_ptr<const Frame> latest_;
    std::vector<json::Value> pending_events_;
    std::vector<std::string> sessions_;
    std::string recording_note_;

    // Counters written by one thread each; read by status().
    std::atomic<std::uint64_t> assigned_{0}, stored_{0}, dropped_{0}, rejected_{0}, missing_upstream_{0};
    std::atomic<std::uint64_t> no_signal_episodes_{0}, disconnects_{0}, reopens_{0}, decimated_{0};
    std::atomic<std::uint64_t> flag_counts_[11]{};
    std::atomic<bool> capture_done_{false};
    std::string final_status_{"complete"};
    std::int64_t started_mono_{0};

    Histogram interval_, processing_, queue_latency_;
    std::int64_t first_acquisition_{0}, last_acquisition_{0};
    std::uint64_t segment_stored_{0};
    json::Value peak_resources_;

    void capture_loop(BoundedQueue<Frame, DroppedFrame>& queue, const std::atomic<bool>& stop_requested);
    void push_event(json::Value event);
    void start_segment();
    void finish_segment(const std::string& status);
    json::Value segment_statistics() const;
    json::Value manifest_base(const std::string& session_id) const;
    void account(const json::Value& event);
};

}  // namespace ahpv
