#include "frames/pipeline.hpp"

#include "capture/fault_injection.hpp"
#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "common/log.hpp"
#include "common/version.hpp"
#include "metrics/resources.hpp"
#include "platform/platform.hpp"

#include <chrono>
#include <cmath>
#include <new>
#include <thread>

namespace ahpv {
namespace {

DroppedFrame summarize(const Frame& f) {
    return {f.frame_id, f.source_epoch, f.has_source_sequence, f.source_sequence, f.acquisition_mono_ns};
}

constexpr const char* kFlagOrder[] = {"corrupted", "sequence_gap", "late", "timestamp_regression", "repeated_content",
                                      "low_contrast", "very_dark", "very_bright", "after_discontinuity",
                                      "after_no_signal", "fault_injected"};

}  // namespace

const char* service_state_name(ServiceState state) {
    switch (state) {
        case ServiceState::Starting: return "starting";
        case ServiceState::Running: return "running";
        case ServiceState::NoSignal: return "no_signal";
        case ServiceState::CameraLost: return "camera_lost";
        case ServiceState::SourceError: return "source_error";
        case ServiceState::Finished: return "finished";
        case ServiceState::Stopped: return "stopped";
    }
    return "unknown";
}

Pipeline::Pipeline(PipelineOptions options) : options_(std::move(options)) {}

Pipeline::~Pipeline() {
    if (recorder_) finish_segment("error");
}

void Pipeline::open() {
    const Config& c = options_.config;
    try {
        source_ = open_source(c.source);
    } catch (const std::exception& error) {
        log_error("CAP-001", std::string("camera/source initialization failed: ") + error.what(),
                  json::Object{{"kind", c.source.kind}, {"path", c.source.path}});
        throw;
    }
    if (!options_.fault_spec.empty())
        source_ = std::make_unique<FaultInjectingSource>(std::move(source_), parse_fault_spec(options_.fault_spec),
                                                         c.source.no_signal_timeout_ms);
    info_ = source_->info();
    if (c.keep_every_n > 1) {
        if (!info_.live) throw std::invalid_argument("pipeline.keep_every_n applies to live sources only; file replay is lossless");
        info_.transformations.push_back("temporal_decimation_keep_1_of_" + std::to_string(c.keep_every_n));
    }
    log_info("CAP-002", "source opened", info_.to_json());
    if (c.recording_enabled) {
        store_ = std::make_unique<SessionStore>(c.data_root);
        run_id_ = "run-" + compact_utc(wall_ns()) + "-" + random_hex(4);
    }
}

void Pipeline::push_event(json::Value event) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.push_back(std::move(event));
}

std::shared_ptr<const Frame> Pipeline::latest_frame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

bool Pipeline::source_full_range() const {
    const json::Value* range = info_.effective.find("color_range");
    return range && range->is_string() && range->as_string() == "FULL";
}

std::vector<std::string> Pipeline::session_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_;
}

json::Value Pipeline::manifest_base(const std::string& session_id) const {
    const Config& c = options_.config;
    json::Value m;
    m.set("schema_version", kSchemaVersion);
    m.set("usage_notice", kUsageNotice);
    m.set("session_id", session_id);
    m.set("run_id", run_id_);
    m.set("segment_index", segment_index_);
    m.set("previous_session_id", previous_session_id_.empty() ? json::Value(nullptr) : json::Value(previous_session_id_));
    m.set("created_at", iso8601_utc(wall_ns()));
    json::Value software;
    software.set("name", kSoftwareName);
    software.set("version", kVersion);
    software.set("build_id", kBuildId);
    software.set("executable_sha256", executable_sha256());
    m.set("software", software);
    json::Value platform = platform_summary();
    if (!options_.inventory_path.empty()) {
        try {
            platform.set("board_inventory_sha256", sha256_file(options_.inventory_path));
            platform.set("board_inventory_path", options_.inventory_path);
        } catch (const std::exception& error) {
            platform.set("board_inventory_error", error.what());
        }
    }
    m.set("platform", platform);
    json::Value clocks;
    clocks.set("ordering_clock", "CLOCK_MONOTONIC");
    clocks.set("dating_clock", "CLOCK_REALTIME");
    clocks.set("monotonic_at_creation_ns", mono_ns());
    clocks.set("realtime_at_creation_ns", wall_ns());
    clocks.set("note", "acquisition_* = when this process obtained the frame; source_timestamp_ns follows source.timestamp_basis");
    m.set("clocks", clocks);
    m.set("source", info_.to_json());
    json::Value config = c.to_json();
    m.set("configuration", config);
    json::Value pipeline;
    pipeline.set("queue_capacity", static_cast<std::int64_t>(c.queue_capacity));
    pipeline.set("drop_policy", drop_policy_name(c.drop_policy));
    pipeline.set("overflow_policy", info_.live ? drop_policy_name(c.drop_policy) : "backpressure (finite input, lossless)");
    pipeline.set("drop_accounting", "every discarded frame ID is logged as a queue_drop event");
    pipeline.set("keep_every_n", static_cast<std::int64_t>(c.keep_every_n));
    pipeline.set("decimation_accounting", "frames skipped by keep_every_n receive no frame ID; their count is in statistics.run_frames_decimated");
    m.set("pipeline", pipeline);
    m.set("provenance", options_.provenance.to_json());
    m.set("fault_injection", options_.fault_spec.empty() ? json::Value(nullptr) : json::Value(options_.fault_spec));
    return m;
}

