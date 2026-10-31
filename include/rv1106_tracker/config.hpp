#pragma once
#include "rv1106_tracker/tracker.hpp"
#include <string>

namespace rv1106 {
TrackerConfig load_config(const std::string& path, TrackerConfig defaults = {});
}
