#include "capture/source.hpp"

#include "capture/y4m_source.hpp"

#include <stdexcept>

namespace ahpv {

#ifdef __linux__
std::unique_ptr<FrameSource> make_v4l2_source(const SourceOptions& options);
#endif

std::unique_ptr<FrameSource> open_source(const SourceOptions& options) {
    if (options.kind == "y4m_file")
        return std::make_unique<Y4mSource>(options.path, false, options.no_signal_timeout_ms, options.realtime);
    if (options.kind == "y4m_stream")
        return std::make_unique<Y4mSource>(options.path, true, options.no_signal_timeout_ms, false);
    if (options.kind == "v4l2") {
#ifdef __linux__
        return make_v4l2_source(options);
#else
        throw std::runtime_error("source.kind v4l2 requires a Linux build; use y4m_file or y4m_stream on this host");
#endif
    }
    throw std::invalid_argument("unknown source kind '" + options.kind + "'");
}

}  // namespace ahpv
