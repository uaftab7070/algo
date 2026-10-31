#pragma once
// Clock domains are explicit: "mono" is CLOCK_MONOTONIC (ordering, intervals),
// "wall" is CLOCK_REALTIME (human dating only; it may step). See docs/data-schemas.md.
#include <cstdint>
#include <string>

namespace ahpv {

std::int64_t mono_ns();
std::int64_t wall_ns();
// UTC ISO-8601 with milliseconds, e.g. 2026-10-24T08:15:30.123Z.
std::string iso8601_utc(std::int64_t wall_ns);
// Compact UTC stamp for identifiers, e.g. 20261024T081530Z.
std::string compact_utc(std::int64_t wall_ns);

}  // namespace ahpv
