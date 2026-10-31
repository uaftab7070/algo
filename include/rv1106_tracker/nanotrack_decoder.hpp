#pragma once

#include <cstddef>
#include <cstdint>

namespace rv1106::nano {

// Logical tensors always have batch=1, H=W=16. Strides are in elements,
// not bytes. The adapter must use queried runtime metadata, not infer layout.
enum class ElementType { Float32, Int8, UInt8 };
enum class Layout { NCHW, NHWC };

struct TensorView {
    const void* data{nullptr};
    std::size_t byte_size{0};
    int channels{0};
    int height{16};
    int width{16};
    ElementType type{ElementType::Float32};
    Layout layout{Layout::NCHW};
    // Zero selects packed stride. NCHW uses plane_stride, NHWC pixel_stride.
    std::size_t row_stride{0};
    std::size_t plane_stride{0};
    std::size_t pixel_stride{0};
    float scale{1.0F};
    int zero_point{0};
};

struct Box { float x, y, width, height; };

struct Candidate {
    Box box{};
    float score{0};          // Raw class evidence, excluding position preference.
    float psr{0};
    bool ambiguous{false};
    int peak_x{0};
    int peak_y{0};
};

// NanoTrack v2: logits are [background, foreground], distances are [l,t,r,b].
// This decodes a proposal; it does NOT accept a track or mutate its template.
// Invalid tensor metadata/data or geometry throws std::invalid_argument.
Candidate decode(const TensorView& logits, const TensorView& distances,
                 const Box& anchor, float expansion = 1.0F);

} // namespace rv1106::nano
