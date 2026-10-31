#pragma once
// Session directories and on-disk layout (docs/data-schemas.md):
//   <data_root>/sessions/<session_id>/session.json      manifest (atomic rewrite)
//                                    /video.y4m         8-bit planar frames
//                                    /frames.jsonl      one index record per stored frame
//                                    /events.jsonl      drops, rejections, signal and storage events
//                                    /checksums.json    SHA-256 of finalized files
//                                    /annotations.jsonl append-only annotation revisions
//                                    /reports/<id>/     generated QA reports (never overwritten)
#include "common/json.hpp"
#include "frames/frame.hpp"

#include <string>
#include <vector>

namespace ahpv {

struct Provenance {
    std::string owner{"unspecified"};
    std::string permission_record{"unspecified"};
    std::string place_category{"unspecified"};
    std::string purpose;
    std::string partition;  // development | acceptance
    std::string scene_label{"unspecified"};
    json::Value to_json() const;
    void validate() const;   // Throws std::invalid_argument.
};

bool valid_session_id(const std::string& id);
bool valid_identifier(const std::string& text, std::size_t max_length = 64);  // [A-Za-z0-9._-]
std::string random_hex(std::size_t bytes);  // From /dev/urandom; throws on failure.

class SessionStore {
public:
    explicit SessionStore(std::string data_root);
    // Creates a new, unique session directory atomically (mkdir); returns its ID.
    std::string create();
    std::string directory(const std::string& session_id) const;  // Validates the ID.
    std::vector<std::string> list() const;
    const std::string& root() const { return root_; }
    std::string sessions_root() const;

private:
    std::string root_;
};

struct FrameIndexEntry {
    std::uint64_t frame_id{0};
    std::uint64_t source_epoch{0};
    bool has_source_sequence{false};
    std::uint64_t source_sequence{0};
    std::int64_t acquisition_mono_ns{0};
    std::int64_t acquisition_wall_ns{0};
    std::int64_t source_timestamp_ns{0};
    std::int64_t interval_ns{-1};
    std::uint64_t missing_before{0};
    std::uint64_t offset{0};
    std::uint64_t size{0};
    bool has_crc{false};
    std::uint32_t crc{0};
    double luma_mean{0};
    double luma_std{0};
    std::uint32_t flags{0};
    std::int64_t processing_ns{0};
};

json::Value frame_index_json(const FrameIndexEntry& entry);
FrameIndexEntry parse_frame_index(const json::Value& value);  // Throws on schema violations.

struct SessionData {
    std::string id;
    std::string dir;
    json::Value manifest;
    FrameGeometry geometry;
    std::vector<FrameIndexEntry> frames;
    std::vector<json::Value> events;
    std::size_t index_partial_lines{0};
    std::vector<std::string> index_errors;
};

// Tolerant loader: malformed index lines are reported in index_errors, not hidden.
SessionData load_session(const std::string& dir);
const FrameIndexEntry* find_frame(const SessionData& session, std::uint64_t frame_id);
std::vector<std::uint8_t> read_frame_payload(const SessionData& session, const FrameIndexEntry& entry);

}  // namespace ahpv
