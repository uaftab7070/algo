#include "metrics/histogram.hpp"

#include <algorithm>
#include <cmath>

namespace ahpv {
namespace {
constexpr int kLinear = 128;
constexpr int kSub = 64;
constexpr int kMaxExponent = 40;
constexpr int kBuckets = kLinear + (kMaxExponent - 7 + 1) * kSub;

int bucket_of(std::uint64_t micros) {
    if (micros < static_cast<std::uint64_t>(kLinear)) return static_cast<int>(micros);
    int exponent = 63 - __builtin_clzll(micros);
    if (exponent > kMaxExponent) return kBuckets - 1;
    const int sub = static_cast<int>((micros >> (exponent - 6)) - kSub);
    return kLinear + (exponent - 7) * kSub + sub;
}

std::uint64_t bucket_upper_micros(int index) {
    if (index < kLinear) return static_cast<std::uint64_t>(index);
    const int exponent = (index - kLinear) / kSub + 7;
    const int sub = (index - kLinear) % kSub;
    return (static_cast<std::uint64_t>(kSub + sub + 1) << (exponent - 6)) - 1;
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }
}  // namespace

Histogram::Histogram() : buckets_(kBuckets, 0) {}

void Histogram::add(std::int64_t value_ns) {
    if (value_ns < 0) {
        ++negative_;
        return;
    }
    const std::uint64_t micros = static_cast<std::uint64_t>(value_ns) / 1000;
    ++buckets_[static_cast<std::size_t>(bucket_of(micros))];
    if (count_ == 0 || value_ns < min_) min_ = value_ns;
    if (count_ == 0 || value_ns > max_) max_ = value_ns;
    sum_ += static_cast<double>(value_ns);
    ++count_;
}

std::int64_t Histogram::quantile_ns(double q) const {
    if (count_ == 0) return 0;
    const std::uint64_t rank = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count_))));
    std::uint64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
        seen += buckets_[static_cast<std::size_t>(i)];
        if (seen >= rank) {
            const std::int64_t upper = static_cast<std::int64_t>((bucket_upper_micros(i) + 1) * 1000 - 1);
            return std::clamp(upper, min_, max_);
        }
    }
    return max_;
}

json::Value Histogram::to_json() const {
    json::Value v;
    v.set("count", count_);
    v.set("min_ms", round3(static_cast<double>(min_ns()) / 1e6));
    v.set("mean_ms", round3(mean_ns() / 1e6));
    v.set("p50_ms", round3(static_cast<double>(quantile_ns(0.50)) / 1e6));
    v.set("p95_ms", round3(static_cast<double>(quantile_ns(0.95)) / 1e6));
    v.set("p99_ms", round3(static_cast<double>(quantile_ns(0.99)) / 1e6));
    v.set("max_ms", round3(static_cast<double>(max_ns()) / 1e6));
    v.set("negative_samples", negative_);
    v.set("resolution", "exact to 1 us below 0.128 ms; <=1.6% relative above (bucket upper bound)");
    return v;
}

}  // namespace ahpv
