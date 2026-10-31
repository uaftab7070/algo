// Standard Linux V4L2 (mmap streaming) capture adapter.
//
// Uses the upstream V4L2 UAPI only; no vendor API. Observed on the Luckfox Pico
// Pro Max image (2026-10-25): /dev/video11 = rkisp_mainpath, multiplanar API,
// single-plane NV12 with chroma at bytesperline*height, full-range quantization,
// CLOCK_MONOTONIC end-of-frame buffer timestamps. Correct exposure/colour needs
// the vendor 3A service (rkaiq_3A_server) running alongside; see
// docs/camera-compatibility.md.
#ifdef __linux__

#include "capture/source.hpp"
#include "common/clock.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace ahpv {
namespace {

int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do r = ::ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

std::string fourcc_name(std::uint32_t f) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) s[i] = static_cast<char>((f >> (8 * i)) & 0xFF);
    return s;
}

std::uint32_t parse_fourcc(const std::string& name) {
    if (name == "NV12") return V4L2_PIX_FMT_NV12;
    if (name == "GREY") return V4L2_PIX_FMT_GREY;
    throw std::invalid_argument("unsupported V4L2 pixel format '" + name + "'; supported: NV12, GREY");
}

std::runtime_error device_error(const std::string& what) {
    return std::runtime_error(what + ": " + std::strerror(errno));
}

class V4l2Source : public FrameSource {
public:
    explicit V4l2Source(const SourceOptions& options) : options_(options) { setup(); }
    ~V4l2Source() override { teardown(); }
    SourceInfo info() const override { return info_; }
    bool reopenable() const override { return true; }
    void reopen() override {
        teardown();
        setup();
    }

    ReadResult read(Frame& out) override {
        pollfd p{fd_, POLLIN, 0};
        int r;
        do r = ::poll(&p, 1, options_.no_signal_timeout_ms);
        while (r < 0 && errno == EINTR);
        if (r == 0) return {ReadStatus::NoSignal, "no frame within " + std::to_string(options_.no_signal_timeout_ms) + " ms"};
        if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return {ReadStatus::Disconnected, "capture device reported an error or hang-up"};

        v4l2_buffer buf{};
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        buf.type = type_;
        buf.memory = V4L2_MEMORY_MMAP;
        if (mplane_) {
            buf.m.planes = planes;
            buf.length = VIDEO_MAX_PLANES;
        }
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return {ReadStatus::NoSignal, "no buffer ready"};
            const bool gone = errno == ENODEV || errno == EIO || errno == ENXIO;
            return {gone ? ReadStatus::Disconnected : ReadStatus::Error, std::string("VIDIOC_DQBUF failed: ") + std::strerror(errno)};
        }
        const std::size_t used = mplane_ ? planes[0].bytesused : buf.bytesused;
        // Timestamps must be CLOCK_MONOTONIC to share the acquisition clock domain.
        if ((buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
            xioctl(fd_, VIDIOC_QBUF, &buf);
            return {ReadStatus::Error, "driver buffer timestamps are not CLOCK_MONOTONIC; refusing to mislabel them"};
        }
        const std::uint8_t* base = static_cast<const std::uint8_t*>(maps_[buf.index].first);

        out.geometry = info_.geometry;
        out.has_source_sequence = true;
        out.source_sequence = buf.sequence;
        out.acquisition_mono_ns = mono_ns();
        out.acquisition_wall_ns = wall_ns();
        out.source_timestamp_ns = static_cast<std::int64_t>(buf.timestamp.tv_sec) * 1000000000LL +
                                  static_cast<std::int64_t>(buf.timestamp.tv_usec) * 1000LL;
        out.flags = 0;
        ReadResult result{ReadStatus::Frame, ""};
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            out.corruption_reason = "driver flagged buffer error";
            result = {ReadStatus::Corrupted, out.corruption_reason};
        } else if (used < minimum_bytes_) {
            out.corruption_reason = "short buffer: " + std::to_string(used) + " of " + std::to_string(minimum_bytes_) + " bytes";
            result = {ReadStatus::Corrupted, out.corruption_reason};
        } else {
            convert(base, out);
        }
        // Always return the buffer to the driver, including on rejection.
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0)
            return {ReadStatus::Disconnected, std::string("VIDIOC_QBUF failed: ") + std::strerror(errno)};
        return result;
    }

