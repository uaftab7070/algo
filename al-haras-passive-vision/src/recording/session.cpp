#include "recording/session.hpp"

#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace ahpv {

json::Value Provenance::to_json() const {
    json::Value v;
    v.set("owner", owner);
    v.set("permission_record", permission_record);
    v.set("place_category", place_category);
    v.set("purpose", purpose);
    v.set("dataset_partition", partition);
    v.set("scene_label", scene_label);
    return v;
}

void Provenance::validate() const {
    if (partition != "development" && partition != "acceptance")
        throw std::invalid_argument("--partition must be 'development' or 'acceptance' (DAT-002)");
    if (purpose.empty()) throw std::invalid_argument("--purpose is required for every recording (DAT-001)");
    for (const auto* field : {&owner, &permission_record, &place_category, &purpose, &scene_label}) {
        if (field->size() > 200) throw std::invalid_argument("provenance fields are limited to 200 characters");
        for (const unsigned char c : *field)
            if (c < 0x20 || c == 0x7F) throw std::invalid_argument("provenance fields cannot contain control characters");
    }
}

bool valid_identifier(const std::string& text, std::size_t max_length) {
    if (text.empty() || text.size() > max_length) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    }) && text[0] != '.';
}

bool valid_session_id(const std::string& id) {
    // s-YYYYMMDDTHHMMSSZ-xxxxxxxx
    if (id.size() != 27 || id.compare(0, 2, "s-") != 0 || id[10] != 'T' || id[17] != 'Z' || id[18] != '-') return false;
    for (std::size_t i = 2; i < 17; ++i)
        if (i != 10 && !std::isdigit(static_cast<unsigned char>(id[i]))) return false;
    for (std::size_t i = 19; i < 27; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(id[i])) || std::isupper(static_cast<unsigned char>(id[i]))) return false;
    return true;
}

std::string random_hex(std::size_t bytes) {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw IoError("cannot open /dev/urandom", errno);
    std::vector<std::uint8_t> buffer(bytes);
    std::size_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::read(fd, buffer.data() + done, bytes - done);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            ::close(fd);
            throw IoError("cannot read /dev/urandom", n < 0 ? errno : EIO);
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return to_hex(buffer.data(), buffer.size());
}

SessionStore::SessionStore(std::string data_root) : root_(std::move(data_root)) {
    make_directories(sessions_root());
}

std::string SessionStore::sessions_root() const { return join_path(root_, "sessions"); }

std::string SessionStore::create() {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::string id = "s-" + compact_utc(wall_ns()) + "-" + random_hex(4);
        if (make_directory_exclusive(join_path(sessions_root(), id))) return id;
    }
    throw std::runtime_error("could not allocate a unique session ID after 16 attempts");
}

std::string SessionStore::directory(const std::string& session_id) const {
    if (!valid_session_id(session_id)) throw std::invalid_argument("invalid session ID '" + session_id + "'");
    return join_path(sessions_root(), session_id);
}

std::vector<std::string> SessionStore::list() const {
    std::vector<std::string> ids;
    for (const auto& name : list_directory(sessions_root()))
        if (valid_session_id(name) && is_directory(join_path(sessions_root(), name))) ids.push_back(name);
    return ids;
}

json::Value frame_index_json(const FrameIndexEntry& e) {
    json::Value v;
    v.set("frame_id", e.frame_id);
    v.set("source_epoch", e.source_epoch);
    v.set("source_sequence", e.has_source_sequence ? json::Value(e.source_sequence) : json::Value(nullptr));
    v.set("acquisition_mono_ns", e.acquisition_mono_ns);
    v.set("acquisition_wall_ns", e.acquisition_wall_ns);
    v.set("source_timestamp_ns", e.source_timestamp_ns);
    v.set("interval_ns", e.interval_ns >= 0 ? json::Value(e.interval_ns) : json::Value(nullptr));
    v.set("missing_before", e.missing_before);
    v.set("offset", e.offset);
    v.set("size", e.size);
    v.set("crc32", e.has_crc ? json::Value(hex32(e.crc)) : json::Value(nullptr));
    v.set("luma_mean", std::round(e.luma_mean * 1000) / 1000);
    v.set("luma_std", std::round(e.luma_std * 1000) / 1000);
    v.set("flags", flag_names(e.flags));
    v.set("processing_ns", e.processing_ns);
    return v;
}

