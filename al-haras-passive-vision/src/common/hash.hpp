#pragma once
// SHA-256 (FIPS 180-4) for file/manifest integrity; CRC-32 (IEEE 802.3) for
// cheap per-frame corruption detection on the Cortex-A7. CRC-32 is not a
// security control; tamper evidence relies on the SHA-256 checksum manifest.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ahpv {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }
    std::array<std::uint8_t, 32> finish();
    std::string finish_hex();

private:
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_used_{0};
    std::uint64_t total_bytes_{0};
    bool finished_{false};
    void compress(const std::uint8_t* block);
};

std::string sha256_hex(std::string_view data);
// Throws IoError when the file cannot be read completely.
std::string sha256_file(const std::string& path);

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t previous = 0);
std::string hex32(std::uint32_t value);
std::string to_hex(const std::uint8_t* data, std::size_t size);

}  // namespace ahpv
