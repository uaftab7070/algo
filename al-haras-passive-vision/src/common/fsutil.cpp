#include "common/fsutil.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace ahpv {

IoError::IoError(const std::string& message, int error)
    : std::runtime_error(message + ": " + std::strerror(error)), error_number(error) {}

bool IoError::storage_exhausted() const {
#ifdef EDQUOT
    if (error_number == EDQUOT) return true;
#endif
    return error_number == ENOSPC;
}

namespace {
void write_all(int fd, const void* data, std::size_t size, const std::string& path) {
    const auto* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::write(fd, p, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw IoError("write failed for " + path, errno);
        }
        p += n;
        size -= static_cast<std::size_t>(n);
    }
}
}  // namespace

std::string read_file(const std::string& path, std::size_t max_bytes) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw IoError("cannot open " + path, errno);
    std::string out;
    char buffer[1 << 15];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int error = errno;
            ::close(fd);
            throw IoError("cannot read " + path, error);
        }
        if (n == 0) break;
        if (out.size() + static_cast<std::size_t>(n) > max_bytes) {
            ::close(fd);
            throw IoError("file exceeds size limit: " + path, EFBIG);
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

void fsync_directory(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw IoError("cannot open directory " + path, errno);
    // Some filesystems reject directory fsync with EINVAL; that is not data loss.
    if (::fsync(fd) != 0 && errno != EINVAL) {
        const int error = errno;
        ::close(fd);
        throw IoError("cannot fsync directory " + path, error);
    }
    ::close(fd);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string parent_directory(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

void write_file_atomic(const std::string& path, std::string_view data, unsigned mode) {
    const std::string temporary = path + ".tmp-" + std::to_string(::getpid());
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) throw IoError("cannot create " + temporary, errno);
    try {
        write_all(fd, data.data(), data.size(), temporary);
        if (::fsync(fd) != 0) throw IoError("cannot fsync " + temporary, errno);
    } catch (...) {
        ::close(fd);
        ::unlink(temporary.c_str());
        throw;
    }
    ::close(fd);
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const int error = errno;
        ::unlink(temporary.c_str());
        throw IoError("cannot replace " + path, error);
    }
    fsync_directory(parent_directory(path));
}

bool make_directory_exclusive(const std::string& path, unsigned mode) {
    if (::mkdir(path.c_str(), mode) == 0) {
        fsync_directory(parent_directory(path));
        return true;
    }
    if (errno == EEXIST) return false;
    throw IoError("cannot create directory " + path, errno);
}

void make_directories(const std::string& path, unsigned mode) {
    if (path.empty() || is_directory(path)) return;
    const std::string parent = parent_directory(path);
    if (parent != path && parent != "." && parent != "/") make_directories(parent, mode);
    if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) throw IoError("cannot create directory " + path, errno);
}

bool path_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::int64_t file_size(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return static_cast<std::int64_t>(st.st_size);
}

std::vector<std::string> list_directory(const std::string& path) {
    std::vector<std::string> names;
    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        if (errno == ENOENT) return names;
        throw IoError("cannot list " + path, errno);
    }
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

StorageStatus storage_status(const std::string& path) {
    struct statvfs st{};
    StorageStatus status;
    if (::statvfs(path.c_str(), &st) != 0) return status;
    status.available = true;
    status.free_bytes = static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
    status.total_bytes = static_cast<std::uint64_t>(st.f_blocks) * st.f_frsize;
    return status;
}

AppendFile::AppendFile(const std::string& path, unsigned mode) : path_(path) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, mode);
    if (fd_ < 0) throw IoError("cannot open " + path + " for append", errno);
    struct stat st{};
    if (::fstat(fd_, &st) == 0) size_ = static_cast<std::uint64_t>(st.st_size);
}

AppendFile::AppendFile(AppendFile&& other) noexcept : fd_(other.fd_), size_(other.size_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

AppendFile& AppendFile::operator=(AppendFile&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        size_ = other.size_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

AppendFile::~AppendFile() {
    if (fd_ >= 0) ::close(fd_);
}

void AppendFile::write(const void* data, std::size_t size) {
    if (fd_ < 0) throw IoError("write to closed file " + path_, EBADF);
    write_all(fd_, data, size, path_);
    size_ += size;
}

void AppendFile::sync() {
    if (fd_ >= 0 && ::fsync(fd_) != 0) throw IoError("cannot fsync " + path_, errno);
}

void AppendFile::close() {
    if (fd_ >= 0) {
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0) throw IoError("cannot close " + path_, errno);
    }
}

std::vector<std::uint8_t> read_range(const std::string& path, std::uint64_t offset, std::size_t size) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw IoError("cannot open " + path, errno);
    std::vector<std::uint8_t> out(size);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd, out.data() + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            const int error = errno;
            ::close(fd);
            throw IoError("cannot read " + path, error);
        }
        if (n == 0) {
            ::close(fd);
            throw IoError("short read from " + path, EIO);
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return out;
}

}  // namespace ahpv
