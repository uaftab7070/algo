#include "recording/verify.hpp"

#include "capture/y4m_source.hpp"
#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "common/version.hpp"
#include "recording/recorder.hpp"
#include "recording/session.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace ahpv {
namespace {

const std::set<std::string> kFinalStatuses = {"complete", "segment_complete", "camera_lost", "source_error",
                                              "stopped_storage", "error", "interrupted"};

class Checks {
public:
    void add(const std::string& name, const std::string& status, const std::string& detail) {
        json::Value c;
        c.set("name", name);
        c.set("status", status);
        c.set("detail", detail);
        checks_.push_back(std::move(c));
        if (status == "fail") ok_ = false;
    }
    void pass(const std::string& n, const std::string& d) { add(n, "pass", d); }
    void fail(const std::string& n, const std::string& d) { add(n, "fail", d); }
    void warn(const std::string& n, const std::string& d) { add(n, "warn", d); }
    bool ok() const { return ok_; }
    json::Value list() const { return json::Value(checks_); }

private:
    json::Array checks_;
    bool ok_{true};
};

std::string first_line(const std::string& path) {
    const int64_t size = file_size(path);
    if (size <= 0) return {};
    const auto bytes = read_range(path, 0, static_cast<std::size_t>(std::min<int64_t>(size, 4096)));
    const std::string text(bytes.begin(), bytes.end());
    const auto newline = text.find('\n');
    return newline == std::string::npos ? std::string() : text.substr(0, newline);
}

}  // namespace

