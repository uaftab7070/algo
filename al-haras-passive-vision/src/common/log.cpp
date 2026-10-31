#include "common/log.hpp"

#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/version.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace ahpv {
namespace {
const char* level_name(Level level) {
    switch (level) {
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
    }
    return "error";
}

void write_line(int fd, const std::string& line) {
    std::size_t done = 0;
    while (done < line.size()) {
        const ssize_t n = ::write(fd, line.data() + done, line.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;  // A failing log sink must never stop capture.
        }
        done += static_cast<std::size_t>(n);
    }
}
}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    path_ = path;
    if (!path.empty()) {
        make_directories(parent_directory(path));
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (fd_ < 0) throw IoError("cannot open log file " + path, errno);
    }
}

void Logger::set_level(Level level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

void Logger::set_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_id_ = session_id;
}

void Logger::set_stderr(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    stderr_ = enabled;
}

void Logger::log(Level level, const char* event_id, const std::string& message, json::Value details) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(level_)) return;
    const std::int64_t wall = wall_ns();
    json::Value record;
    record.set("ts", iso8601_utc(wall));
    record.set("mono_ns", mono_ns());
    record.set("level", level_name(level));
    record.set("event_id", event_id);
    record.set("software_version", kVersion);
    record.set("session_id", session_id_.empty() ? json::Value(nullptr) : json::Value(session_id_));
    record.set("message", message);
    if (!details.is_null()) record.set("details", std::move(details));
    const std::string line = json::dump(record) + "\n";
    if (stderr_) write_line(STDERR_FILENO, line);
    if (fd_ >= 0) write_line(fd_, line);
}

Level parse_level(const std::string& text) {
    if (text == "debug") return Level::Debug;
    if (text == "info") return Level::Info;
    if (text == "warn") return Level::Warn;
    if (text == "error") return Level::Error;
    throw std::invalid_argument("unknown log level '" + text + "'");
}

}  // namespace ahpv
