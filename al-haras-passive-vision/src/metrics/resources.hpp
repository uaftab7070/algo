#pragma once
// Process and system resource sampling. Linux values come from /proc and /sys;
// values a platform cannot supply are reported as null, never estimated.
#include "common/json.hpp"

#include <cstdint>
#include <string>

namespace ahpv {

class ResourceSampler {
public:
    explicit ResourceSampler(std::string storage_path) : storage_path_(std::move(storage_path)) {}
    // Fields: rss_bytes, peak_rss_bytes, process_cpu_percent, system_cpu_percent,
    // mem_total_bytes, mem_available_bytes, cma_total_bytes, cma_free_bytes,
    // thermal_c{zone:value}, storage_free_bytes, storage_total_bytes, open_fds.
    json::Value sample();

private:
    std::string storage_path_;
    bool has_previous_{false};
    std::int64_t previous_mono_{0};
    double previous_process_cpu_s_{0};
    std::uint64_t previous_sys_busy_{0};
    std::uint64_t previous_sys_total_{0};
};

}  // namespace ahpv
