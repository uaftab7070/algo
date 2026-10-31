#include "annotations/annotations.hpp"

#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "common/log.hpp"
#include "common/seal.hpp"
#include "common/version.hpp"

#include <cerrno>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/file.h>
#include <unistd.h>

namespace ahpv {
namespace {

const std::string kGenesis(64, '0');
constexpr std::size_t kNotesMax = 500;
constexpr std::size_t kCommentMax = 500;

struct Folded {
    std::vector<std::string> order;
    std::map<std::string, json::Value> current;
    std::vector<json::Value> events;
    bool chain_ok{true};
    std::string last_hash{kGenesis};
};

Folded fold(const std::string& text) {
    Folded f;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        if (end == std::string::npos) {
            f.chain_ok = false;  // An incomplete final line means an interrupted append.
            break;
        }
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        try {
            json::Value event = json::parse(line);
            if (event.at("previous_event_sha256").as_string() != f.last_hash || !record_seal_ok(event)) f.chain_ok = false;
            const std::string id = event.at("annotation_id").as_string();
            if (!f.current.count(id)) f.order.push_back(id);
            f.current[id] = event.at("annotation");
            f.events.push_back(std::move(event));
        } catch (const std::exception&) {
            f.chain_ok = false;
        }
        f.last_hash = sha256_hex(line);
    }
    return f;
}

void allow_only(const json::Value& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) throw AnnotationError(400, "invalid_body", "request body must be a JSON object");
    for (const auto& [key, value] : body.as_object())
        if (!allowed.count(key))
            throw AnnotationError(400, "field_not_permitted",
                                  "field '" + key + "' is not permitted; annotations are display/data records only");
}

std::int64_t expected_revision(const json::Value& body) {
    const json::Value* value = body.find("expected_revision");
    if (!value || !value->is_integer() || value->as_int() < 1)
        throw AnnotationError(400, "expected_revision_required", "expected_revision (positive integer) is required");
    return value->as_int();
}

void check_revision(const json::Value* current, std::int64_t expected, const std::string& id) {
    if (!current) throw AnnotationError(404, "not_found", "annotation '" + id + "' does not exist");
    if (current->at("deleted").as_bool()) throw AnnotationError(409, "deleted", "annotation '" + id + "' is deleted");
    if (current->at("revision").as_int() != expected)
        throw AnnotationError(409, "revision_conflict", "annotation '" + id + "' is at revision " +
                                                            std::to_string(current->at("revision").as_int()) +
                                                            ", not " + std::to_string(expected) + "; reload and retry");
}

}  // namespace

AnnotationStore::AnnotationStore(const SessionData& session, bool require_independent_review)
    : session_(session), require_independent_review_(require_independent_review),
      path_(join_path(session.dir, "annotations.jsonl")) {}

void AnnotationStore::check_region(const json::Value& region, int width, int height) {
    const std::string bounds = "valid bounds: x>=0, y>=0, width>=1, height>=1, x+width<=" + std::to_string(width) +
                               ", y+height<=" + std::to_string(height) + " (source pixels, origin top-left)";
    if (!region.is_array() || region.as_array().size() != 4)
        throw AnnotationError(400, "invalid_region", "region_xywh_pixels must be [x, y, width, height]; " + bounds);
    std::int64_t v[4];
    for (int i = 0; i < 4; ++i) {
        const auto& item = region.as_array()[static_cast<std::size_t>(i)];
        if (!item.is_integer()) throw AnnotationError(400, "invalid_region", "region values must be integers; " + bounds);
        v[i] = item.as_int();
    }
    if (v[0] < 0 || v[1] < 0 || v[2] < 1 || v[3] < 1 || v[0] + v[2] > width || v[1] + v[3] > height)
        throw AnnotationError(400, "region_out_of_bounds",
                              "region [" + std::to_string(v[0]) + "," + std::to_string(v[1]) + "," + std::to_string(v[2]) +
                                  "," + std::to_string(v[3]) + "] is outside the image; " + bounds);
}

std::string AnnotationStore::check_text(const json::Value* value, const char* field, std::size_t max_bytes) {
    if (!value || value->is_null()) return {};
    if (!value->is_string()) throw AnnotationError(400, "invalid_text", std::string(field) + " must be a string");
    const std::string& text = value->as_string();
    if (text.size() > max_bytes)
        throw AnnotationError(400, "text_too_long", std::string(field) + " is limited to " + std::to_string(max_bytes) + " bytes");
    for (const unsigned char c : text)
        if (c < 0x20 || c == 0x7F) throw AnnotationError(400, "invalid_text", std::string(field) + " cannot contain control characters");
    return text;
}

