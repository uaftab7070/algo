#pragma once
// Independent integrity checks for a session (AC-004, AC-005, AC-023, NFR-004,
// NFR-009) and non-destructive recovery of sessions interrupted mid-recording.
#include "common/json.hpp"

#include <string>

namespace ahpv {

struct VerifyOptions {
    bool check_payload_crc{true};
};

// Never modifies the session. Result: {"ok": bool, "checks": [...], "summary": {...}}.
json::Value verify_session(const std::string& session_dir, VerifyOptions options = {});

// For a session whose manifest still says "recording": records what is indexed,
// marks status "interrupted", computes checksums of the files as found, and
// appends a recovery event. Never truncates or rewrites recorded data.
json::Value recover_session(const std::string& session_dir);

}  // namespace ahpv
