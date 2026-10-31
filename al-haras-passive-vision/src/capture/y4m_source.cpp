#include "capture/y4m_source.hpp"

#include "common/clock.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace ahpv {
namespace {

int parse_positive(const std::string& text, const char* what) {
    if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(std::string("Y4M header has invalid ") + what + " '" + text + "'");
    const int value = std::stoi(text);
    if (value <= 0) throw std::invalid_argument(std::string("Y4M header ") + what + " must be positive");
    return value;
}

// index * den / num seconds, in ns, without 128-bit arithmetic (unavailable on
// 32-bit ARM): num and den are below 1e9 (parse_positive), so no step overflows.
std::int64_t nominal_time_ns(std::uint64_t index, int num, int den) {
    if (num <= 0) return 0;
    const std::uint64_t ticks = index * static_cast<std::uint64_t>(den);
    const std::uint64_t whole = ticks / static_cast<std::uint64_t>(num);
    const std::uint64_t remainder = ticks % static_cast<std::uint64_t>(num);
    return static_cast<std::int64_t>(whole * 1000000000ULL + remainder * 1000000000ULL / static_cast<std::uint64_t>(num));
}

// Opens a stream path for reading. A FIFO is opened with an ordinary blocking
// open on a helper thread (non-blocking FIFO opens do not reliably wake waiting
// writers on every kernel); on timeout the helper is released by briefly
// opening the write end. Returns -1 with errno ETIMEDOUT when no producer appears.
int open_stream(const std::string& path, int timeout_ms) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -1;
    if (!S_ISFIFO(st.st_mode)) return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    std::atomic<int> fd{-2};
    int open_errno = 0;
    std::thread opener([&] {
        const int result = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        open_errno = errno;
        fd = result;
    });
    const std::int64_t deadline = mono_ns() + static_cast<std::int64_t>(timeout_ms) * 1000000;
    while (fd.load() == -2 && mono_ns() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (fd.load() == -2) {
        const int releaser = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        opener.join();
        if (releaser >= 0) ::close(releaser);
        if (fd.load() >= 0) ::close(fd.load());
        errno = ETIMEDOUT;
        return -1;
    }
    opener.join();
    if (fd.load() < 0) errno = open_errno;
    return fd.load();
}

}  // namespace

std::int64_t SourceInfo::nominal_interval_ns() const {
    if (fps_num <= 0 || fps_den <= 0) return 0;
    return static_cast<std::int64_t>(1000000000.0 * fps_den / fps_num);
}

json::Value SourceInfo::to_json() const {
    json::Value v;
    v.set("kind", kind);
    v.set("uri", uri);
    v.set("live", live);
    v.set("content_origin", content_origin);
    v.set("width", geometry.width);
    v.set("height", geometry.height);
    v.set("pixel_format", pixel_format_name(geometry.format));
    v.set("nominal_fps", json::Value(json::Array{fps_num, fps_den}));
    v.set("timestamp_basis", timestamp_basis);
    json::Array t;
    for (const auto& item : transformations) t.emplace_back(item);
    v.set("transformations", json::Value(std::move(t)));
    v.set("requested", requested.is_null() ? json::Value(json::Object{}) : requested);
    v.set("effective", effective.is_null() ? json::Value(json::Object{}) : effective);
    if (!details.is_null()) v.set("details", details);
    return v;
}

const char* read_status_name(ReadStatus status) {
    switch (status) {
        case ReadStatus::Frame: return "frame";
        case ReadStatus::Corrupted: return "corrupted";
        case ReadStatus::NoSignal: return "no_signal";
        case ReadStatus::EndOfStream: return "end_of_stream";
        case ReadStatus::Disconnected: return "disconnected";
        case ReadStatus::Error: return "error";
    }
    return "error";
}

