#include "security/config.hpp"

#include "common/fsutil.hpp"
#include "common/hash.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>

namespace ahpv {
namespace {

// Every accepted key, in canonical output order.
const std::vector<std::string> kKeys = {
    "data_root",
    "source.kind", "source.path", "source.no_signal_timeout_ms", "source.realtime",
    "source.auto_reopen", "source.reopen_attempts", "source.reopen_delay_ms",
    "source.v4l2.width", "source.v4l2.height", "source.v4l2.pixel_format", "source.v4l2.fps", "source.v4l2.buffers",
    "pipeline.queue_capacity", "pipeline.drop_policy", "pipeline.keep_every_n",
    "validation.late_factor", "validation.very_dark_mean", "validation.very_bright_mean", "validation.low_contrast_std",
    "recording.enabled", "recording.fsync_interval_frames", "recording.frame_checksum", "recording.max_session_bytes",
    "recording.segment_max_frames",
    "storage.warn_free_bytes", "storage.reserve_free_bytes",
    "preview.enabled", "preview.bind_address", "preview.port", "preview.allow_non_loopback", "preview.max_width",
    "metrics.sample_interval_ms",
    "security.auth_file", "security.audit_log",
    "annotations.require_independent_review",
    "logging.file", "logging.level",
};

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r");
    return s.substr(begin, end - begin + 1);
}

[[noreturn]] void fail_at(int line, const std::string& message) {
    throw ConfigError(line > 0 ? "line " + std::to_string(line) + ": " + message : message);
}

// Removes a trailing comment outside quotes and unquotes the scalar.
std::string scalar_value(const std::string& raw, int line) {
    std::string value;
    char quote = 0;
    std::size_t i = 0;
    const std::string text = trim(raw);
    if (!text.empty() && (text[0] == '"' || text[0] == '\'')) {
        quote = text[0];
        const auto close = text.find(quote, 1);
        if (close == std::string::npos) fail_at(line, "unterminated quoted value");
        value = text.substr(1, close - 1);
        const std::string rest = trim(text.substr(close + 1));
        if (!rest.empty() && rest[0] != '#') fail_at(line, "unexpected text after quoted value");
        return value;
    }
    for (; i < text.size(); ++i) {
        if (text[i] == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t')) break;
        value.push_back(text[i]);
    }
    value = trim(value);
    if (!value.empty() && (value[0] == '[' || value[0] == '{' || value[0] == '&' || value[0] == '*' || value[0] == '|' || value[0] == '>'))
        fail_at(line, "only plain scalar values are supported");
    return value;
}

class Reader {
public:
    explicit Reader(const FlatConfig& flat) : flat_(flat) {}
    bool has(const std::string& key) const { return flat_.count(key) != 0; }
    int line(const std::string& key) const { return has(key) ? flat_.at(key).second : 0; }

    std::string str(const std::string& key, const std::string& fallback) {
        used_.insert(key);
        return has(key) ? flat_.at(key).first : fallback;
    }
    bool boolean(const std::string& key, bool fallback) {
        const std::string v = str(key, fallback ? "true" : "false");
        if (v == "true") return true;
        if (v == "false") return false;
        fail_at(line(key), key + " must be true or false, got '" + v + "'");
    }
    std::int64_t integer(const std::string& key, std::int64_t fallback, std::int64_t lo, std::int64_t hi) {
        const std::string v = str(key, std::to_string(fallback));
        if (v.empty() || v.size() > 19 || v.find_first_not_of("0123456789") != std::string::npos)
            fail_at(line(key), key + " must be a non-negative integer, got '" + v + "'");
        const std::int64_t n = std::stoll(v);
        if (n < lo || n > hi)
            fail_at(line(key), key + " must be between " + std::to_string(lo) + " and " + std::to_string(hi) + ", got " + v);
        return n;
    }
    double number(const std::string& key, double fallback, double lo, double hi) {
        std::ostringstream def;
        def << fallback;
        const std::string v = str(key, def.str());
        char* end = nullptr;
        const double n = std::strtod(v.c_str(), &end);
        if (v.empty() || !end || *end != '\0' || !std::isfinite(n)) fail_at(line(key), key + " must be a number, got '" + v + "'");
        if (n < lo || n > hi) fail_at(line(key), key + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
        return n;
    }
    std::string choice(const std::string& key, const std::string& fallback, const std::vector<std::string>& options) {
        const std::string v = str(key, fallback);
        if (std::find(options.begin(), options.end(), v) == options.end()) {
            std::string list;
            for (const auto& o : options) list += (list.empty() ? "" : ", ") + o;
            fail_at(line(key), key + " must be one of: " + list + " (got '" + v + "')");
        }
        return v;
    }
    void reject_unused() const {
        for (const auto& [key, entry] : flat_)
            if (!used_.count(key)) fail_at(entry.second, "unknown configuration key '" + key + "'");
    }

private:
    const FlatConfig& flat_;
    std::set<std::string> used_;
};

bool is_ipv4_literal(const std::string& text) {
    int parts = 0;
    std::istringstream in(text);
    std::string part;
    while (std::getline(in, part, '.')) {
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos || std::stoi(part) > 255)
            return false;
        ++parts;
    }
    return parts == 4 && text.back() != '.';
}

}  // namespace

std::vector<std::string> known_config_keys() { return kKeys; }

FlatConfig parse_yaml_subset(const std::string& text) {
    FlatConfig flat;
    std::vector<std::pair<int, std::string>> stack;  // (indent, key)
    std::istringstream in(text);
    std::string raw;
    int line = 0;
    int pending_section_line = 0;
    std::string pending_section;
    while (std::getline(in, raw)) {
        ++line;
        if (raw.find('\t') != std::string::npos && raw.find_first_not_of(" \t") != std::string::npos &&
            raw.find('\t') < raw.find_first_not_of(" \t"))
            fail_at(line, "tabs are not allowed for indentation");
        const std::string content = trim(raw);
        if (content.empty() || content[0] == '#') continue;
        if (content == "---") continue;
        if (content[0] == '-') fail_at(line, "lists are not supported");
        const int indent = static_cast<int>(raw.find_first_not_of(' '));
        if (indent % 2 != 0) fail_at(line, "indentation must be a multiple of two spaces");
        const auto colon = content.find(':');
        if (colon == std::string::npos) fail_at(line, "expected 'key: value'");
        const std::string key = trim(content.substr(0, colon));
        if (key.empty() || !std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '_'; }))
            fail_at(line, "keys must use lowercase letters, digits and '_' (got '" + key + "')");
        while (!stack.empty() && stack.back().first >= indent) stack.pop_back();
        if (!pending_section.empty() && (stack.empty() || stack.back().second != pending_section))
            fail_at(pending_section_line, "section '" + pending_section + "' has no values");
        pending_section.clear();
        if (!stack.empty() && indent != stack.back().first + 2) fail_at(line, "unexpected indentation");
        if (stack.empty() && indent != 0) fail_at(line, "unexpected indentation");
        // Stack entries hold full dotted paths.
        const std::string full = stack.empty() ? key : stack.back().second + "." + key;
        const std::string rest = trim(content.substr(colon + 1));
        if (rest.empty() || rest[0] == '#') {
            stack.emplace_back(indent, full);
            pending_section = full;
            pending_section_line = line;
            continue;
        }
        if (flat.count(full)) fail_at(line, "duplicate key '" + full + "'");
        for (const auto& [existing, entry] : flat)
            if (existing.rfind(full + ".", 0) == 0) fail_at(line, "'" + full + "' is already a section");
        flat[full] = {scalar_value(rest, line), line};
    }
    if (!pending_section.empty()) fail_at(pending_section_line, "section '" + pending_section + "' has no values");
    return flat;
}

