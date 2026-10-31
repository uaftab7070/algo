#include "recording/recorder.hpp"

#include "capture/y4m_source.hpp"
#include "common/clock.hpp"
#include "common/hash.hpp"
#include "common/log.hpp"
#include "common/version.hpp"

namespace ahpv {

json::Value make_event(const std::string& type, json::Value details) {
    json::Value event;
    event.set("event", type);
    event.set("at_mono_ns", mono_ns());
    event.set("at", iso8601_utc(wall_ns()));
    if (details.is_object())
        for (auto& [key, value] : details.as_object()) event.set(key, value);
    return event;
}

Recorder::Recorder(std::string session_dir, json::Value manifest_base, const SourceInfo& source, RecorderOptions options)
    : dir_(std::move(session_dir)), manifest_(std::move(manifest_base)), options_(options), geometry_(source.geometry) {
    json::Value recording;
    recording.set("container", "yuv4mpeg2");
    recording.set("pixel_format", pixel_format_name(geometry_.format));
    recording.set("frame_bytes", static_cast<std::int64_t>(geometry_.frame_bytes()));
    recording.set("frame_checksum", options_.frame_checksum ? "crc32" : "none");
    recording.set("fsync_interval_frames", options_.fsync_interval_frames);
    recording.set("max_session_bytes", static_cast<std::int64_t>(options_.max_session_bytes));
    recording.set("reserve_free_bytes", static_cast<std::int64_t>(options_.reserve_free_bytes));
    recording.set("encoding", "none (raw planar); hardware encoding is not integrated, see docs/known-limitations.md");
    manifest_.set("recording", recording);
    manifest_.set("status", "recording");
    manifest_.set("manifest_revision", 0);
    json::Value files;
    files.set("video", "video.y4m");
    files.set("frame_index", "frames.jsonl");
    files.set("events", "events.jsonl");
    files.set("metrics", "metrics.jsonl");
    files.set("annotations", "annotations.jsonl");
    files.set("checksums", "checksums.json");
    manifest_.set("files", files);
    write_manifest();

    video_ = AppendFile(join_path(dir_, "video.y4m"));
    frames_ = AppendFile(join_path(dir_, "frames.jsonl"));
    events_ = AppendFile(join_path(dir_, "events.jsonl"));
    metrics_ = AppendFile(join_path(dir_, "metrics.jsonl"));
    // No per-session values in the container, so identical input yields an
    // identical video file (AC-007); the manifest records its SHA-256.
    std::vector<std::string> extensions;
    // Y4M requires a rate; an unknown source rate is declared rather than invented.
    if (source.fps_num <= 0) extensions.push_back("AHPV_NOMINAL_FPS=UNKNOWN");
    video_.write(format_y4m_header(geometry_, source.fps_num > 0 ? source.fps_num : 1, source.fps_num > 0 ? source.fps_den : 1,
                                   extensions));
    sync_all();
}

std::uint64_t Recorder::bytes_written() const {
    return video_.size() + frames_.size() + events_.size() + metrics_.size();
}

void Recorder::write_metrics(const json::Value& sample) {
    if (finalized_) return;
    try {
        metrics_.write(json::dump(sample) + "\n");
    } catch (const IoError& error) {
        log_error("REC-022", std::string("metrics write failed: ") + error.what());
        if (active_) stop(error.storage_exhausted() ? "stopped_storage" : "error", error.what());
    }
}

void Recorder::write_manifest() {
    manifest_.set("manifest_revision", manifest_.at("manifest_revision").as_int() + 1);
    manifest_.set("manifest_written_at", iso8601_utc(wall_ns()));
    write_file_atomic(join_path(dir_, "session.json"), json::dump(manifest_) + "\n", 0640);
}

void Recorder::sync_all() {
    video_.sync();
    frames_.sync();
    events_.sync();
    metrics_.sync();
    since_sync_ = 0;
}

void Recorder::stop(const std::string& status, const std::string& reason) {
    if (!active_) return;
    active_ = false;
    stop_status_ = status;
    stop_reason_ = reason;
    log_warn("REC-020", "recording stopped: " + reason, json::Object{{"status", status}});
    record_event(make_event("recording_stopped", json::Object{{"status", status}, {"reason", reason}}));
}

void Recorder::record_event(json::Value event) {
    if (finalized_ || events_failed_) return;
    try {
        events_.write(json::dump(event) + "\n");
    } catch (const IoError& error) {
        // Losing the event log is reported, never silently ignored.
        events_failed_ = true;
        log_error("REC-021", std::string("event log write failed: ") + error.what());
        if (active_) stop(error.storage_exhausted() ? "stopped_storage" : "error", error.what());
    }
}

bool Recorder::write_frame(const Frame& frame, const QualityStats& stats, std::int64_t processing_ns) {
    if (!active_) return false;
    if (frame.geometry != geometry_) {
        stop("source_error", "frame geometry changed during the session");
        return false;
    }
    const std::uint64_t needed = 6 + frame.data.size() + 512;
    if (options_.max_session_bytes > 0 && bytes_written() + needed > options_.max_session_bytes) {
        stop("stopped_storage", "configured max_session_bytes reached");
        return false;
    }
    const StorageStatus storage = storage_status(dir_);
    if (storage.available) {
        if (storage.free_bytes < options_.reserve_free_bytes + needed) {
            stop("stopped_storage", "free space reached the protected reserve (" +
                                        std::to_string(storage.free_bytes) + " bytes free)");
            return false;
        }
        if (storage.free_bytes < options_.warn_free_bytes && !storage_warned_) {
            storage_warned_ = true;
            log_warn("REC-010", "storage is low", json::Object{{"free_bytes", static_cast<std::int64_t>(storage.free_bytes)}});
            record_event(make_event("storage_low", json::Object{{"free_bytes", static_cast<std::int64_t>(storage.free_bytes)}}));
        } else if (storage.free_bytes > options_.warn_free_bytes + options_.warn_free_bytes / 10) {
            storage_warned_ = false;
        }
    }
    try {
        FrameIndexEntry entry;
        entry.frame_id = frame.frame_id;
        entry.source_epoch = frame.source_epoch;
        entry.has_source_sequence = frame.has_source_sequence;
        entry.source_sequence = frame.source_sequence;
        entry.acquisition_mono_ns = frame.acquisition_mono_ns;
        entry.acquisition_wall_ns = frame.acquisition_wall_ns;
        entry.source_timestamp_ns = frame.source_timestamp_ns;
        entry.interval_ns = frame.interval_ns;
        entry.missing_before = frame.missing_before;
        entry.offset = video_.size() + 6;
        entry.size = frame.data.size();
        entry.has_crc = options_.frame_checksum;
        entry.crc = stats.crc32;
        entry.luma_mean = stats.luma_mean;
        entry.luma_std = stats.luma_std;
        entry.flags = frame.flags;
        entry.processing_ns = processing_ns;
        // Payload first: the index never references bytes that were not written.
        video_.write("FRAME\n", 6);
        video_.write(frame.data.data(), frame.data.size());
        frames_.write(json::dump(frame_index_json(entry)) + "\n");
        ++frames_written_;
        if (++since_sync_ >= static_cast<std::uint64_t>(options_.fsync_interval_frames)) sync_all();
    } catch (const IoError& error) {
        stop(error.storage_exhausted() ? "stopped_storage" : "error", error.what());
        return false;
    }
    return true;
}

json::Value Recorder::finalize(const std::string& status, json::Value statistics) {
    if (finalized_) return manifest_;
    const std::string final_status = stop_status_.empty() ? status : stop_status_;
    record_event(make_event("session_finalized", json::Object{{"status", final_status}}));
    finalized_ = true;
    json::Value checksums;
    try {
        sync_all();
        video_.close();
        frames_.close();
        events_.close();
        metrics_.close();
        json::Value files;
        for (const char* name : {"video.y4m", "frames.jsonl", "events.jsonl", "metrics.jsonl"}) {
            const std::string path = join_path(dir_, name);
            json::Value item;
            item.set("sha256", sha256_file(path));
            item.set("size", file_size(path));
            files.set(name, item);
        }
        checksums.set("schema_version", kSchemaVersion);
        checksums.set("algorithm", "sha256");
        checksums.set("computed_at", iso8601_utc(wall_ns()));
        checksums.set("files", files);
        write_file_atomic(join_path(dir_, "checksums.json"), json::dump(checksums) + "\n", 0640);
    } catch (const IoError& error) {
        log_error("REC-030", std::string("finalization could not complete checksums: ") + error.what());
        manifest_.set("finalization_error", error.what());
    }
    manifest_.set("status", final_status);
    manifest_.set("stop_reason", stop_reason_.empty() ? json::Value(nullptr) : json::Value(stop_reason_));
    manifest_.set("finalized_at", iso8601_utc(wall_ns()));
    manifest_.set("statistics", std::move(statistics));
    if (checksums.is_object()) manifest_.set("checksums", checksums.at("files"));
    try {
        write_manifest();
    } catch (const IoError& error) {
        // The previous manifest (status "recording") remains; recovery handles it.
        log_error("REC-031", std::string("final manifest write failed; run 'ahpv recover': ") + error.what());
    }
    return manifest_;
}

}  // namespace ahpv
