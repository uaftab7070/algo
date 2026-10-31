// Test-only C ABI to compare the production C++ decoder with Python inference.
#include "rv1106_tracker/nanotrack_decoder.hpp"
#include <cstdio>
#include <exception>

struct ViewSpec {
    const void* data;
    std::size_t bytes;
    int channels, height, width, type, layout;
    std::size_t row, plane, pixel;
    float scale;
    int zero_point;
};

static rv1106::nano::TensorView view(const ViewSpec& s) {
    using namespace rv1106::nano;
    return {s.data, s.bytes, s.channels, s.height, s.width,
            static_cast<ElementType>(s.type), static_cast<Layout>(s.layout),
            s.row, s.plane, s.pixel, s.scale, s.zero_point};
}

extern "C" int nano_decode_for_test(const ViewSpec* cls, const ViewSpec* dist,
                                    const float* box, float expansion, float* output,
                                    char* error, std::size_t error_size) {
    try {
        const auto r = rv1106::nano::decode(view(*cls), view(*dist),
                                           {box[0], box[1], box[2], box[3]}, expansion);
        output[0] = r.box.x; output[1] = r.box.y;
        output[2] = r.box.width; output[3] = r.box.height;
        output[4] = r.score; output[5] = r.psr;
        output[6] = r.ambiguous ? 1.0F : 0.0F;
        output[7] = static_cast<float>(r.peak_x); output[8] = static_cast<float>(r.peak_y);
        return 0;
    } catch (const std::exception& e) {
        std::snprintf(error, error_size, "%s", e.what());
        return 1;
    }
}
