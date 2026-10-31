#pragma once
// Per-record integrity seal for append-only logs. "record_sha256" is the SHA-256
// of the record serialised without that field. Together with the previous-record
// chain this detects accidental corruption and edits of any record, including the
// last. It is not a signature: a writer able to recompute digests can forge it.
#include "common/hash.hpp"
#include "common/json.hpp"

namespace ahpv {

inline void seal_record(json::Value& record) {
    record.set("record_sha256", sha256_hex(json::dump(record)));
}

inline bool record_seal_ok(const json::Value& record) {
    const json::Value* seal = record.find("record_sha256");
    if (!seal || !seal->is_string() || !record.is_object() || record.as_object().back().first != "record_sha256") return false;
    json::Value copy = record;
    copy.as_object().pop_back();
    return sha256_hex(json::dump(copy)) == seal->as_string();
}

}  // namespace ahpv