Y4mHeader parse_y4m_header(const std::string& line) {
    std::istringstream tokens(line);
    std::string magic;
    tokens >> magic;
    if (magic != "YUV4MPEG2") throw std::invalid_argument("input is not YUV4MPEG2 (missing 'YUV4MPEG2' signature)");
    Y4mHeader header;
    std::string colorspace = "420jpeg";
    bool have_w = false, have_h = false, have_f = false;
    std::string token;
    while (tokens >> token) {
        const char tag = token[0];
        const std::string value = token.substr(1);
        switch (tag) {
            case 'W':
                if (have_w) throw std::invalid_argument("Y4M header repeats W");
                header.geometry.width = parse_positive(value, "width");
                have_w = true;
                break;
            case 'H':
                if (have_h) throw std::invalid_argument("Y4M header repeats H");
                header.geometry.height = parse_positive(value, "height");
                have_h = true;
                break;
            case 'F': {
                const auto colon = value.find(':');
                if (colon == std::string::npos) throw std::invalid_argument("Y4M frame rate must be F<num>:<den>");
                header.fps_num = parse_positive(value.substr(0, colon), "frame-rate numerator");
                header.fps_den = parse_positive(value.substr(colon + 1), "frame-rate denominator");
                have_f = true;
                break;
            }
            case 'I':
                header.interlace = value;
                if (value != "p" && value != "?")
                    throw std::invalid_argument("interlaced Y4M (I" + value + ") is not supported; supply progressive video");
                break;
            case 'A': header.aspect = value; break;
            case 'C': colorspace = value; break;
            case 'X':
                header.extensions.push_back(value);
                if (value == "AHPV_SYNTHETIC=1") header.synthetic = true;
                if (value.rfind("COLORRANGE=", 0) == 0) header.color_range = value.substr(11);
                break;
            default: throw std::invalid_argument(std::string("Y4M header has unknown parameter '") + token + "'");
        }
    }
    if (!have_w || !have_h) throw std::invalid_argument("Y4M header lacks W or H");
    if (!have_f) throw std::invalid_argument("Y4M header lacks frame rate F<num>:<den>");
    if (colorspace == "420jpeg" || colorspace == "420paldv" || colorspace == "420mpeg2" || colorspace == "420")
        header.geometry.format = PixelFormat::I420;
    else if (colorspace == "444")
        header.geometry.format = PixelFormat::I444;
    else if (colorspace == "mono")
        header.geometry.format = PixelFormat::Gray8;
    else
        throw std::invalid_argument("unsupported Y4M colorspace C" + colorspace +
                                    "; supported: 420jpeg, 420paldv, 420mpeg2, 420, 444, mono (8-bit)");
    header.colorspace = colorspace;
    if (!header.geometry.valid()) throw std::invalid_argument("Y4M dimensions exceed 8192x8192");
    return header;
}

std::string format_y4m_header(const FrameGeometry& geometry, int fps_num, int fps_den,
                              const std::vector<std::string>& extensions) {
    std::string colorspace = geometry.format == PixelFormat::Gray8 ? "mono"
                             : geometry.format == PixelFormat::I444 ? "444" : "420jpeg";
    std::string line = "YUV4MPEG2 W" + std::to_string(geometry.width) + " H" + std::to_string(geometry.height) +
                       " F" + std::to_string(fps_num) + ":" + std::to_string(fps_den) + " Ip A1:1 C" + colorspace;
    for (const auto& extension : extensions) line += " X" + extension;
    return line + "\n";
}

Y4mSource::Y4mSource(std::string path, bool live, int no_signal_timeout_ms, bool realtime)
    : path_(std::move(path)), live_(live), timeout_ms_(no_signal_timeout_ms), realtime_(realtime), buffer_(1 << 20) {
    open_and_read_header(false);
}

Y4mSource::~Y4mSource() {
    if (fd_ > 0) ::close(fd_);
}

