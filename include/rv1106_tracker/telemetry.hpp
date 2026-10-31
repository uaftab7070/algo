#pragma once
#include "rv1106_tracker/tracker.hpp"
#include <string>

namespace rv1106 {
std::string to_json(const TrackingResult& result, int image_width, int image_height);
}
