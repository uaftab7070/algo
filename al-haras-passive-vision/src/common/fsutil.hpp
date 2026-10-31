#pragma once
// Durable file helpers. Every failure carries errno so storage exhaustion
// (ENOSPC/EDQUOT) can be distinguished from other I/O faults.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ahpv {

struct IoError : std::runtime_error {
    IoError(const std::string& message, int error);
    int error_number;
    bool storage_exhausted() const;
};

std::string read_file(const std::string& path, std::size_t max_bytes = 64u << 20);
// Writes a temporary sibling, fsyncs it, renames over path and fsyncs the directory.
void write_file_atomic(const std::string& path, std::string_view data, unsigned mode = 0644);
// Creates exactly one directory; returns false if it already exists.
bool make_directory_exclusive(const std::string& path, unsigned mode = 0750);
void make_directories(const std::string& path, unsigned mode = 0750);
bool path_exists(const std::string& path);
bool is_directory(const std::string& path);
std::int64_t file_size(const std::string& path);  // -1 when absent.
std::vector<std::string> list_directory(const std::string& path);  // Sorted names, no dot entries.
void fsync_directory(const std::string& path);
std::string join_path(const std::string& a, const std::string& b);
std::string parent_directory(const std::string& path);

struct StorageStatus {
    bool available{false};
    std::uint64_t free_bytes{0};    // Available to unprivileged writers.
    std::uint64_t total_bytes{0};
};
StorageStatus storage_status(const std::string& path);

// Append-only file with full-write and explicit fsync semantics.
class AppendFile {
public:
    AppendFile() = default;
    explicit AppendFile(const std::string& path, unsigned mode = 0640);
    AppendFile(const AppendFile&) = delete;
    AppendFile& operator=(const AppendFile&) = delete;
    AppendFile(AppendFile&& other) noexcept;
    AppendFile& operator=(AppendFile&& other) noexcept;
    ~AppendFile();

    bool is_open() const { return fd_ >= 0; }
    void write(const void* data, std::size_t size);
    void write(std::string_view text) { write(text.data(), text.size()); }
    void sync();
    void close();
    std::uint64_t size() const { return size_; }
    const std::string& path() const { return path_; }

private:
    int fd_{-1};
    std::uint64_t size_{0};
    std::string path_;
};

// Reads [offset, offset+size) exactly; throws IoError on short read.
std::vector<std::uint8_t> read_range(const std::string& path, std::uint64_t offset, std::size_t size);

}  // namespace ahpv
