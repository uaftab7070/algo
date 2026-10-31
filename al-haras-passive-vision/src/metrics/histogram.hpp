#pragma once
// Fixed-memory log-linear histogram (microsecond buckets, ~1.6% resolution above
// 128 us, exact below). A 24-hour soak at 30 fps must not retain every sample on
// a 256 MB board. Quantiles report the bucket's upper bound, capped at the maximum.
#include "common/json.hpp"

#include <cstdint>
#include <vector>

namespace ahpv {

class Histogram {
public:
    Histogram();
    void add(std::int64_t value_ns);
    std::uint64_t count() const { return count_; }
    std::int64_t min_ns() const { return count_ ? min_ : 0; }
    std::int64_t max_ns() const { return count_ ? max_ : 0; }
    double mean_ns() const { return count_ ? sum_ / static_cast<double>(count_) : 0.0; }
    std::int64_t quantile_ns(double q) const;
    // Milliseconds: {count, min_ms, mean_ms, p50_ms, p95_ms, p99_ms, max_ms, resolution}.
    json::Value to_json() const;

private:
    std::vector<std::uint64_t> buckets_;
    std::uint64_t count_{0};
    std::int64_t min_{0};
    std::int64_t max_{0};
    double sum_{0};
    std::uint64_t negative_{0};
};

}  // namespace ahpv