Config parse_config(const std::string& text, const std::string& origin) {
    const FlatConfig flat = parse_yaml_subset(text);
    Reader r(flat);
    Config c;
    c.origin = origin;
    c.text_sha256 = sha256_hex(text);
    c.data_root = r.str("data_root", c.data_root);
    if (c.data_root.empty()) fail_at(r.line("data_root"), "data_root must not be empty");
    c.source.kind = r.choice("source.kind", "y4m_file", {"y4m_file", "y4m_stream", "v4l2"});
    c.source.path = r.str("source.path", "");
    c.source.no_signal_timeout_ms = static_cast<int>(r.integer("source.no_signal_timeout_ms", 2000, 100, 60000));
    c.source.realtime = r.boolean("source.realtime", false);
    c.source_auto_reopen = r.boolean("source.auto_reopen", false);
    c.source_reopen_attempts = static_cast<int>(r.integer("source.reopen_attempts", 3, 0, 100));
    c.source_reopen_delay_ms = static_cast<int>(r.integer("source.reopen_delay_ms", 1000, 10, 60000));
    c.source.width = static_cast<int>(r.integer("source.v4l2.width", 0, 0, 8192));
    c.source.height = static_cast<int>(r.integer("source.v4l2.height", 0, 0, 8192));
    c.source.pixel_format = r.choice("source.v4l2.pixel_format", "NV12", {"NV12", "GREY"});
    c.source.fps = static_cast<int>(r.integer("source.v4l2.fps", 0, 0, 240));
    c.source.buffers = static_cast<int>(r.integer("source.v4l2.buffers", 4, 2, 16));
    c.queue_capacity = static_cast<std::size_t>(r.integer("pipeline.queue_capacity", 4, 1, 64));
    c.drop_policy = parse_drop_policy(r.choice("pipeline.drop_policy", "drop_oldest", {"drop_oldest", "drop_newest"}));
    c.keep_every_n = static_cast<std::uint64_t>(r.integer("pipeline.keep_every_n", 1, 1, 60));
    c.validation.late_factor = r.number("validation.late_factor", 1.5, 1.05, 100);
    c.validation.very_dark_mean = r.number("validation.very_dark_mean", 16, 0, 255);
    c.validation.very_bright_mean = r.number("validation.very_bright_mean", 240, 0, 255);
    c.validation.low_contrast_std = r.number("validation.low_contrast_std", 2.0, 0, 128);
    c.recording_enabled = r.boolean("recording.enabled", true);
    c.recorder.fsync_interval_frames = static_cast<int>(r.integer("recording.fsync_interval_frames", 30, 1, 10000));
    c.recorder.frame_checksum = r.choice("recording.frame_checksum", "crc32", {"crc32", "none"}) == "crc32";
    c.recorder.max_session_bytes = static_cast<std::uint64_t>(r.integer("recording.max_session_bytes", 0, 0, INT64_MAX));
    c.segment_max_frames = static_cast<std::uint64_t>(r.integer("recording.segment_max_frames", 18000, 1, 100000000));
    c.recorder.warn_free_bytes = static_cast<std::uint64_t>(r.integer("storage.warn_free_bytes", 512ll << 20, 0, INT64_MAX));
    c.recorder.reserve_free_bytes = static_cast<std::uint64_t>(r.integer("storage.reserve_free_bytes", 64ll << 20, 0, INT64_MAX));
    c.preview_enabled = r.boolean("preview.enabled", false);
    c.preview_bind = r.str("preview.bind_address", c.preview_bind);
    c.preview_port = static_cast<int>(r.integer("preview.port", 8640, 1024, 65535));
    c.preview_allow_non_loopback = r.boolean("preview.allow_non_loopback", false);
    c.preview_max_width = static_cast<int>(r.integer("preview.max_width", 640, 64, 4096));
    c.metrics_interval_ms = static_cast<int>(r.integer("metrics.sample_interval_ms", 5000, 100, 3600000));
    c.auth_file = r.str("security.auth_file", join_path(c.data_root, "auth.conf"));
    c.audit_log = r.str("security.audit_log", join_path(c.data_root, "audit.jsonl"));
    c.require_independent_review = r.boolean("annotations.require_independent_review", true);
    c.log_file = r.str("logging.file", "");
    c.log_level = parse_level(r.choice("logging.level", "info", {"debug", "info", "warn", "error"}));
    r.reject_unused();

    if ((c.source.kind == "y4m_file" || c.source.kind == "y4m_stream" || c.source.kind == "v4l2") && c.source.path.empty())
        fail_at(0, "source.path is required for source.kind " + c.source.kind);
    if (c.source.kind == "v4l2" && (c.source.width == 0 || c.source.height == 0))
        fail_at(0, "source.v4l2.width and source.v4l2.height are required for v4l2 capture");
    if (!is_ipv4_literal(c.preview_bind)) fail_at(r.line("preview.bind_address"), "preview.bind_address must be an IPv4 address literal");
    if (c.preview_bind.rfind("127.", 0) != 0 && !c.preview_allow_non_loopback)
        fail_at(r.line("preview.bind_address"),
                "non-loopback preview.bind_address requires preview.allow_non_loopback: true (SEC-003)");
    if (c.recorder.reserve_free_bytes > c.recorder.warn_free_bytes)
        fail_at(r.line("storage.reserve_free_bytes"), "storage.reserve_free_bytes must not exceed storage.warn_free_bytes");
    if (c.validation.very_dark_mean >= c.validation.very_bright_mean)
        fail_at(r.line("validation.very_dark_mean"), "validation.very_dark_mean must be below validation.very_bright_mean");
    return c;
}