void Y4mSource::open_and_read_header(bool require_same_geometry) {
    if (fd_ > 0) ::close(fd_);
    fd_ = -1;
    buffer_begin_ = buffer_end_ = 0;
    if (path_ == "-") {
        fd_ = STDIN_FILENO;
    } else if (live_) {
        fd_ = open_stream(path_, timeout_ms_);
        if (fd_ < 0 && errno == ETIMEDOUT)
            throw std::runtime_error("no video stream producer within " + std::to_string(timeout_ms_) + " ms (camera stream absent?)");
        if (fd_ < 0) throw std::runtime_error("cannot open video stream '" + path_ + "': " + std::strerror(errno));
    } else {
        fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) throw std::runtime_error("cannot open video input '" + path_ + "': " + std::strerror(errno));
    }
    std::string line;
    bool partial = false;
    const std::int64_t deadline = mono_ns() + static_cast<std::int64_t>(timeout_ms_) * 1000000;
    for (;;) {
        const int remaining = static_cast<int>(std::max<std::int64_t>(0, (deadline - mono_ns()) / 1000000));
        const Fill result = read_line(line, 4096, live_ ? remaining : -1, partial);
        if (result == Fill::Ok) break;
        if (result == Fill::Error) throw std::runtime_error("cannot read Y4M header: " + read_error_);
        if (live_ && result == Fill::Timeout)
            throw std::runtime_error("no video stream header within " + std::to_string(timeout_ms_) + " ms (camera stream absent?)");
        if (live_) throw std::runtime_error("video stream closed before its header was complete");
        throw std::runtime_error(partial ? "Y4M header is truncated" : "video file is empty");
    }
    Y4mHeader header = parse_y4m_header(line);
    if (require_same_geometry && header.geometry != header_.geometry)
        throw std::runtime_error("reopened stream changed geometry; start a new session");
    header_ = header;
    info_ = SourceInfo{};
    info_.kind = live_ ? "y4m_stream" : "y4m_file";
    info_.uri = path_;
    info_.live = live_;
    info_.content_origin = header.synthetic ? "synthetic_fixture" : live_ ? "unknown_stream" : "recorded_file";
    info_.geometry = header.geometry;
    info_.fps_num = header.fps_num;
    info_.fps_den = header.fps_den;
    info_.timestamp_basis = live_ ? "stream_delivery_monotonic" : "file_nominal_frame_rate";
    json::Value effective;
    effective.set("width", header.geometry.width);
    effective.set("height", header.geometry.height);
    effective.set("colorspace", header.colorspace);
    effective.set("pixel_format", pixel_format_name(header.geometry.format));
    effective.set("nominal_fps", json::Value(json::Array{header.fps_num, header.fps_den}));
    effective.set("interlace", header.interlace);
    effective.set("color_range", header.color_range.empty() ? json::Value(nullptr) : json::Value(header.color_range));
    info_.effective = effective;
    json::Array extensions;
    for (const auto& e : header.extensions) extensions.emplace_back(e);
    info_.details.set("y4m_extensions", json::Value(std::move(extensions)));
    pace_origin_ns_ = mono_ns();
}

void Y4mSource::reopen() {
    failed_ = false;
    open_and_read_header(true);
    index_ = 0;
}

Y4mSource::Fill Y4mSource::fill(int timeout_ms) {
    if (buffer_begin_ > 0) {
        std::memmove(buffer_.data(), buffer_.data() + buffer_begin_, buffer_end_ - buffer_begin_);
        buffer_end_ -= buffer_begin_;
        buffer_begin_ = 0;
    }
    if (buffer_end_ == buffer_.size()) return Fill::Ok;
    if (timeout_ms >= 0) {
        // select(), not poll(): macOS poll() misses data arriving on a FIFO
        // during the wait (verified on Darwin 22); select() works everywhere.
        if (fd_ >= FD_SETSIZE) {
            read_error_ = "descriptor exceeds FD_SETSIZE";
            return Fill::Error;
        }
        int r;
        do {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(fd_, &readable);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            r = ::select(fd_ + 1, &readable, nullptr, nullptr, &tv);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            read_error_ = std::strerror(errno);
            return Fill::Error;
        }
        if (r == 0) return Fill::Timeout;
    }
    for (;;) {
        const ssize_t n = ::read(fd_, buffer_.data() + buffer_end_, buffer_.size() - buffer_end_);
        if (n < 0) {
            if (errno == EINTR) continue;
            read_error_ = std::strerror(errno);
            return Fill::Error;
        }
        if (n == 0) return Fill::Eof;
        buffer_end_ += static_cast<std::size_t>(n);
        return Fill::Ok;
    }
}