void Pipeline::start_segment() {
    ++segment_index_;
    const std::string id = store_->create();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id_ = id;
    }
    Logger::instance().set_session(id);
    recorder_ = std::make_unique<Recorder>(store_->directory(id), manifest_base(id), info_, options_.config.recorder);
    segment_accounted_ = 0;
    segment_stored_ = 0;
    interval_ = Histogram();
    processing_ = Histogram();
    queue_latency_ = Histogram();
    first_acquisition_ = last_acquisition_ = 0;
    peak_resources_ = json::Value();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.push_back(id);
        recording_note_.clear();
    }
    log_info("REC-001", "recording session started", json::Object{{"session_id", id}, {"segment_index", segment_index_}});
}

json::Value Pipeline::segment_statistics() const {
    json::Value s;
    s.set("frames_assigned", segment_accounted_);
    s.set("frames_stored", segment_stored_);
    json::Value flags;
    for (std::size_t i = 0; i < 11; ++i) flags.set(kFlagOrder[i], flag_counts_[i].load());
    s.set("run_flag_counts", flags);
    s.set("run_frames_dropped_queue", dropped_.load());
    s.set("run_frames_rejected", rejected_.load());
    s.set("run_frames_missing_upstream", missing_upstream_.load());
    s.set("run_no_signal_episodes", no_signal_episodes_.load());
    s.set("run_disconnects", disconnects_.load());
    s.set("run_reopens", reopens_.load());
    s.set("run_frames_decimated", decimated_.load());
    s.set("acquisition_interval", interval_.to_json());
    s.set("processing_time", processing_.to_json());
    s.set("queue_latency", queue_latency_.to_json());
    if (segment_stored_ > 1 && last_acquisition_ > first_acquisition_) {
        const double seconds = static_cast<double>(last_acquisition_ - first_acquisition_) / 1e9;
        s.set("effective_fps_measured", std::round(static_cast<double>(segment_stored_ - 1) / seconds * 1000) / 1000);
    } else {
        s.set("effective_fps_measured", nullptr);
    }
    s.set("peak_resources", peak_resources_.is_null() ? json::Value(json::Object{}) : peak_resources_);
    return s;
}

void Pipeline::finish_segment(const std::string& status) {
    if (!recorder_) return;
    const json::Value manifest = recorder_->finalize(status, segment_statistics());
    log_info("REC-002", "recording session finalized",
             json::Object{{"session_id", session_id_}, {"status", manifest.at("status").as_string()}});
    previous_session_id_ = session_id_;
    recorder_.reset();
}

void Pipeline::account(const json::Value& event) {
    if (!recorder_) return;
    const std::string& type = event.at("event").as_string();
    const bool accounting = type == "queue_drop" || type == "rejected_frame";
    // After the recorder stops, later frame IDs belong to no session.
    if (accounting && !recorder_->active()) return;
    recorder_->record_event(event);
    if (accounting) ++segment_accounted_;
}