json::Value AnnotationStore::list(bool include_deleted) const {
    std::string text;
    if (path_exists(path_)) text = read_file(path_, 64u << 20);
    const Folded f = fold(text);
    json::Array items;
    for (const auto& id : f.order) {
        const json::Value& record = f.current.at(id);
        if (!include_deleted && record.at("deleted").as_bool()) continue;
        items.push_back(record);
    }
    json::Value out;
    out.set("usage_notice", kUsageNotice);
    out.set("session_id", session_.id);
    out.set("chain_ok", f.chain_ok);
    out.set("events", static_cast<std::int64_t>(f.events.size()));
    out.set("annotations", json::Value(std::move(items)));
    return out;
}

json::Value AnnotationStore::history(const std::string& annotation_id) const {
    std::string text;
    if (path_exists(path_)) text = read_file(path_, 64u << 20);
    const Folded f = fold(text);
    json::Array events;
    for (const auto& e : f.events)
        if (e.at("annotation_id").as_string() == annotation_id) events.push_back(e);
    if (events.empty()) throw AnnotationError(404, "not_found", "annotation '" + annotation_id + "' does not exist");
    json::Value out;
    out.set("annotation_id", annotation_id);
    out.set("chain_ok", f.chain_ok);
    out.set("events", json::Value(std::move(events)));
    return out;
}

