#include "security/audit.hpp"

#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "common/log.hpp"
#include "common/seal.hpp"
#include "common/version.hpp"

#include <sstream>

namespace ahpv {
namespace {
const std::string kGenesis(64, '0');

std::string last_line(const std::string& path) {
    const std::int64_t size = file_size(path);
    if (size <= 0) return {};
    const std::uint64_t window = static_cast<std::uint64_t>(std::min<std::int64_t>(size, 65536));
    const auto bytes = read_range(path, static_cast<std::uint64_t>(size) - window, window);
    std::string text(bytes.begin(), bytes.end());
    while (!text.empty() && text.back() == '\n') text.pop_back();
    const auto newline = text.rfind('\n');
    return newline == std::string::npos ? text : text.substr(newline + 1);
}
}  // namespace

AuditLog::AuditLog(std::string path) : path_(std::move(path)) {
    make_directories(parent_directory(path_));
    const std::string last = last_line(path_);
    previous_hash_ = last.empty() ? kGenesis : sha256_hex(last);
}

void AuditLog::record(const std::string& principal, const std::string& action, const std::string& resource,
                      const std::string& outcome, const std::string& reason, json::Value details) {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value entry;
    entry.set("at", iso8601_utc(wall_ns()));
    entry.set("mono_ns", mono_ns());
    entry.set("software_version", kVersion);
    entry.set("principal", principal.empty() ? "unauthenticated" : principal);
    entry.set("action", action);
    entry.set("resource", resource);
    entry.set("outcome", outcome);
    entry.set("reason", reason);
    if (!details.is_null()) entry.set("details", std::move(details));
    entry.set("previous_sha256", previous_hash_);
    seal_record(entry);
    const std::string line = json::dump(entry);
    try {
        AppendFile file(path_, 0640);
        file.write(line + "\n");
        file.sync();
        previous_hash_ = sha256_hex(line);
    } catch (const IoError& error) {
        log_error("SEC-090", std::string("audit log write failed: ") + error.what(),
                  json::Object{{"action", action}, {"outcome", outcome}});
        throw;
    }
    log_info(outcome == "denied" ? "SEC-020" : "SEC-010", "audit: " + action + " " + outcome,
             json::Object{{"principal", principal.empty() ? "unauthenticated" : principal}, {"resource", resource}});
}

json::Value verify_audit_chain(const std::string& path) {
    json::Value result;
    std::istringstream in(read_file(path, 256u << 20));
    std::string line, expected = kGenesis;
    std::int64_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        bool ok = false;
        try {
            const json::Value entry = json::parse(line);
            ok = entry.at("previous_sha256").as_string() == expected && record_seal_ok(entry);
        } catch (const std::exception&) {
        }
        if (!ok) {
            result.set("ok", false);
            result.set("records", number - 1);
            result.set("first_bad_line", number);
            return result;
        }
        expected = sha256_hex(line);
    }
    result.set("ok", true);
    result.set("records", number);
    result.set("first_bad_line", nullptr);
    return result;
}

}  // namespace ahpv