void Pipeline::capture_loop(BoundedQueue<Frame, DroppedFrame>& queue, const std::atomic<bool>& stop_requested) {
    const Config& c = options_.config;
    SequenceTimingAnalyzer analyzer(info_.nominal_interval_ns(), c.validation.late_factor, c.keep_every_n);
    std::uint64_t epoch = 0, next_id = 0, delivered = 0;
    std::int64_t last_preview_ns = 0;
    bool in_no_signal = false;
    const std::int64_t deadline = options_.duration_s > 0 ? started_mono_ + static_cast<std::int64_t>(options_.duration_s * 1e9) : 0;
    state_ = ServiceState::Running;
    while (!stop_requested.load()) {
        if (options_.max_frames && next_id >= options_.max_frames) break;
        if (deadline && mono_ns() >= deadline) break;
        Frame frame;
        ReadResult result;
        try {
            result = source_->read(frame);
        } catch (const std::bad_alloc&) {
            log_error("RES-010", "memory allocation failed in capture; stopping safely");
            push_event(make_event("memory_failure", json::Object{{"stage", "capture"}}));
            final_status_ = "error";
            state_ = ServiceState::SourceError;
            break;
        } catch (const std::exception& error) {
            result = {ReadStatus::Error, error.what()};
        }
        switch (result.status) {
            case ReadStatus::Frame: {
                // Configured decimation happens before ID assignment: it is a declared
                // operating-envelope transform, not loss.
                if (c.keep_every_n > 1 && delivered++ % c.keep_every_n != 0) {
                    ++decimated_;
                    break;
                }
                frame.frame_id = next_id++;
                frame.source_epoch = epoch;
                analyzer.observe(frame);
                if (in_no_signal) {
                    in_no_signal = false;
                    state_ = ServiceState::Running;
                    log_info("CAP-011", "signal restored");
                    push_event(make_event("signal_restored", json::Object{{"next_frame_id", frame.frame_id}}));
                }
                if (c.preview_enabled && frame.acquisition_mono_ns - last_preview_ns >= 100000000) {
                    last_preview_ns = frame.acquisition_mono_ns;
                    auto preview = std::make_shared<const Frame>(frame);  // Copy; recording keeps the original.
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_ = std::move(preview);
                }
                missing_upstream_ += frame.missing_before;
                for (std::size_t i = 0; i < 11; ++i)
                    if (frame.flags & (1u << i)) ++flag_counts_[i];
                ++assigned_;
                if (info_.live) queue.push(std::move(frame));
                else queue.push_blocking(std::move(frame));
                break;
            }
            case ReadStatus::Corrupted: {
                const std::uint64_t id = next_id++;
                // Keep sequence continuity: a rejected frame is not an upstream gap.
                frame.source_epoch = epoch;
                analyzer.observe(frame);
                ++assigned_;
                ++rejected_;
                ++flag_counts_[0];
                log_warn("CAP-020", "corrupted frame rejected", json::Object{{"frame_id", id}, {"reason", result.message}});
                push_event(make_event("rejected_frame", json::Object{
                    {"frame_id", id}, {"source_epoch", epoch}, {"reason", result.message},
                    {"source_sequence", frame.has_source_sequence ? json::Value(frame.source_sequence) : json::Value(nullptr)},
                    {"fault_injected", (frame.flags & kFlagFaultInjected) != 0}}));
                break;
            }
            case ReadStatus::NoSignal:
                if (!in_no_signal) {
                    in_no_signal = true;
                    ++no_signal_episodes_;
                    state_ = ServiceState::NoSignal;
                    analyzer.mark_no_signal();
                    log_warn("CAP-010", "no signal: " + result.message);
                    push_event(make_event("no_signal", json::Object{{"message", result.message}}));
                }
                break;
            case ReadStatus::EndOfStream:
                log_info("CAP-003", result.message);
                push_event(make_event("end_of_stream", json::Object{{"message", result.message}}));
                state_ = ServiceState::Finished;
                capture_done_ = true;
                return;
            case ReadStatus::Disconnected: {
                ++disconnects_;
                state_ = ServiceState::CameraLost;
                log_error("CAP-030", "source disconnected: " + result.message);
                push_event(make_event("source_disconnected", json::Object{{"message", result.message}}));
                bool restored = false;
                if (c.source_auto_reopen && source_->reopenable()) {
                    for (int attempt = 1; attempt <= c.source_reopen_attempts && !stop_requested.load(); ++attempt) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(c.source_reopen_delay_ms));
                        try {
                            source_->reopen();
                            ++epoch;
                            ++reopens_;
                            analyzer.mark_discontinuity();
                            state_ = ServiceState::Running;
                            log_info("CAP-031", "source reopened", json::Object{{"attempt", attempt}, {"source_epoch", epoch}});
                            push_event(make_event("source_reopened", json::Object{{"attempt", attempt}, {"source_epoch", epoch}}));
                            restored = true;
                            delivered = 0;
                            break;
                        } catch (const std::exception& error) {
                            log_warn("CAP-032", std::string("reopen failed: ") + error.what(), json::Object{{"attempt", attempt}});
                            push_event(make_event("source_reopen_failed", json::Object{{"attempt", attempt}, {"message", error.what()}}));
                        }
                    }
                }
                if (!restored) {
                    final_status_ = "camera_lost";
                    capture_done_ = true;
                    return;
                }
                break;
            }
            case ReadStatus::Error:
                log_error("CAP-040", "source error: " + result.message);
                push_event(make_event("source_error", json::Object{{"message", result.message}}));
                final_status_ = "source_error";
                state_ = ServiceState::SourceError;
                capture_done_ = true;
                return;
        }
    }
    if (stop_requested.load()) push_event(make_event("operator_stop"));
    else push_event(make_event("capture_limit_reached", json::Object{{"frames_assigned", next_id}}));
    state_ = ServiceState::Stopped;
    capture_done_ = true;
}

