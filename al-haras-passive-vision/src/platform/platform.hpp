#pragma once
// Read-only platform identity and inventory. Nothing here probes buses, opens
// serial ports, or changes configuration; absent information is reported as null.
#include "common/json.hpp"

#include <string>

namespace ahpv {

std::string executable_path();
std::string executable_sha256();  // Empty when the executable cannot be read.
// Compact identity embedded in manifests and reports (DAT-006).
json::Value platform_summary();
// Full inventory for HWR-001/HWR-004: device tree, memory, storage, video/media
// nodes, network interfaces, serial device names, thermal zones, listeners.
json::Value platform_inventory();
// TCP/UDP listening sockets owned by any process (Linux /proc/net); null elsewhere.
json::Value listening_sockets();

}  // namespace ahpv