json::Value AnnotationStore::append(const Principal& actor, const std::string& event_name, const std::string& annotation_id,
                                    const std::function<json::Value(const json::Value*)>& mutate) {
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0) throw IoError("cannot open " + path_, errno);
    struct Unlock {
        int fd;
        ~Unlock() {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    } guard{fd};
    if (::flock(fd, LOCK_EX) != 0) throw IoError("cannot lock " + path_, errno);
    const std::string text = read_file(path_, 64u << 20);
    const Folded f = fold(text);
    if (!f.chain_ok)
        throw AnnotationError(409, "log_integrity", "annotation log failed its integrity check; writes are refused until reviewed");
    const auto it = f.current.find(annotation_id);
    json::Value record = mutate(it == f.current.end() ? nullptr : &it->second);
    json::Value event;
    event.set("schema_version", kSchemaVersion);
    event.set("event", event_name);
    event.set("event_id", random_hex(8));
    event.set("at", iso8601_utc(wall_ns()));
    event.set("actor_id", actor.id);
    event.set("actor_role", actor.role);
    event.set("annotation_id", annotation_id);
    event.set("previous_event_sha256", f.last_hash);
    event.set("annotation", record);
    seal_record(event);
    const std::string line = json::dump(event) + "\n";
    std::size_t done = 0;
    while (done < line.size()) {
        const ssize_t n = ::write(fd, line.data() + done, line.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw IoError("cannot append annotation", errno);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) throw IoError("cannot fsync annotations", errno);
    log_info("ANN-001", "annotation " + event_name, json::Object{{"annotation_id", annotation_id}, {"actor", actor.id}});
    return record;
}

json::Value AnnotationStore::create(const Principal& actor, const json::Value& body) {
    if (!role_allows(actor.role, Permission::Annotate)) throw AnnotationError(403, "forbidden", "role cannot create annotations");
    allow_only(body, {"frame_id", "region_xywh_pixels", "notes", "annotation_type"});
    const json::Value* type = body.find("annotation_type");
    if (type && (!type->is_string() || type->as_string() != "manual_region_of_interest"))
        throw AnnotationError(400, "invalid_type", "annotation_type must be 'manual_region_of_interest'");
    const json::Value* frame_value = body.find("frame_id");
    if (!frame_value || !frame_value->is_integer() || frame_value->as_int() < 0)
        throw AnnotationError(400, "invalid_frame", "frame_id (non-negative integer) is required");
    const FrameIndexEntry* frame = find_frame(session_, static_cast<std::uint64_t>(frame_value->as_int()));
    if (!frame) throw AnnotationError(400, "unknown_frame", "frame_id " + std::to_string(frame_value->as_int()) + " is not stored in this session");
    const json::Value* region = body.find("region_xywh_pixels");
    if (!region) throw AnnotationError(400, "invalid_region", "region_xywh_pixels is required");
    check_region(*region, session_.geometry.width, session_.geometry.height);
    const std::string notes = check_text(body.find("notes"), "notes", kNotesMax);
    const std::string id = "a-" + random_hex(8);
    return append(actor, "create", id, [&](const json::Value*) {
        const std::string now = iso8601_utc(wall_ns());
        json::Value r;
        r.set("schema_version", kSchemaVersion);
        r.set("annotation_id", id);
        r.set("revision", 1);
        r.set("session_id", session_.id);
        r.set("frame_id", frame->frame_id);
        r.set("source_timestamp_ns", frame->source_timestamp_ns);
        r.set("timestamp_basis", session_.manifest.at("source").at("timestamp_basis").as_string());
        r.set("annotator_id", actor.id);
        r.set("annotation_type", "manual_region_of_interest");
        r.set("region_xywh_pixels", *region);
        r.set("coordinate_space", "source_pixels_origin_top_left");
        r.set("image_size_pixels", json::Value(json::Array{session_.geometry.width, session_.geometry.height}));
        r.set("review_status", "unreviewed");
        r.set("reviewer_id", nullptr);
        r.set("review_comment", nullptr);
        r.set("notes", notes);
        r.set("created_at", now);
        r.set("updated_at", now);
        r.set("last_modified_by", actor.id);
        r.set("deleted", false);
        return r;
    });
}

json::Value AnnotationStore::revise(const Principal& actor, const std::string& id, const json::Value& body) {
    if (!role_allows(actor.role, Permission::Annotate)) throw AnnotationError(403, "forbidden", "role cannot revise annotations");
    allow_only(body, {"expected_revision", "region_xywh_pixels", "notes"});
    const std::int64_t expected = expected_revision(body);
    const json::Value* region = body.find("region_xywh_pixels");
    if (region) check_region(*region, session_.geometry.width, session_.geometry.height);
    const json::Value* notes = body.find("notes");
    const std::string notes_text = check_text(notes, "notes", kNotesMax);
    if (!region && !notes) throw AnnotationError(400, "no_change", "supply region_xywh_pixels and/or notes");
    return append(actor, "revise", id, [&](const json::Value* current) {
        check_revision(current, expected, id);
        json::Value r = *current;
        if (region) r.set("region_xywh_pixels", *region);
        if (notes) r.set("notes", notes_text);
        r.set("revision", expected + 1);
        // Changed content needs a fresh review.
        r.set("review_status", "unreviewed");
        r.set("reviewer_id", nullptr);
        r.set("review_comment", nullptr);
        r.set("updated_at", iso8601_utc(wall_ns()));
        r.set("last_modified_by", actor.id);
        return r;
    });
}

json::Value AnnotationStore::remove(const Principal& actor, const std::string& id, const json::Value& body) {
    if (!role_allows(actor.role, Permission::Annotate)) throw AnnotationError(403, "forbidden", "role cannot delete annotations");
    allow_only(body, {"expected_revision", "reason"});
    const std::int64_t expected = expected_revision(body);
    const std::string reason = check_text(body.find("reason"), "reason", kCommentMax);
    return append(actor, "delete", id, [&](const json::Value* current) {
        check_revision(current, expected, id);
        json::Value r = *current;
        r.set("revision", expected + 1);
        r.set("deleted", true);
        r.set("deletion_reason", reason);
        r.set("updated_at", iso8601_utc(wall_ns()));
        r.set("last_modified_by", actor.id);
        return r;
    });
}

json::Value AnnotationStore::review(const Principal& actor, const std::string& id, const json::Value& body) {
    if (!role_allows(actor.role, Permission::Review)) throw AnnotationError(403, "forbidden", "role cannot review annotations");
    allow_only(body, {"expected_revision", "review_status", "comment"});
    const std::int64_t expected = expected_revision(body);
    const json::Value* status = body.find("review_status");
    if (!status || !status->is_string() ||
        (status->as_string() != "accepted" && status->as_string() != "rejected" && status->as_string() != "needs_revision"))
        throw AnnotationError(400, "invalid_review_status", "review_status must be accepted, rejected or needs_revision");
    const std::string comment = check_text(body.find("comment"), "comment", kCommentMax);
    return append(actor, "review", id, [&](const json::Value* current) {
        check_revision(current, expected, id);
        if (require_independent_review_ &&
            (current->at("annotator_id").as_string() == actor.id || current->at("last_modified_by").as_string() == actor.id))
            throw AnnotationError(409, "independent_review_required", "a reviewer cannot review an annotation they created or revised");
        json::Value r = *current;
        r.set("revision", expected + 1);
        r.set("review_status", status->as_string());
        r.set("reviewer_id", actor.id);
        r.set("review_comment", comment);
        r.set("updated_at", iso8601_utc(wall_ns()));
        return r;
    });
}

}  // namespace ahpv