int Pipeline::run(const std::atomic<bool>& stop_requested) {
    const Config& c = options_.config;
    started_mono_ = mono_ns();
    if (store_) start_segment();
    BoundedQueue<Frame, DroppedFrame> queue(c.queue_capacity, c.drop_policy, &summarize);
    ContentAnalyzer content(c.validation);
    ResourceSampler sampler(c.data_root);
    std::int64_t next_sample = mono_ns();
    std::thread capture([&] {
        capture_loop(queue, stop_requested);
        queue.close();
    });

    std::int64_t previous_acquisition = 0;
    for (;;) {
        std::optional<Frame> item = queue.pop(std::chrono::milliseconds(100));
        // Drain events and drops recorded before this frame was queued, preserving ID order.
        std::vector<json::Value> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events.swap(pending_events_);
        }
        std::vector<DroppedFrame> drops = queue.take_dropped();
        for (const auto& d : drops) {
            ++dropped_;
            account(make_event("queue_drop", json::Object{
                {"frame_id", d.frame_id}, {"source_epoch", d.source_epoch},
                {"source_sequence", d.has_source_sequence ? json::Value(d.source_sequence) : json::Value(nullptr)},
                {"acquisition_mono_ns", d.acquisition_mono_ns}, {"policy", drop_policy_name(c.drop_policy)}}));
        }
        for (auto& e : events) account(e);
        if (!drops.empty()) log_warn("PIP-010", "frame queue overflow", json::Object{{"dropped", static_cast<std::int64_t>(drops.size())}});

        if (item) {
            const std::int64_t begin = mono_ns();
            auto frame = std::make_shared<Frame>(std::move(*item));
            const QualityStats stats = content.analyze(*frame, c.recorder.frame_checksum);
            for (std::size_t i = 4; i < 8; ++i)
                if (frame->flags & (1u << i)) ++flag_counts_[i];
            queue_latency_.add(begin - frame->acquisition_mono_ns);
            if (previous_acquisition) interval_.add(frame->acquisition_mono_ns - previous_acquisition);
            previous_acquisition = frame->acquisition_mono_ns;
            if (!first_acquisition_) first_acquisition_ = frame->acquisition_mono_ns;
            last_acquisition_ = frame->acquisition_mono_ns;
            if (!c.preview_enabled) {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_ = frame;  // Status only; the preview publishes from capture.
            }
            if (recorder_ && recorder_->active()) {
                // Processing time covers validation and storage of this frame.
                const std::int64_t processing = mono_ns() - begin;
                if (recorder_->write_frame(*frame, stats, processing)) {
                    ++stored_;
                    ++segment_stored_;
                    ++segment_accounted_;
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    recording_note_ = "recording stopped: " + recorder_->stop_reason();
                }
            }
            processing_.add(mono_ns() - begin);
            if (recorder_ && recorder_->active() && segment_stored_ >= c.segment_max_frames) {
                finish_segment("segment_complete");
                try {
                    start_segment();
                } catch (const std::exception& error) {
                    // Preview continues; the operator sees why recording ended.
                    log_error("REC-003", std::string("could not start the next segment: ") + error.what());
                    std::lock_guard<std::mutex> lock(mutex_);
                    recording_note_ = std::string("recording stopped: next segment failed: ") + error.what();
                }
            }
        } else if (queue.closed_and_empty() && capture_done_.load()) {
            break;
        }

        if (mono_ns() >= next_sample) {
            next_sample = mono_ns() + static_cast<std::int64_t>(c.metrics_interval_ms) * 1000000;
            json::Value sample = sampler.sample();
            sample.set("queue_depth", static_cast<std::int64_t>(queue.size()));
            sample.set("queue_high_water", static_cast<std::int64_t>(queue.high_water()));
            sample.set("frames_assigned", assigned_.load());
            sample.set("frames_stored", stored_.load());
            sample.set("frames_dropped_queue", dropped_.load());
            sample.set("frames_rejected", rejected_.load());
            sample.set("state", service_state_name(state_.load()));
            for (const char* key : {"rss_bytes", "peak_rss_bytes", "process_cpu_percent"}) {
                const json::Value* value = sample.find(key);
                const json::Value* current = peak_resources_.find(key);
                if (value && value->is_number() && (!current || value->as_number() > current->as_number()))
                    peak_resources_.set(key, *value);
            }
            const json::Value* free = sample.find("storage_free_bytes");
            const json::Value* low = peak_resources_.find("min_storage_free_bytes");
            if (free && free->is_number() && (!low || free->as_number() < low->as_number()))
                peak_resources_.set("min_storage_free_bytes", *free);
            peak_resources_.set("queue_high_water", static_cast<std::int64_t>(queue.high_water()));
            if (recorder_) recorder_->write_metrics(sample);
            log_info("MET-001", "resource sample", sample);
        }
    }
    capture.join();
    // Final drain after the capture thread has exited.
    std::vector<json::Value> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.swap(pending_events_);
    }
    for (auto& e : events) account(e);
    finish_segment(final_status_);
    if (state_ == ServiceState::Running || state_ == ServiceState::NoSignal) state_ = ServiceState::Stopped;
    log_info("PIP-002", "pipeline finished", status());
    if (final_status_ == "camera_lost") return 3;
    if (final_status_ == "source_error") return 4;
    if (final_status_ == "error") return 5;
    return 0;
}