Config load_config(const std::string& path) {
    std::string text;
    try {
        text = read_file(path, 1u << 20);
    } catch (const IoError& error) {
        throw ConfigError(error.what());
    }
    try {
        return parse_config(text, path);
    } catch (const ConfigError& error) {
        throw ConfigError(path + ": " + error.what());
    }
}

std::string set_config_value(const std::string& text, const std::string& key, const std::string& value) {
    if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) throw ConfigError("unknown configuration key '" + key + "'");
    for (const unsigned char c : value)
        if (c < 0x20 || c == 0x7F || c == '"') throw ConfigError("configuration values cannot contain control characters or '\"'");
    FlatConfig flat = parse_yaml_subset(text);
    flat[key] = {value, 0};
    std::string out = "# AL-HARAS passive vision configuration (canonical form written by 'ahpv config set').\n"
                      "# The previous version is retained beside this file. NOT FOR FLIGHT OR ENGAGEMENT CONTROL.\n";
    std::vector<std::string> open;
    for (const auto& k : kKeys) {
        const auto it = flat.find(k);
        if (it == flat.end()) continue;
        std::vector<std::string> parts;
        std::istringstream in(k);
        std::string part;
        while (std::getline(in, part, '.')) parts.push_back(part);
        std::size_t common = 0;
        while (common < open.size() && common + 1 < parts.size() && open[common] == parts[common]) ++common;
        open.resize(common);
        for (std::size_t i = common; i + 1 < parts.size(); ++i) {
            out += std::string(i * 2, ' ') + parts[i] + ":\n";
            open.push_back(parts[i]);
        }
        out += std::string((parts.size() - 1) * 2, ' ') + parts.back() + ": \"" + it->second.first + "\"\n";
    }
    parse_config(out, "<proposed>");  // Reject the change if the result is invalid.
    return out;
}

