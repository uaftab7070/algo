#pragma once
// YUV4MPEG2 reader for deterministic file replay and for live pipes/FIFOs fed
// by a vendor-supported capture tool. Only 8-bit progressive mono/4:2:0/4:4:4.
#include "capture/source.hpp"

#include <string>
#include <vector>

namespace ahpv {

struct Y4mHeader {
    FrameGeometry geometry;
    int fps_num{0};
    int fps_den{1};
    std::string colorspace;
    std::string interlace{"p"};
    std::string aspect;
    std::string color_range;
    bool synthetic{false};
    std::vector<std::string> extensions;
};

// Throws std::invalid_argument with an actionable message.
Y4mHeader parse_y4m_header(const std::string& line);
std::string format_y4m_header(const FrameGeometry& geometry, int fps_num, int fps_den,
                              const std::vector<std::string>& extensions);

class Y4mSource : public FrameSource {
public:
    // path "-" reads standard input. live=true uses delivery timestamps.
    Y4mSource(std::string path, bool live, int no_signal_timeout_ms, bool realtime);
    ~Y4mSource() override;
    SourceInfo info() const override { return info_; }
    ReadResult read(Frame& out) override;
    bool reopenable() const override { return live_ && path_ != "-"; }
    void reopen() override;

private:
    enum class Fill { Ok, Eof, Timeout, Error };
    std::string path_;
    bool live_;
    int timeout_ms_;
    bool realtime_;
    int fd_{-1};
    std::vector<char> buffer_;
    std::size_t buffer_begin_{0};
    std::size_t buffer_end_{0};
    SourceInfo info_;
    Y4mHeader header_;
    std::uint64_t index_{0};
    std::int64_t pace_origin_ns_{0};
    bool failed_{false};
    std::string read_error_;

    void open_and_read_header(bool require_same_geometry);
    Fill fill(int timeout_ms);
    // Returns Ok with a line, Eof when no bytes at all, Timeout or Error.
    Fill read_line(std::string& line, std::size_t max_length, int timeout_ms, bool& partial);
    Fill read_exact(std::uint8_t* out, std::size_t size, int timeout_ms, std::size_t& got);
};

}  // namespace ahpv
