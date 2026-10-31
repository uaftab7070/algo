#pragma once
// Append-only, hash-chained audit log: each record carries the SHA-256 of the
// previous record's line and its own seal (common/seal.hpp), so edits, deletions
// and reordering are detectable. Truncating trailing records is detectable only
// by comparing record counts with an external copy (docs/security-boundaries.md).
#include "common/json.hpp"

#include <mutex>
#include <string>

namespace ahpv {

class AuditLog {
public:
    explicit AuditLog(std::string path);
    // outcome: "allowed" | "denied" | "failed".
    void record(const std::string& principal, const std::string& action, const std::string& resource,
                const std::string& outcome, const std::string& reason, json::Value details = json::Value());
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::string previous_hash_;
    std::mutex mutex_;
};

// Returns {"ok": bool, "records": n, "first_bad_line": n|null}.
json::Value verify_audit_chain(const std::string& path);

}  // namespace ahpv
