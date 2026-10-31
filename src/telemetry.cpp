#include "rv1106_tracker/telemetry.hpp"
#include <iomanip>
#include <sstream>

namespace rv1106 {
std::string to_json(const TrackingResult& r, int width, int height) {
    std::ostringstream s; s << std::fixed << std::setprecision(4)
      << "{\"frame_id\":" << r.frame_id << ",\"timestamp_us\":" << r.timestamp_us
      << ",\"x\":" << r.box.x << ",\"y\":" << r.box.y << ",\"width\":" << r.box.width
      << ",\"height\":" << r.box.height << ",\"center_x\":" << r.center.x
      << ",\"center_y\":" << r.center.y << ",\"normalized_center_x\":" << r.normalized_center.x
      << ",\"normalized_center_y\":" << r.normalized_center.y << ",\"image_error_x\":" << r.image_error.x
      << ",\"image_error_y\":" << r.image_error.y << ",\"normalized_error_x\":" << r.normalized_error.x
      << ",\"normalized_error_y\":" << r.normalized_error.y << ",\"velocity_x\":" << r.velocity.x
      << ",\"velocity_y\":" << r.velocity.y << ",\"confidence\":" << r.confidence
      << ",\"similarity\":" << r.similarity << ",\"flow_inlier_ratio\":" << r.flow_inlier_ratio
      << ",\"state\":\"" << to_string(r.state) << "\",\"lost_reason\":\"" << r.lost_reason
      << "\",\"latency_ms\":" << r.latency_ms << ",\"image_width\":" << width
      << ",\"image_height\":" << height << "}";
    return s.str();
}
} // namespace rv1106
