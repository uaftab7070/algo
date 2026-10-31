#pragma once
// Configuration: a strict YAML subset (nested mappings of scalars, '#' comments,
// two-space indentation). Unknown keys, wrong types and out-of-range values are
// rejected with the file line number (NFR-008). There is deliberately no key for
// any vehicle, serial, MAVLink, PWM or actuator interface.
#include "capture/source.hpp"
#include "common/json.hpp"
#include "common/log.hpp"
#include "frames/bounded_queue.hpp"
#include "frames/frame.hpp"
#include "recording/recorder.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ahpv {

struct ConfigError : std::runtime_error {
    explicit ConfigError(const std::string& message) : std::runtime_error(message) {}
};

struct Config {
    std::string origin;            // File path or "<string>".
    std::string text_sha256;       // Hash of the exact configuration text.
    std::string data_root{"./data"};
    SourceOptions source;
    bool source_auto_reopen{false};
    int source_reopen_attempts{3};
    int source_reopen_delay_ms{1000};
    std::size_t queue_capacity{4};
    std::uint64_t keep_every_n{1};  // Live sources only: configured temporal decimation.
    DropPolicy drop_policy{DropPolicy::DropOldest};
    ValidationThresholds validation;
    bool recording_enabled{true};
    RecorderOptions recorder;
    std::uint64_t segment_max_frames{18000};
    bool preview_enabled{false};
    std::string preview_bind{"127.0.0.1"};
    int preview_port{8640};
    bool preview_allow_non_loopback{false};
    int preview_max_width{640};
    int metrics_interval_ms{5000};
    std::string auth_file;
    std::string audit_log;
    bool require_independent_review{true};
    std::string log_file;
    Level log_level{Level::Info};

    json::Value to_json() const;  // Effective values (paths resolved as configured).
};

using FlatConfig = std::map<std::string, std::pair<std::string, int>>;  // key -> (raw value, line)

FlatConfig parse_yaml_subset(const std::string& text);
Config parse_config(const std::string& text, const std::string& origin);
Config load_config(const std::string& path);
// Returns new canonical text with one key changed; validates the full result.
std::string set_config_value(const std::string& text, const std::string& key, const std::string& value);
std::vector<std::string> known_config_keys();

}  // namespace ahpv