Y4mSource::Fill Y4mSource::read_line(std::string& line, std::size_t max_length, int timeout_ms, bool& partial) {
    partial = false;
    for (;;) {
        const char* begin = buffer_.data() + buffer_begin_;
        const char* end = buffer_.data() + buffer_end_;
        const char* newline = static_cast<const char*>(std::memchr(begin, '\n', static_cast<std::size_t>(end - begin)));
        if (newline) {
            line.assign(begin, newline);
            buffer_begin_ += static_cast<std::size_t>(newline - begin) + 1;
            return Fill::Ok;
        }
        partial = end != begin;
        if (static_cast<std::size_t>(end - begin) > max_length) {
            read_error_ = "header line exceeds " + std::to_string(max_length) + " bytes";
            return Fill::Error;
        }
        const Fill result = fill(timeout_ms);
        if (result != Fill::Ok) return result;
    }
}

Y4mSource::Fill Y4mSource::read_exact(std::uint8_t* out, std::size_t size, int timeout_ms, std::size_t& got) {
    got = 0;
    while (got < size) {
        const std::size_t available = buffer_end_ - buffer_begin_;
        if (available > 0) {
            const std::size_t take = std::min(available, size - got);
            std::memcpy(out + got, buffer_.data() + buffer_begin_, take);
            buffer_begin_ += take;
            got += take;
            continue;
        }
        const Fill result = fill(timeout_ms);
        if (result != Fill::Ok) return result;
    }
    return Fill::Ok;
}

ReadResult Y4mSource::read(Frame& out) {
    if (failed_) return {ReadStatus::Error, "source previously failed"};
    std::string line;
    bool partial = false;
    const int timeout = live_ ? timeout_ms_ : -1;
    const Fill header = read_line(line, 1024, timeout, partial);
    if (header == Fill::Timeout) return {ReadStatus::NoSignal, "no frame within " + std::to_string(timeout_ms_) + " ms"};
    if (header == Fill::Eof && !partial)
        return live_ ? ReadResult{ReadStatus::Disconnected, "stream producer closed the stream"}
                     : ReadResult{ReadStatus::EndOfStream, "end of file after " + std::to_string(index_) + " frames"};
    if (header != Fill::Ok) {
        failed_ = !live_;
        const std::string reason = header == Fill::Error ? read_error_ : "truncated FRAME header";
        return {live_ ? ReadStatus::Disconnected : ReadStatus::Error,
                "frame " + std::to_string(index_) + ": " + reason};
    }
    if (line != "FRAME" && line.rfind("FRAME ", 0) != 0) {
        failed_ = true;
        return {ReadStatus::Error, "malformed FRAME header at frame " + std::to_string(index_) +
                                       "; the remaining input cannot be aligned"};
    }
    const std::size_t size = header_.geometry.frame_bytes();
    out.data.resize(size);
    std::size_t got = 0;
    const Fill body = read_exact(out.data.data(), size, timeout, got);
    if (body != Fill::Ok) {
        failed_ = !live_;
        const std::string why = body == Fill::Eof ? "input ended"
                                : body == Fill::Timeout ? "no data for " + std::to_string(timeout_ms_) + " ms"
                                                        : "read error: " + read_error_;
        const std::string what = "truncated frame " + std::to_string(index_) + ": received " + std::to_string(got) +
                                 " of " + std::to_string(size) + " bytes (" + why + ")";
        return {live_ ? ReadStatus::Disconnected : ReadStatus::Error, what};
    }
    out.geometry = header_.geometry;
    out.has_source_sequence = !live_;
    out.source_sequence = index_;
    out.acquisition_mono_ns = mono_ns();
    out.acquisition_wall_ns = wall_ns();
    out.source_timestamp_ns = live_ ? out.acquisition_mono_ns : nominal_time_ns(index_, header_.fps_num, header_.fps_den);
    out.flags = 0;
    if (realtime_ && !live_) {
        const std::int64_t due = pace_origin_ns_ + out.source_timestamp_ns;
        const std::int64_t wait = due - mono_ns();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
        out.acquisition_mono_ns = mono_ns();
        out.acquisition_wall_ns = wall_ns();
    }
    ++index_;
    return {ReadStatus::Frame, ""};
}

}  // namespace ahpv
