#pragma once
// Structured JSON-lines log. Every record carries an event ID from the catalogue
// in docs/data-schemas.md, the software version and the active session ID.
#include "common/json.hpp"

#include <mutex>
#include <string>

namespace ahpv {

enum class Level { Debug, Info, Warn, Error };

class Logger {
public:
    static Logger& instance();
    void set_file(const std::string& path);  // Optional second sink.
    void set_level(Level level);
    void set_session(const std::string& session_id);
    void set_stderr(bool enabled);
    void log(Level level, const char* event_id, const std::string& message, json::Value details = json::Value());

private:
    Logger() = default;
    std::mutex mutex_;
    std::string session_id_;
    std::string path_;
    int fd_{-1};
    Level level_{Level::Info};
    bool stderr_{true};
};

inline void log_info(const char* id, const std::string& m, json::Value d = {}) { Logger::instance().log(Level::Info, id, m, std::move(d)); }
inline void log_warn(const char* id, const std::string& m, json::Value d = {}) { Logger::instance().log(Level::Warn, id, m, std::move(d)); }
inline void log_error(const char* id, const std::string& m, json::Value d = {}) { Logger::instance().log(Level::Error, id, m, std::move(d)); }

Level parse_level(const std::string& text);

}  // namespace ahpv
