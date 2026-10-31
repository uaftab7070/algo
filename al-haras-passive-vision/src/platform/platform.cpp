#include "platform/platform.hpp"

#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "common/version.hpp"

#include <climits>
#include <fstream>
#include <sstream>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

namespace ahpv {
namespace {

json::Value read_text(const std::string& path, std::size_t limit = 4096) {
    try {
        std::string text = read_file(path, 1u << 20);
        // Device-tree strings are NUL-separated.
        for (auto& c : text)
            if (c == '\0') c = ' ';
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        if (text.size() > limit) text.resize(limit);
        return json::Value(text);
    } catch (const std::exception&) {
        return json::Value(nullptr);
    }
}

json::Value lines_of(const std::string& path, std::size_t max_lines = 200) {
    try {
        std::istringstream in(read_file(path, 1u << 20));
        json::Array out;
        std::string line;
        while (std::getline(in, line) && out.size() < max_lines) out.emplace_back(line);
        return json::Value(std::move(out));
    } catch (const std::exception&) {
        return json::Value(nullptr);
    }
}

json::Value names_matching(const std::string& dir, const std::string& prefix) {
    json::Array out;
    for (const auto& name : list_directory(dir))
        if (name.rfind(prefix, 0) == 0) out.emplace_back(name);
    return json::Value(std::move(out));
}

std::string os_release_name() {
    try {
        std::istringstream in(read_file("/etc/os-release", 65536));
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                std::string value = line.substr(12);
                if (value.size() >= 2 && value.front() == '"') value = value.substr(1, value.size() - 2);
                return value;
            }
    } catch (const std::exception&) {
    }
#ifdef __APPLE__
    char version[64] = {};
    std::size_t size = sizeof version;
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) == 0) return std::string("macOS ") + version;
#endif
    return {};
}

}  // namespace

std::string executable_path() {
#ifdef __linux__
    char buffer[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n > 0) return std::string(buffer, static_cast<std::size_t>(n));
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    std::uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        char resolved[PATH_MAX];
        if (::realpath(buffer, resolved)) return resolved;
        return buffer;
    }
#endif
    return {};
}

std::string executable_sha256() {
    try {
        const std::string path = executable_path();
        return path.empty() ? std::string() : sha256_file(path);
    } catch (const std::exception&) {
        return {};
    }
}

json::Value platform_summary() {
    utsname u{};
    ::uname(&u);
    json::Value v;
    v.set("os", u.sysname);
    v.set("os_release", os_release_name());
    v.set("kernel_release", u.release);
    v.set("kernel_version", u.version);
    v.set("machine", u.machine);
    v.set("device_tree_model", read_text("/proc/device-tree/model"));
    v.set("device_tree_compatible", read_text("/proc/device-tree/compatible"));
    v.set("is_target_hardware_verified", false);
    v.set("note", "Board identity must be confirmed against the physical unit (HWR-001).");
    return v;
}

json::Value listening_sockets() {
#ifdef __linux__
    json::Array out;
    for (const char* table : {"/proc/net/tcp", "/proc/net/tcp6", "/proc/net/udp", "/proc/net/udp6"}) {
        std::ifstream in(table);
        std::string line;
        std::getline(in, line);  // header
        const bool tcp = std::string(table).find("tcp") != std::string::npos;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::string slot, local, remote, state;
            fields >> slot >> local >> remote >> state;
            // TCP LISTEN is state 0A; unconnected UDP sockets are state 07.
            if ((tcp && state != "0A") || (!tcp && state != "07")) continue;
            const auto colon = local.rfind(':');
            json::Value s;
            s.set("table", table);
            s.set("local_address_hex", local.substr(0, colon));
            s.set("port", static_cast<std::int64_t>(std::stol(local.substr(colon + 1), nullptr, 16)));
            out.push_back(std::move(s));
        }
    }
    return json::Value(std::move(out));
#else
    return json::Value(nullptr);
#endif
}

json::Value platform_inventory() {
    json::Value v;
    v.set("schema_version", kSchemaVersion);
    v.set("usage_notice", kUsageNotice);
    v.set("collected_at", iso8601_utc(wall_ns()));
    v.set("collector", std::string(kSoftwareName) + " " + kVersion);
    v.set("platform", platform_summary());
    v.set("cpuinfo", lines_of("/proc/cpuinfo", 60));
    v.set("meminfo", lines_of("/proc/meminfo", 60));
    v.set("kernel_cmdline", read_text("/proc/cmdline"));
    v.set("mtd_partitions", lines_of("/proc/mtd"));
    v.set("block_partitions", lines_of("/proc/partitions"));
    v.set("mounts", lines_of("/proc/mounts"));
    v.set("video_nodes", names_matching("/dev", "video"));
    v.set("media_nodes", names_matching("/dev", "media"));
    json::Value v4l2_names;
    for (const auto& node : list_directory("/sys/class/video4linux"))
        v4l2_names.set(node, read_text("/sys/class/video4linux/" + node + "/name"));
    v.set("video4linux_names", v4l2_names.is_null() ? json::Value(json::Object{}) : v4l2_names);
    v.set("network_interfaces", names_matching("/sys/class/net", ""));
    // Names only, for the interface audit; this application never opens them.
    json::Array serial;
    for (const char* prefix : {"ttyS", "ttyFIQ", "ttyUSB", "ttyACM", "ttyGS"}) {
        const json::Value names = names_matching("/dev", prefix);
        for (const auto& name : names.as_array()) serial.push_back(name);
    }
    v.set("serial_device_names", json::Value(std::move(serial)));
    json::Value thermal;
    for (const auto& zone : list_directory("/sys/class/thermal"))
        if (zone.rfind("thermal_zone", 0) == 0) thermal.set(zone, read_text("/sys/class/thermal/" + zone + "/type"));
    v.set("thermal_zones", thermal.is_null() ? json::Value(json::Object{}) : thermal);
    json::Value usb;
    for (const auto& device : list_directory("/sys/bus/usb/devices")) {
        const json::Value product = read_text("/sys/bus/usb/devices/" + device + "/product");
        if (!product.is_null()) usb.set(device, product);
    }
    v.set("usb_devices", usb.is_null() ? json::Value(json::Object{}) : usb);
    v.set("rockchip_npu_node_present", path_exists("/dev/rknpu") || path_exists("/sys/class/misc/rknpu"));
    v.set("listening_sockets", listening_sockets());
    return v;
}

}  // namespace ahpv