json::Value verify_session(const std::string& dir, VerifyOptions options) {
    Checks checks;
    json::Value summary;
    SessionData session;
    try {
        session = load_session(dir);
    } catch (const std::exception& error) {
        checks.fail("manifest_readable", error.what());
        json::Value result;
        result.set("ok", false);
        result.set("usage_notice", kUsageNotice);
        result.set("checks", checks.list());
        return result;
    }
    const auto& m = session.manifest;
    checks.pass("manifest_readable", "session.json parsed");
    const json::Value* schema = m.find("schema_version");
    const json::Value* notice = m.find("usage_notice");
    if (schema && schema->is_string() && schema->as_string() == kSchemaVersion && notice && notice->is_string() &&
        notice->as_string() == kUsageNotice)
        checks.pass("manifest_schema", "schema_version and usage notice present");
    else
        checks.fail("manifest_schema", "schema_version or usage_notice missing/mismatched");

    const std::string status = m.at("status").as_string();
    const bool interrupted = status == "interrupted";
    if (kFinalStatuses.count(status)) checks.pass("session_finalized", "status " + status);
    else checks.fail("session_finalized", "status '" + status + "' is not final; run 'ahpv recover'");

    // File checksums.
    const std::string checksum_path = join_path(dir, "checksums.json");
    if (!path_exists(checksum_path)) {
        checks.fail("checksums_present", "checksums.json missing");
    } else {
        try {
            const json::Value sums = json::parse(read_file(checksum_path));
            std::string mismatches;
            for (const auto& [name, item] : sums.at("files").as_object()) {
                if (!valid_identifier(name)) {
                    mismatches += " " + name + "(invalid name)";
                    continue;
                }
                const std::string path = join_path(dir, name);
                if (!path_exists(path)) {
                    mismatches += " " + name + "(missing)";
                } else if (file_size(path) != item.at("size").as_int() || sha256_file(path) != item.at("sha256").as_string()) {
                    mismatches += " " + name;
                }
            }
            if (mismatches.empty()) checks.pass("checksums_match", "all SHA-256 checksums match");
            else checks.fail("checksums_match", "mismatch:" + mismatches);
            const json::Value* recorded = m.find("checksums");
            if (recorded && *recorded != sums.at("files"))
                checks.fail("manifest_checksums_consistent", "manifest checksums differ from checksums.json");
        } catch (const std::exception& error) {
            checks.fail("checksums_match", error.what());
        }
    }

    // Container header.
    const std::string video = join_path(dir, "video.y4m");
    try {
        const Y4mHeader header = parse_y4m_header(first_line(video));
        if (header.geometry == session.geometry) checks.pass("video_header", "Y4M header matches manifest geometry");
        else checks.fail("video_header", "Y4M header geometry differs from manifest");
    } catch (const std::exception& error) {
        checks.fail("video_header", error.what());
    }

    // Index parse errors.
    if (session.index_errors.empty()) {
        checks.pass("index_parse", std::to_string(session.frames.size()) + " frame records, " +
                                       std::to_string(session.events.size()) + " event records");
    } else {
        std::string detail = std::to_string(session.index_errors.size()) + " problem(s): " + session.index_errors.front();
        const bool only_partial = session.index_errors.size() == session.index_partial_lines;
        if (interrupted && only_partial) checks.warn("index_parse", detail + " (interrupted session)");
        else checks.fail("index_parse", detail);
    }

    // Frame records, payload placement and CRC.
    const std::int64_t video_size = file_size(video);
    std::uint64_t crc_failures = 0, placement_failures = 0, order_failures = 0, crc_checked = 0;
    std::uint64_t mono_regressions = 0, unflagged_source_regressions = 0, wall_steps = 0;
    std::uint64_t indexed_end = 0;
    const std::size_t expected_size = session.geometry.frame_bytes();
    for (std::size_t i = 0; i < session.frames.size(); ++i) {
        const auto& f = session.frames[i];
        if (i > 0) {
            const auto& p = session.frames[i - 1];
            if (f.frame_id <= p.frame_id || f.offset <= p.offset) ++order_failures;
            if (f.acquisition_mono_ns <= p.acquisition_mono_ns) ++mono_regressions;
            if (f.acquisition_wall_ns < p.acquisition_wall_ns) ++wall_steps;
            if (f.source_epoch == p.source_epoch && f.source_timestamp_ns <= p.source_timestamp_ns &&
                !(f.flags & kFlagTimestampRegression))
                ++unflagged_source_regressions;
        }
        bool placed = f.size == expected_size && f.offset >= 6 &&
                      static_cast<std::int64_t>(f.offset + f.size) <= video_size;
        if (placed) {
            const auto marker = read_range(video, f.offset - 6, 6);
            placed = std::string(marker.begin(), marker.end()) == "FRAME\n";
        }
        if (!placed) {
            ++placement_failures;
            continue;
        }
        indexed_end = std::max(indexed_end, f.offset + f.size);
        if (options.check_payload_crc && f.has_crc) {
            ++crc_checked;
            const auto payload = read_range(video, f.offset, f.size);
            if (crc32(payload.data(), payload.size()) != f.crc) ++crc_failures;
        }
    }
    if (order_failures == 0) checks.pass("frame_order", "frame IDs and offsets strictly increase");
    else checks.fail("frame_order", std::to_string(order_failures) + " ordering violations");
    if (placement_failures == 0) checks.pass("payload_placement", "every indexed payload follows a FRAME marker inside the file");
    else checks.fail("payload_placement", std::to_string(placement_failures) + " records point outside valid payloads");
    if (!options.check_payload_crc) checks.warn("payload_crc", "payload CRC check skipped by request");
    else if (crc_failures == 0) checks.pass("payload_crc", std::to_string(crc_checked) + " payload CRC-32 values match");
    else checks.fail("payload_crc", std::to_string(crc_failures) + " of " + std::to_string(crc_checked) + " payloads fail CRC-32");

    // Timestamp integrity (AC-023).
    if (mono_regressions == 0) checks.pass("monotonic_order", "acquisition monotonic timestamps strictly increase");
    else checks.fail("monotonic_order", std::to_string(mono_regressions) + " monotonic regressions");
    if (unflagged_source_regressions == 0) checks.pass("source_timestamp_order", "source timestamp regressions are all flagged");
    else checks.fail("source_timestamp_order", std::to_string(unflagged_source_regressions) + " unflagged source timestamp regressions");
    if (wall_steps > 0) checks.warn("wall_clock_steps", std::to_string(wall_steps) + " backward wall-clock steps (ordering uses monotonic time)");

    // Frame accounting (AC-005): every assigned ID is stored, dropped or rejected exactly once.
    std::map<std::uint64_t, std::string> accounted;
    std::uint64_t duplicates = 0;
    for (const auto& f : session.frames)
        if (!accounted.emplace(f.frame_id, "stored").second) ++duplicates;
    std::uint64_t dropped = 0, rejected = 0;
    for (const auto& e : session.events) {
        const json::Value* type = e.find("event");
        const json::Value* id = e.find("frame_id");
        if (!type || !id || !type->is_string()) continue;
        if (type->as_string() != "queue_drop" && type->as_string() != "rejected_frame") continue;
        (type->as_string() == "queue_drop" ? dropped : rejected)++;
        if (!accounted.emplace(static_cast<std::uint64_t>(id->as_int()), type->as_string()).second) ++duplicates;
    }
    std::uint64_t holes = 0;
    if (!accounted.empty()) {
        const std::uint64_t first = accounted.begin()->first, last = accounted.rbegin()->first;
        holes = (last - first + 1) - accounted.size();
        summary.set("first_frame_id", first);
        summary.set("last_frame_id", last);
        const json::Value* stats = m.find("statistics");
        if (stats && stats->is_object() && stats->find("frames_assigned")) {
            const std::uint64_t assigned = static_cast<std::uint64_t>(stats->at("frames_assigned").as_int());
            if (assigned != accounted.size())
                checks.fail("accounting_matches_manifest", "manifest frames_assigned " + std::to_string(assigned) +
                                                               " but " + std::to_string(accounted.size()) + " accounted");
            else
                checks.pass("accounting_matches_manifest", "manifest frames_assigned matches the records");
        }
    }
    if (duplicates == 0 && holes == 0)
        checks.pass("frame_accounting", std::to_string(accounted.size()) + " frame IDs, each stored, dropped or rejected once");
    else if (interrupted && duplicates == 0)
        checks.warn("frame_accounting", std::to_string(holes) + " unaccounted IDs in an interrupted session");
    else
        checks.fail("frame_accounting", std::to_string(duplicates) + " duplicates, " + std::to_string(holes) + " unaccounted IDs");

    if (video_size >= 0 && static_cast<std::uint64_t>(video_size) > indexed_end && !session.frames.empty()) {
        const std::uint64_t trailing = static_cast<std::uint64_t>(video_size) - indexed_end;
        if (interrupted || status == "stopped_storage") checks.warn("trailing_bytes", std::to_string(trailing) + " unindexed trailing bytes (not presented as frames)");
        else checks.fail("trailing_bytes", std::to_string(trailing) + " unindexed trailing bytes in a cleanly finalized session");
    }

    summary.set("frames_stored", static_cast<std::int64_t>(session.frames.size()));
    summary.set("frames_dropped", dropped);
    summary.set("frames_rejected", rejected);
    summary.set("status", status);
    json::Value result;
    result.set("schema_version", kSchemaVersion);
    result.set("usage_notice", kUsageNotice);
    result.set("session_id", session.id);
    result.set("verified_at", iso8601_utc(wall_ns()));
    result.set("verifier_version", kVersion);
    result.set("ok", checks.ok());
    result.set("checks", checks.list());
    result.set("summary", summary);
    return result;
}

