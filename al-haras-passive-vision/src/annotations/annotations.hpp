#pragma once
// Manual region-of-interest annotations for laboratory footage (FR-005, FR-006,
// DAT-003, DAT-004). Annotations are display/data artifacts: the accepted field
// set is closed, so no steering offset, aim point or command can be stored, and
// nothing in this module produces any output other than records.
//
// Storage: <session>/annotations.jsonl, one event per line, hash-chained and
// never rewritten. Current state is derived by replaying the log.
#include "common/json.hpp"
#include "recording/session.hpp"
#include "security/auth.hpp"

#include <functional>
#include <stdexcept>
#include <string>

namespace ahpv {

struct AnnotationError : std::runtime_error {
    AnnotationError(int status, std::string code_name, const std::string& message)
        : std::runtime_error(message), http_status(status), code(std::move(code_name)) {}
    int http_status;
    std::string code;
};

class AnnotationStore {
public:
    AnnotationStore(const SessionData& session, bool require_independent_review);

    // {"annotations": [...current records...], "chain_ok": bool, "events": n}
    json::Value list(bool include_deleted) const;
    json::Value history(const std::string& annotation_id) const;
    // body: {frame_id, region_xywh_pixels, notes?, annotation_type?}
    json::Value create(const Principal& actor, const json::Value& body);
    // body: {expected_revision, region_xywh_pixels?, notes?}
    json::Value revise(const Principal& actor, const std::string& annotation_id, const json::Value& body);
    // body: {expected_revision, reason?}
    json::Value remove(const Principal& actor, const std::string& annotation_id, const json::Value& body);
    // body: {expected_revision, review_status: accepted|rejected|needs_revision, comment?}
    json::Value review(const Principal& actor, const std::string& annotation_id, const json::Value& body);

    // Shared validation, exposed for tests.
    static void check_region(const json::Value& region, int width, int height);
    static std::string check_text(const json::Value* value, const char* field, std::size_t max_bytes);

private:
    const SessionData& session_;
    bool require_independent_review_;
    std::string path_;

    json::Value append(const Principal& actor, const std::string& event, const std::string& annotation_id,
                       const std::function<json::Value(const json::Value* current)>& mutate);
};

}  // namespace ahpv
