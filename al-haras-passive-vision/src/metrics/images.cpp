#include "metrics/images.hpp"

#include <algorithm>
#include <stdexcept>

namespace ahpv {
namespace {
inline std::uint8_t clamp8(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

void put_le(std::string& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}
}  // namespace

RgbImage frame_to_rgb(const FrameGeometry& g, const std::uint8_t* data, int max_width, bool full_range) {
    if (!g.valid() || max_width < 1) throw std::invalid_argument("invalid geometry for preview conversion");
    const int factor = std::max(1, (g.width + max_width - 1) / max_width);
    RgbImage image;
    image.width = std::max(1, g.width / factor);
    image.height = std::max(1, g.height / factor);
    image.rgb.resize(static_cast<std::size_t>(image.width) * image.height * 3);
    const std::uint8_t* y_plane = data;
    const std::uint8_t* u_plane = data + g.luma_bytes();
    const std::uint8_t* v_plane = u_plane + g.chroma_plane_bytes();
    const int chroma_width = g.format == PixelFormat::I420 ? (g.width + 1) / 2 : g.width;
    for (int oy = 0; oy < image.height; ++oy) {
        const int sy = oy * factor;
        for (int ox = 0; ox < image.width; ++ox) {
            const int sx = ox * factor;
            const int y = y_plane[static_cast<std::size_t>(sy) * g.width + sx];
            std::uint8_t* px = &image.rgb[(static_cast<std::size_t>(oy) * image.width + ox) * 3];
            if (g.format == PixelFormat::Gray8) {
                px[0] = px[1] = px[2] = static_cast<std::uint8_t>(y);
                continue;
            }
            const int cx = g.format == PixelFormat::I420 ? sx / 2 : sx;
            const int cy = g.format == PixelFormat::I420 ? sy / 2 : sy;
            const int u = u_plane[static_cast<std::size_t>(cy) * chroma_width + cx] - 128;
            const int v = v_plane[static_cast<std::size_t>(cy) * chroma_width + cx] - 128;
            if (full_range) {
                px[0] = clamp8(y + ((359 * v) >> 8));
                px[1] = clamp8(y - ((88 * u + 183 * v) >> 8));
                px[2] = clamp8(y + ((454 * u) >> 8));
            } else {
                const int c = 298 * (y - 16);
                px[0] = clamp8((c + 409 * v + 128) >> 8);
                px[1] = clamp8((c - 100 * u - 208 * v + 128) >> 8);
                px[2] = clamp8((c + 516 * u + 128) >> 8);
            }
        }
    }
    image.transform = "source " + std::to_string(g.width) + "x" + std::to_string(g.height) + " " + pixel_format_name(g.format) +
                      " -> display " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                      " rgb24; nearest-neighbour decimation 1/" + std::to_string(factor) +
                      (g.format == PixelFormat::Gray8 ? "; grey replicated" :
                       full_range ? "; BT.601 full-range YCbCr->RGB" : "; BT.601 limited-range YCbCr->RGB (assumed)");
    return image;
}

std::string encode_bmp(const RgbImage& image) {
    const std::uint32_t row = static_cast<std::uint32_t>((image.width * 3 + 3) & ~3);
    const std::uint32_t pixels = row * static_cast<std::uint32_t>(image.height);
    std::string out;
    out.reserve(54 + pixels);
    out += "BM";
    put_le(out, 54 + pixels, 4);
    put_le(out, 0, 4);
    put_le(out, 54, 4);
    put_le(out, 40, 4);
    put_le(out, static_cast<std::uint32_t>(image.width), 4);
    put_le(out, static_cast<std::uint32_t>(image.height), 4);  // Positive: bottom-up rows.
    put_le(out, 1, 2);
    put_le(out, 24, 2);
    put_le(out, 0, 4);
    put_le(out, pixels, 4);
    put_le(out, 2835, 4);
    put_le(out, 2835, 4);
    put_le(out, 0, 4);
    put_le(out, 0, 4);
    for (int y = image.height - 1; y >= 0; --y) {
        const std::uint8_t* src = &image.rgb[static_cast<std::size_t>(y) * image.width * 3];
        for (int x = 0; x < image.width; ++x) {
            out.push_back(static_cast<char>(src[x * 3 + 2]));
            out.push_back(static_cast<char>(src[x * 3 + 1]));
            out.push_back(static_cast<char>(src[x * 3]));
        }
        for (std::uint32_t pad = static_cast<std::uint32_t>(image.width) * 3; pad < row; ++pad) out.push_back('\0');
    }
    return out;
}

RgbImage contact_sheet(const std::vector<RgbImage>& tiles, int columns) {
    if (tiles.empty() || columns < 1) throw std::invalid_argument("contact sheet needs tiles");
    const int tw = tiles.front().width, th = tiles.front().height, gap = 4;
    const int rows = static_cast<int>((tiles.size() + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
    RgbImage sheet;
    sheet.width = columns * tw + (columns + 1) * gap;
    sheet.height = rows * th + (rows + 1) * gap;
    sheet.rgb.assign(static_cast<std::size_t>(sheet.width) * sheet.height * 3, 40);
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const auto& tile = tiles[i];
        if (tile.width != tw || tile.height != th) throw std::invalid_argument("contact sheet tiles differ in size");
        const int ox = gap + static_cast<int>(i % static_cast<std::size_t>(columns)) * (tw + gap);
        const int oy = gap + static_cast<int>(i / static_cast<std::size_t>(columns)) * (th + gap);
        for (int y = 0; y < th; ++y)
            std::copy_n(&tile.rgb[static_cast<std::size_t>(y) * tw * 3], static_cast<std::size_t>(tw) * 3,
                        &sheet.rgb[(static_cast<std::size_t>(oy + y) * sheet.width + ox) * 3]);
    }
    sheet.transform = "contact sheet of " + std::to_string(tiles.size()) + " tiles; each: " + tiles.front().transform;
    return sheet;
}

}  // namespace ahpv