json::Value recover_session(const std::string& dir) {
    json::Value result;
    SessionData session = load_session(dir);
    result.set("session_id", session.id);
    json::Value manifest = session.manifest;
    if (manifest.at("status").as_string() != "recording") {
        result.set("recovered", false);
        result.set("reason", "status is '" + manifest.at("status").as_string() + "', not 'recording'");
        return result;
    }
    const std::string video = join_path(dir, "video.y4m");
    const std::int64_t video_size = std::max<std::int64_t>(0, file_size(video));
    std::uint64_t indexed_end = 0;
    for (const auto& f : session.frames) indexed_end = std::max(indexed_end, f.offset + f.size);
    // Payload durability is unknown after power loss; check the newest records.
    std::uint64_t tail_crc_failures = 0;
    const std::size_t tail = std::min<std::size_t>(session.frames.size(), 5);
    for (std::size_t i = session.frames.size() - tail; i < session.frames.size(); ++i) {
        const auto& f = session.frames[i];
        if (!f.has_crc) continue;
        if (static_cast<std::int64_t>(f.offset + f.size) > video_size) {
            ++tail_crc_failures;
            continue;
        }
        const auto payload = read_range(video, f.offset, f.size);
        if (crc32(payload.data(), payload.size()) != f.crc) ++tail_crc_failures;
    }

    json::Value recovery;
    recovery.set("recovered_at", iso8601_utc(wall_ns()));
    recovery.set("indexed_frames", static_cast<std::int64_t>(session.frames.size()));
    recovery.set("incomplete_index_lines", static_cast<std::int64_t>(session.index_partial_lines));
    recovery.set("index_errors", static_cast<std::int64_t>(session.index_errors.size()));
    recovery.set("video_bytes", video_size);
    recovery.set("indexed_video_end", indexed_end);
    recovery.set("trailing_unindexed_bytes", static_cast<std::int64_t>(std::max<std::int64_t>(0, video_size - static_cast<std::int64_t>(indexed_end))));
    recovery.set("tail_crc_failures", tail_crc_failures);
    recovery.set("note", "Recorded data was not modified. Only indexed frames with valid CRC are usable.");

    // Append the recovery event on its own line even if the last line is partial.
    const std::string events_path = join_path(dir, "events.jsonl");
    {
        AppendFile events(events_path);
        const std::int64_t size = file_size(events_path);
        if (size > 0) {
            const auto last = read_range(events_path, static_cast<std::uint64_t>(size - 1), 1);
            if (last[0] != '\n') events.write("\n");
        }
        events.write(json::dump(make_event("session_recovered", recovery)) + "\n");
        events.sync();
    }
    json::Value files;
    for (const char* name : {"video.y4m", "frames.jsonl", "events.jsonl", "metrics.jsonl"}) {
        const std::string path = join_path(dir, name);
        if (!path_exists(path)) continue;
        json::Value item;
        item.set("sha256", sha256_file(path));
        item.set("size", file_size(path));
        files.set(name, item);
    }
    json::Value checksums;
    checksums.set("schema_version", kSchemaVersion);
    checksums.set("algorithm", "sha256");
    checksums.set("computed_at", iso8601_utc(wall_ns()));
    checksums.set("files", files);
    checksums.set("note", "computed during recovery over files as found");
    write_file_atomic(join_path(dir, "checksums.json"), json::dump(checksums) + "\n", 0640);

    manifest.set("status", "interrupted");
    manifest.set("recovery", recovery);
    manifest.set("checksums", files);
    manifest.set("manifest_revision", manifest.at("manifest_revision").as_int() + 1);
    manifest.set("manifest_written_at", iso8601_utc(wall_ns()));
    write_file_atomic(join_path(dir, "session.json"), json::dump(manifest) + "\n", 0640);
    result.set("recovered", true);
    result.set("recovery", recovery);
    return result;
}

}  // namespace ahpv
