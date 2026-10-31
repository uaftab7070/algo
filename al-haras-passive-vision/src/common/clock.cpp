#include "common/clock.hpp"

#include <cstdio>
#include <ctime>

namespace ahpv {
namespace {
std::int64_t read_clock(clockid_t id) {
    timespec ts{};
    clock_gettime(id, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

std::tm utc(std::int64_t wall_ns, long* millis) {
    std::int64_t seconds = wall_ns / 1000000000LL;
    std::int64_t remainder = wall_ns % 1000000000LL;
    if (remainder < 0) {
        remainder += 1000000000LL;
        --seconds;
    }
    if (millis) *millis = static_cast<long>(remainder / 1000000LL);
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm out{};
    gmtime_r(&t, &out);
    return out;
}
}  // namespace

std::int64_t mono_ns() { return read_clock(CLOCK_MONOTONIC); }
std::int64_t wall_ns() { return read_clock(CLOCK_REALTIME); }

std::string iso8601_utc(std::int64_t wall_ns) {
    long millis = 0;
    const std::tm t = utc(wall_ns, &millis);
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", t.tm_year + 1900, t.tm_mon + 1,
                  t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, millis);
    return buffer;
}

std::string compact_utc(std::int64_t wall_ns) {
    const std::tm t = utc(wall_ns, nullptr);
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%04d%02d%02dT%02d%02d%02dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    return buffer;
}

}  // namespace ahpv