json::Value Config::to_json() const {
    json::Value v;
    v.set("origin", origin);
    v.set("text_sha256", text_sha256);
    v.set("data_root", data_root);
    json::Value s;
    s.set("kind", source.kind);
    s.set("path", source.path);
    s.set("no_signal_timeout_ms", source.no_signal_timeout_ms);
    s.set("realtime", source.realtime);
    s.set("auto_reopen", source_auto_reopen);
    s.set("reopen_attempts", source_reopen_attempts);
    s.set("reopen_delay_ms", source_reopen_delay_ms);
    s.set("v4l2", json::Object{{"width", source.width}, {"height", source.height}, {"pixel_format", source.pixel_format},
                               {"fps", source.fps}, {"buffers", source.buffers}});
    v.set("source", s);
    v.set("pipeline", json::Object{{"queue_capacity", static_cast<std::int64_t>(queue_capacity)},
                                   {"drop_policy", drop_policy_name(drop_policy)},
                                   {"keep_every_n", static_cast<std::int64_t>(keep_every_n)}});
    v.set("validation", json::Object{{"late_factor", validation.late_factor}, {"very_dark_mean", validation.very_dark_mean},
                                     {"very_bright_mean", validation.very_bright_mean},
                                     {"low_contrast_std", validation.low_contrast_std}});
    v.set("recording", json::Object{{"enabled", recording_enabled}, {"fsync_interval_frames", recorder.fsync_interval_frames},
                                    {"frame_checksum", recorder.frame_checksum ? "crc32" : "none"},
                                    {"max_session_bytes", static_cast<std::int64_t>(recorder.max_session_bytes)},
                                    {"segment_max_frames", static_cast<std::int64_t>(segment_max_frames)}});
    v.set("storage", json::Object{{"warn_free_bytes", static_cast<std::int64_t>(recorder.warn_free_bytes)},
                                  {"reserve_free_bytes", static_cast<std::int64_t>(recorder.reserve_free_bytes)}});
    v.set("preview", json::Object{{"enabled", preview_enabled}, {"bind_address", preview_bind}, {"port", preview_port},
                                  {"allow_non_loopback", preview_allow_non_loopback}, {"max_width", preview_max_width}});
    v.set("metrics", json::Object{{"sample_interval_ms", metrics_interval_ms}});
    v.set("security", json::Object{{"auth_file", auth_file}, {"audit_log", audit_log}});
    v.set("annotations", json::Object{{"require_independent_review", require_independent_review}});
    v.set("logging", json::Object{{"file", log_file}, {"level", log_level == Level::Debug ? "debug" : log_level == Level::Info ? "info" : log_level == Level::Warn ? "warn" : "error"}});
    return v;
}

}  // namespace ahpv
