#include "rv1106_tracker/config.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>

namespace rv1106 {
namespace {
std::string trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r\n"), last=s.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : s.substr(first,last-first+1);
}
bool parse_bool(const std::string& s) { return s=="true" || s=="1" || s=="yes" || s=="on"; }
}

TrackerConfig load_config(const std::string& path, TrackerConfig c) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open config: " + path);
    std::string line;
    while (std::getline(in,line)) {
        line=trim(line); if (line.empty() || line[0]=='#' || line[0]==';' || line[0]=='[') continue;
        const auto split=line.find('='); if (split==std::string::npos) continue;
        const std::string k=trim(line.substr(0,split)), v=trim(line.substr(split+1));
        try {
            if(k=="high_confidence") c.high_confidence=std::stof(v);
            else if(k=="weak_confidence") c.weak_confidence=std::stof(v);
            else if(k=="minimum_correlation") c.minimum_correlation=std::stof(v);
            else if(k=="minimum_psr") c.minimum_psr=std::stof(v);
            else if(k=="search_scale_normal") c.search_scale_normal=std::stod(v);
            else if(k=="search_scale_uncertain") c.search_scale_uncertain=std::stod(v);
            else if(k=="search_scale_lost") c.search_scale_lost=std::stod(v);
            else if(k=="max_scale_change_per_frame") c.max_scale_change_per_frame=std::stod(v);
            else if(k=="lost_after_frames") c.lost_after_frames=std::stoi(v);
            else if(k=="prediction_only_frames") c.prediction_only_frames=std::stoi(v);
            else if(k=="flow_min_points") c.flow_min_points=std::stoi(v);
            else if(k=="flow_max_points") c.flow_max_points=std::stoi(v);
            else if(k=="minimum_flow_inlier_ratio") c.minimum_flow_inlier_ratio=std::stof(v);
            else if(k=="use_optical_flow") c.use_optical_flow=parse_bool(v);
            else if(k=="use_motion_filter") c.use_motion_filter=parse_bool(v);
            else if(k=="allow_template_update") c.allow_template_update=parse_bool(v);
            else if(k=="template_update_confidence") c.template_update_confidence=std::stof(v);
            else if(k=="template_update_stable_frames") c.template_update_stable_frames=std::stoi(v);
            else if(k=="template_update_blend") c.template_update_blend=std::stof(v);
            else if(k=="frame_skip") c.frame_skip=std::stoi(v);
        } catch (const std::exception&) { throw std::runtime_error("Invalid value for config key '"+k+"'"); }
    }
    return c;
}
} // namespace rv1106