json::Value Pipeline::status() const {
    json::Value s;
    s.set("usage_notice", kUsageNotice);
    s.set("state", service_state_name(state_.load()));
    s.set("source", info_.to_json());
    s.set("frames_assigned", assigned_.load());
    s.set("frames_stored", stored_.load());
    s.set("frames_dropped_queue", dropped_.load());
    s.set("frames_rejected", rejected_.load());
    s.set("frames_missing_upstream", missing_upstream_.load());
    s.set("no_signal_episodes", no_signal_episodes_.load());
    s.set("disconnects", disconnects_.load());
    s.set("reopens", reopens_.load());
    s.set("frames_decimated", decimated_.load());
    std::lock_guard<std::mutex> lock(mutex_);
    s.set("recording_enabled", options_.config.recording_enabled);
    s.set("current_session_id", session_id_.empty() ? json::Value(nullptr) : json::Value(session_id_));
    s.set("recording_note", recording_note_);
    if (latest_) {
        s.set("last_frame_id", latest_->frame_id);
        s.set("last_frame_age_ms", static_cast<double>(mono_ns() - latest_->acquisition_mono_ns) / 1e6);
    } else {
        s.set("last_frame_id", nullptr);
        s.set("last_frame_age_ms", nullptr);
    }
    return s;
}

}  // namespace ahpv
