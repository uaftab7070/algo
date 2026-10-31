#pragma once
// Role-based authorization (SEC-001, FR-015). Tokens are 256-bit random values;
// only their SHA-256 digests are stored, in a file that must not be group- or
// world-writable. Who may edit that file is an operating-system decision.
#include <optional>
#include <string>
#include <vector>

namespace ahpv {

enum class Permission { View, Annotate, Review, Record, Configure };
const char* permission_name(Permission permission);

struct Principal {
    std::string id;
    std::string role;  // viewer | annotator | reviewer | operator
};

bool role_allows(const std::string& role, Permission permission);
bool valid_role(const std::string& role);

class AuthStore {
public:
    AuthStore() = default;
    // Throws std::runtime_error on unsafe permissions or malformed entries.
    static AuthStore load(const std::string& path);
    std::optional<Principal> authenticate(const std::string& token) const;
    bool empty() const { return entries_.empty(); }
    // Adds a principal and returns its new token (shown once). Creates the file 0600.
    static std::string add_principal(const std::string& path, const std::string& principal, const std::string& role);

private:
    struct Entry {
        Principal principal;
        std::string token_sha256;
    };
    std::vector<Entry> entries_;
};

std::string hash_token(const std::string& token);
// Reads a token from a file (first line) or the AHPV_TOKEN environment variable.
std::string read_token(const std::string& token_file);

}  // namespace ahpv
