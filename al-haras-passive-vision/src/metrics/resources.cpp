#include "metrics/resources.hpp"

#include "common/clock.hpp"
#include "common/fsutil.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace ahpv {
namespace {

json::Value nullable(std::int64_t v) { return v >= 0 ? json::Value(v) : json::Value(nullptr); }

#ifdef __linux__
std::int64_t kb_field(const std::string& path, const std::string& key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) == 0 && line.size() > key.size() && line[key.size()] == ':') {
            std::istringstream fields(line.substr(key.size() + 1));
            std::int64_t kb = -1;
            fields >> kb;
            return kb >= 0 ? kb * 1024 : -1;
        }
    }
    return -1;
}
#endif

double process_cpu_seconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

std::int64_t count_open_fds() {
#ifdef __linux__
    const auto names = list_directory("/proc/self/fd");
    return static_cast<std::int64_t>(names.size());
#elif defined(__APPLE__)
    const auto names = list_directory("/dev/fd");
    return static_cast<std::int64_t>(names.size());
#else
    return -1;
#endif
}

}  // namespace

json::Value ResourceSampler::sample() {
    json::Value v;
    const std::int64_t now = mono_ns();
    v.set("at", iso8601_utc(wall_ns()));
    v.set("mono_ns", now);
    std::int64_t rss = -1, peak = -1, mem_total = -1, mem_available = -1, cma_total = -1, cma_free = -1;
    json::Value thermal = json::Object{};
    std::uint64_t sys_busy = 0, sys_total = 0;
    bool have_sys = false;
#ifdef __linux__
    rss = kb_field("/proc/self/status", "VmRSS");
    peak = kb_field("/proc/self/status", "VmHWM");
    mem_total = kb_field("/proc/meminfo", "MemTotal");
    mem_available = kb_field("/proc/meminfo", "MemAvailable");
    cma_total = kb_field("/proc/meminfo", "CmaTotal");
    cma_free = kb_field("/proc/meminfo", "CmaFree");
    {
        std::ifstream stat("/proc/stat");
        std::string cpu;
        std::uint64_t values[8] = {};
        if (stat >> cpu && cpu == "cpu") {
            for (auto& value : values) stat >> value;
            for (const auto value : values) sys_total += value;
            sys_busy = sys_total - values[3] - values[4];  // minus idle and iowait
            have_sys = true;
        }
    }
    for (const auto& zone : list_directory("/sys/class/thermal")) {
        if (zone.rfind("thermal_zone", 0) != 0) continue;
        std::ifstream temp("/sys/class/thermal/" + zone + "/temp");
        std::ifstream type("/sys/class/thermal/" + zone + "/type");
        std::int64_t milli = 0;
        std::string name = zone;
        if (type) std::getline(type, name);
        if (temp >> milli) thermal.set(zone + ":" + name, static_cast<double>(milli) / 1000.0);
    }
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        rss = static_cast<std::int64_t>(info.resident_size);
        peak = static_cast<std::int64_t>(info.resident_size_max);
    }
    std::uint64_t memsize = 0;
    std::size_t size = sizeof memsize;
    if (sysctlbyname("hw.memsize", &memsize, &size, nullptr, 0) == 0) mem_total = static_cast<std::int64_t>(memsize);
#endif
    const double cpu_s = process_cpu_seconds();
    json::Value process_cpu = nullptr, system_cpu = nullptr;
    if (has_previous_ && now > previous_mono_) {
        const double wall_s = static_cast<double>(now - previous_mono_) / 1e9;
        process_cpu = std::round((cpu_s - previous_process_cpu_s_) / wall_s * 1000.0) / 10.0;
        if (have_sys && sys_total > previous_sys_total_)
            system_cpu = std::round(static_cast<double>(sys_busy - previous_sys_busy_) /
                                    static_cast<double>(sys_total - previous_sys_total_) * 1000.0) / 10.0;
    }
    has_previous_ = true;
    previous_mono_ = now;
    previous_process_cpu_s_ = cpu_s;
    previous_sys_busy_ = sys_busy;
    previous_sys_total_ = sys_total;
    const StorageStatus storage = storage_status(storage_path_);
    v.set("rss_bytes", nullable(rss));
    v.set("peak_rss_bytes", nullable(peak));
    v.set("process_cpu_seconds", std::round(cpu_s * 1000.0) / 1000.0);
    v.set("process_cpu_percent", process_cpu);
    v.set("system_cpu_percent", system_cpu);
    v.set("mem_total_bytes", nullable(mem_total));
    v.set("mem_available_bytes", nullable(mem_available));
    v.set("cma_total_bytes", nullable(cma_total));
    v.set("cma_free_bytes", nullable(cma_free));
    v.set("thermal_c", thermal);
    v.set("storage_free_bytes", storage.available ? json::Value(storage.free_bytes) : json::Value(nullptr));
    v.set("storage_total_bytes", storage.available ? json::Value(storage.total_bytes) : json::Value(nullptr));
    v.set("open_fds", nullable(count_open_fds()));
    return v;
}

}  // namespace ahpv
