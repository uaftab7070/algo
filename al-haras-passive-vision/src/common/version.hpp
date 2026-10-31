#pragma once

#ifndef AHPV_VERSION
#define AHPV_VERSION "0.1.0-dev"
#endif
#ifndef AHPV_BUILD_ID
#define AHPV_BUILD_ID "unrecorded"
#endif

namespace ahpv {
inline constexpr const char* kSoftwareName = "al-haras-passive-vision";
inline constexpr const char* kVersion = AHPV_VERSION;
inline constexpr const char* kBuildId = AHPV_BUILD_ID;
// Required on every observation output (brief section 2.2).
inline constexpr const char* kUsageNotice = "NOT FOR FLIGHT OR ENGAGEMENT CONTROL";
inline constexpr const char* kSchemaVersion = "1.0";
}  // namespace ahpv