FrameIndexEntry parse_frame_index(const json::Value& v) {
    FrameIndexEntry e;
    const auto unsigned_field = [&](const char* key) {
        const std::int64_t value = v.at(key).as_int();
        if (value < 0) throw std::invalid_argument(std::string(key) + " must be non-negative");
        return static_cast<std::uint64_t>(value);
    };
    e.frame_id = unsigned_field("frame_id");
    e.source_epoch = unsigned_field("source_epoch");
    if (!v.at("source_sequence").is_null()) {
        e.has_source_sequence = true;
        e.source_sequence = unsigned_field("source_sequence");
    }
    e.acquisition_mono_ns = v.at("acquisition_mono_ns").as_int();
    e.acquisition_wall_ns = v.at("acquisition_wall_ns").as_int();
    e.source_timestamp_ns = v.at("source_timestamp_ns").as_int();
    e.interval_ns = v.at("interval_ns").is_null() ? -1 : v.at("interval_ns").as_int();
    e.missing_before = unsigned_field("missing_before");
    e.offset = unsigned_field("offset");
    e.size = unsigned_field("size");
    if (!v.at("crc32").is_null()) {
        const std::string& text = v.at("crc32").as_string();
        if (text.size() != 8 || text.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::invalid_argument("crc32 must be 8 lowercase hex digits");
        e.has_crc = true;
        e.crc = static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
    }
    e.luma_mean = v.at("luma_mean").as_number();
    e.luma_std = v.at("luma_std").as_number();
    e.flags = parse_flag_names(v.at("flags"));
    e.processing_ns = v.at("processing_ns").as_int();
    return e;
}

SessionData load_session(const std::string& dir) {
    SessionData data;
    data.dir = dir;
    data.manifest = json::parse(read_file(join_path(dir, "session.json"), 4u << 20));
    data.id = data.manifest.at("session_id").as_string();
    const auto& source = data.manifest.at("source");
    data.geometry = {static_cast<int>(source.at("width").as_int()), static_cast<int>(source.at("height").as_int()),
                     parse_pixel_format(source.at("pixel_format").as_string())};
    const auto load_lines = [&](const std::string& name, auto&& consume) {
        const std::string path = join_path(dir, name);
        if (!path_exists(path)) return;
        const std::string text = read_file(path, 1u << 30);
        std::size_t start = 0, line_number = 0;
        while (start < text.size()) {
            const auto end = text.find('\n', start);
            ++line_number;
            if (end == std::string::npos) {
                // A final line without newline is an interrupted append.
                ++data.index_partial_lines;
                data.index_errors.push_back(name + ": line " + std::to_string(line_number) + " is incomplete");
                break;
            }
            try {
                consume(json::parse(std::string_view(text).substr(start, end - start)));
            } catch (const std::exception& error) {
                data.index_errors.push_back(name + ": line " + std::to_string(line_number) + ": " + error.what());
            }
            start = end + 1;
        }
    };
    load_lines("frames.jsonl", [&](json::Value v) { data.frames.push_back(parse_frame_index(v)); });
    load_lines("events.jsonl", [&](json::Value v) { data.events.push_back(std::move(v)); });
    return data;
}

const FrameIndexEntry* find_frame(const SessionData& session, std::uint64_t frame_id) {
    const auto it = std::lower_bound(session.frames.begin(), session.frames.end(), frame_id,
                                     [](const FrameIndexEntry& e, std::uint64_t id) { return e.frame_id < id; });
    if (it == session.frames.end() || it->frame_id != frame_id) return nullptr;
    return &*it;
}

std::vector<std::uint8_t> read_frame_payload(const SessionData& session, const FrameIndexEntry& entry) {
    if (entry.size != session.geometry.frame_bytes())
        throw std::runtime_error("frame " + std::to_string(entry.frame_id) + " index size disagrees with geometry");
    return read_range(join_path(session.dir, "video.y4m"), entry.offset, entry.size);
}

}  // namespace ahpv
