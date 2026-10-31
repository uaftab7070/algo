#pragma once
// Display-only image conversion. Every conversion is described in `transform`
// so a viewer never mistakes a scaled/colour-converted preview for source data.
#include "frames/frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ahpv {

struct RgbImage {
    int width{0};
    int height{0};
    std::vector<std::uint8_t> rgb;  // Row-major, top-down, 3 bytes per pixel.
    std::string transform;
};

// Nearest-neighbour decimation by an integer factor so width <= max_width,
// BT.601 YCbCr->RGB (limited range unless full_range).
RgbImage frame_to_rgb(const FrameGeometry& geometry, const std::uint8_t* data, int max_width, bool full_range);
std::string encode_bmp(const RgbImage& image);  // 24-bit uncompressed.
RgbImage contact_sheet(const std::vector<RgbImage>& tiles, int columns);

}  // namespace ahpv
