#include "security/auth.hpp"

#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "recording/session.hpp"

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

namespace ahpv {
namespace {

bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

void check_permissions(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) throw std::runtime_error("authorization file " + path + " does not exist; create principals with 'ahpv auth add'");
    if (!S_ISREG(st.st_mode)) throw std::runtime_error("authorization file " + path + " is not a regular file");
    if (st.st_mode & (S_IWGRP | S_IWOTH | S_IROTH))
        throw std::runtime_error("authorization file " + path + " must not be world-readable or group/world-writable (chmod 600)");
}

}  // namespace

const char* permission_name(Permission permission) {
    switch (permission) {
        case Permission::View: return "view";
        case Permission::Annotate: return "annotate";
        case Permission::Review: return "review";
        case Permission::Record: return "record";
        case Permission::Configure: return "configure";
    }
    return "unknown";
}

bool valid_role(const std::string& role) {
    return role == "viewer" || role == "annotator" || role == "reviewer" || role == "operator";
}

bool role_allows(const std::string& role, Permission permission) {
    if (permission == Permission::View) return valid_role(role);
    if (role == "annotator") return permission == Permission::Annotate;
    if (role == "reviewer") return permission == Permission::Review;
    if (role == "operator") return permission == Permission::Record || permission == Permission::Configure;
    return false;
}

std::string hash_token(const std::string& token) { return sha256_hex(token); }

AuthStore AuthStore::load(const std::string& path) {
    check_permissions(path);
    AuthStore store;
    std::istringstream in(read_file(path, 1u << 20));
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        Entry entry;
        std::string extra;
        if (!(fields >> entry.principal.id >> entry.principal.role >> entry.token_sha256) || (fields >> extra))
            throw std::runtime_error(path + ": line " + std::to_string(number) + " must be '<principal> <role> <sha256>'");
        if (!valid_identifier(entry.principal.id) || !valid_role(entry.principal.role) || entry.token_sha256.size() != 64 ||
            entry.token_sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error(path + ": line " + std::to_string(number) + " has an invalid principal, role or digest");
        store.entries_.push_back(std::move(entry));
    }
    return store;
}

std::optional<Principal> AuthStore::authenticate(const std::string& token) const {
    if (token.empty() || token.size() > 256) return std::nullopt;
    const std::string digest = hash_token(token);
    std::optional<Principal> match;
    // Compare against every entry so timing does not reveal the matching position.
    for (const auto& entry : entries_)
        if (constant_time_equal(digest, entry.token_sha256) && !match) match = entry.principal;
    return match;
}

std::string AuthStore::add_principal(const std::string& path, const std::string& principal, const std::string& role) {
    if (!valid_identifier(principal)) throw std::invalid_argument("principal must match [A-Za-z0-9._-]{1,64}");
    if (!valid_role(role)) throw std::invalid_argument("role must be viewer, annotator, reviewer or operator");
    std::string existing;
    if (path_exists(path)) {
        check_permissions(path);
        existing = read_file(path, 1u << 20);
        const AuthStore store = load(path);
        for (const auto& entry : store.entries_)
            if (entry.principal.id == principal) throw std::invalid_argument("principal '" + principal + "' already exists");
    } else {
        existing = "# principal role sha256(token). Tokens are never stored. Keep this file mode 0600.\n";
    }
    const std::string token = "ahpv_" + random_hex(32);
    if (!existing.empty() && existing.back() != '\n') existing += "\n";
    existing += principal + " " + role + " " + hash_token(token) + "\n";
    make_directories(parent_directory(path));
    write_file_atomic(path, existing, 0600);
    ::chmod(path.c_str(), 0600);
    return token;
}

std::string read_token(const std::string& token_file) {
    std::string token;
    if (!token_file.empty()) {
        const std::string text = read_file(token_file, 4096);
        token = text.substr(0, text.find('\n'));
    } else if (const char* env = std::getenv("AHPV_TOKEN")) {
        token = env;
    }
    while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) token.pop_back();
    return token;
}

}  // namespace ahpv