private:
    SourceOptions options_;
    SourceInfo info_;
    int fd_{-1};
    bool mplane_{false};
    std::uint32_t type_{0};
    std::uint32_t fourcc_{0};
    std::uint32_t stride_{0};
    std::size_t minimum_bytes_{0};
    std::vector<std::pair<void*, std::size_t>> maps_;
    bool streaming_{false};

    void convert(const std::uint8_t* base, Frame& out) const {
        const int w = info_.geometry.width, h = info_.geometry.height;
        out.data.resize(info_.geometry.frame_bytes());
        std::uint8_t* y = out.data.data();
        for (int row = 0; row < h; ++row) std::memcpy(y + static_cast<std::size_t>(row) * w, base + static_cast<std::size_t>(row) * stride_, w);
        if (fourcc_ != V4L2_PIX_FMT_NV12) return;
        // NV12 chroma is assumed to start at stride*height; verify on the device.
        const std::uint8_t* uv = base + static_cast<std::size_t>(stride_) * h;
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        std::uint8_t* u = y + info_.geometry.luma_bytes();
        std::uint8_t* v = u + info_.geometry.chroma_plane_bytes();
        for (int row = 0; row < ch; ++row) {
            const std::uint8_t* line = uv + static_cast<std::size_t>(row) * stride_;
            for (int col = 0; col < cw; ++col) {
                u[static_cast<std::size_t>(row) * cw + col] = line[col * 2];
                v[static_cast<std::size_t>(row) * cw + col] = line[col * 2 + 1];
            }
        }
    }

    void setup() {
        fd_ = ::open(options_.path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) throw device_error("cannot open capture device " + options_.path);
        try {
            configure();
        } catch (...) {
            teardown();
            throw;
        }
    }

    void configure() {
        v4l2_capability cap{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) throw device_error("VIDIOC_QUERYCAP failed");
        const std::uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        mplane_ = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE;
        if (!mplane_ && !(caps & V4L2_CAP_VIDEO_CAPTURE)) throw std::runtime_error(options_.path + " is not a video capture node");
        if (!(caps & V4L2_CAP_STREAMING)) throw std::runtime_error(options_.path + " does not support streaming I/O");
        type_ = mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;

        const std::uint32_t requested = parse_fourcc(options_.pixel_format);
        v4l2_format fmt{};
        fmt.type = type_;
        if (mplane_) {
            fmt.fmt.pix_mp.width = static_cast<std::uint32_t>(options_.width);
            fmt.fmt.pix_mp.height = static_cast<std::uint32_t>(options_.height);
            fmt.fmt.pix_mp.pixelformat = requested;
            fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        } else {
            fmt.fmt.pix.width = static_cast<std::uint32_t>(options_.width);
            fmt.fmt.pix.height = static_cast<std::uint32_t>(options_.height);
            fmt.fmt.pix.pixelformat = requested;
            fmt.fmt.pix.field = V4L2_FIELD_NONE;
        }
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) throw device_error("VIDIOC_S_FMT failed");
        // Report the driver's reply, which may differ from the request (AC-006).
        std::uint32_t width, height, sizeimage, planes = 1, quantization = V4L2_QUANTIZATION_DEFAULT;
        if (mplane_) {
            width = fmt.fmt.pix_mp.width;
            height = fmt.fmt.pix_mp.height;
            fourcc_ = fmt.fmt.pix_mp.pixelformat;
            planes = fmt.fmt.pix_mp.num_planes;
            stride_ = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
            sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
            quantization = fmt.fmt.pix_mp.quantization;
        } else {
            width = fmt.fmt.pix.width;
            height = fmt.fmt.pix.height;
            fourcc_ = fmt.fmt.pix.pixelformat;
            stride_ = fmt.fmt.pix.bytesperline;
            sizeimage = fmt.fmt.pix.sizeimage;
            quantization = fmt.fmt.pix.quantization;
        }
        if (fourcc_ != requested)
            throw std::runtime_error("driver selected pixel format " + fourcc_name(fourcc_) + " instead of " + options_.pixel_format);
        if (planes != 1) throw std::runtime_error("multi-plane buffer layouts (e.g. NV12M) are not supported");
        if (stride_ < width) stride_ = width;
        minimum_bytes_ = static_cast<std::size_t>(stride_) * height * (fourcc_ == V4L2_PIX_FMT_NV12 ? 3 : 2) / 2;
        if (fourcc_ == V4L2_PIX_FMT_GREY) minimum_bytes_ = static_cast<std::size_t>(stride_) * height;

        info_ = SourceInfo{};
        info_.kind = "v4l2";
        info_.uri = options_.path;
        info_.live = true;
        info_.content_origin = "camera";
        info_.geometry = {static_cast<int>(width), static_cast<int>(height),
                          fourcc_ == V4L2_PIX_FMT_NV12 ? PixelFormat::I420 : PixelFormat::Gray8};
        if (fourcc_ == V4L2_PIX_FMT_NV12) info_.transformations.push_back("nv12_to_i420_chroma_deinterleave");
        if (stride_ != width) info_.transformations.push_back("row_stride_removed");

        if (options_.fps > 0) {
            v4l2_streamparm parm{};
            parm.type = type_;
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(options_.fps);
            xioctl(fd_, VIDIOC_S_PARM, &parm);  // Optional; the effective value is read back below.
        }
        v4l2_streamparm parm{};
        parm.type = type_;
        if (xioctl(fd_, VIDIOC_G_PARM, &parm) == 0 && (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) &&
            parm.parm.capture.timeperframe.numerator > 0) {
            info_.fps_num = static_cast<int>(parm.parm.capture.timeperframe.denominator);
            info_.fps_den = static_cast<int>(parm.parm.capture.timeperframe.numerator);
        }

        v4l2_requestbuffers req{};
        req.count = static_cast<std::uint32_t>(options_.buffers);
        req.type = type_;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) throw device_error("VIDIOC_REQBUFS failed");
        if (req.count < 2) throw std::runtime_error("driver granted fewer than two capture buffers");
        for (std::uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            v4l2_plane plane[VIDEO_MAX_PLANES]{};
            buf.type = type_;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (mplane_) {
                buf.m.planes = plane;
                buf.length = VIDEO_MAX_PLANES;
            }
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) throw device_error("VIDIOC_QUERYBUF failed");
            const std::size_t length = mplane_ ? plane[0].length : buf.length;
            const off_t offset = mplane_ ? static_cast<off_t>(plane[0].m.mem_offset) : static_cast<off_t>(buf.m.offset);
            void* map = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
            if (map == MAP_FAILED) throw device_error("mmap of capture buffer failed");
            maps_.emplace_back(map, length);
            if (length < minimum_bytes_) throw std::runtime_error("capture buffer smaller than one frame");
            if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) throw device_error("VIDIOC_QBUF failed");
        }
        v4l2_buf_type stream_type = static_cast<v4l2_buf_type>(type_);
        if (xioctl(fd_, VIDIOC_STREAMON, &stream_type) < 0) throw device_error("VIDIOC_STREAMON failed");
        streaming_ = true;

        // The timestamp basis is whatever the driver declares, recorded verbatim.
        // Clock checked on every buffer in read(); the exposure point comes from the driver flags.
        info_.timestamp_basis = "v4l2_buffer_monotonic (driver timestamp source flag recorded per manifest)";
        json::Value requested_json, effective_json;
        requested_json.set("width", options_.width);
        requested_json.set("height", options_.height);
        requested_json.set("pixel_format", options_.pixel_format);
        requested_json.set("fps", options_.fps);
        requested_json.set("buffers", options_.buffers);
        effective_json.set("width", static_cast<std::int64_t>(width));
        effective_json.set("height", static_cast<std::int64_t>(height));
        effective_json.set("pixel_format", fourcc_name(fourcc_));
        effective_json.set("bytes_per_line", static_cast<std::int64_t>(stride_));
        effective_json.set("size_image", static_cast<std::int64_t>(sizeimage));
        effective_json.set("buffers", static_cast<std::int64_t>(req.count));
        effective_json.set("nominal_fps", info_.fps_num > 0 ? json::Value(json::Array{info_.fps_num, info_.fps_den}) : json::Value(nullptr));
        effective_json.set("multiplanar_api", mplane_);
        effective_json.set("color_range", quantization == V4L2_QUANTIZATION_FULL_RANGE ? "FULL"
                                          : quantization == V4L2_QUANTIZATION_LIM_RANGE ? "LIMITED" : "DEFAULT");
        effective_json.set("driver", reinterpret_cast<const char*>(cap.driver));
        effective_json.set("card", reinterpret_cast<const char*>(cap.card));
        effective_json.set("bus_info", reinterpret_cast<const char*>(cap.bus_info));
        info_.requested = requested_json;
        info_.effective = effective_json;
    }

    void teardown() {
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type stream_type = static_cast<v4l2_buf_type>(type_);
            xioctl(fd_, VIDIOC_STREAMOFF, &stream_type);
        }
        streaming_ = false;
        for (auto& [map, length] : maps_) ::munmap(map, length);
        maps_.clear();
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
};

}  // namespace

std::unique_ptr<FrameSource> make_v4l2_source(const SourceOptions& options) {
    return std::make_unique<V4l2Source>(options);
}

}  // namespace ahpv

#endif  // __linux__
