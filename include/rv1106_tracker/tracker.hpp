#pragma once

#include <opencv2/core.hpp>
#include <opencv2/video/tracking.hpp>
#include <cstdint>
#include <string>

namespace rv1106 {

enum class TrackingState { Initializing, Tracking, Weak, Lost };
const char* to_string(TrackingState state);

struct TrackerConfig {
    // All thresholds are tunable; defaults are starting values, not validated RV1106 results.
    float high_confidence{0.70F};
    float weak_confidence{0.43F};
    float minimum_correlation{0.38F};
    float minimum_psr{3.0F};
    float minimum_flow_inlier_ratio{0.25F};
    int flow_min_points{5};
    int flow_max_points{100};
    double flow_max_error{18.0};
    double search_scale_normal{2.5};
    double search_scale_uncertain{4.0};
    double search_scale_lost{5.0};
    double max_scale_change_per_frame{1.35};
    int lost_after_frames{5};
    int prediction_only_frames{3};
    bool use_optical_flow{true};
    bool use_motion_filter{true};
    bool allow_template_update{true};
    float template_update_confidence{0.84F};
    int template_update_stable_frames{10};
    float template_update_blend{0.04F};
    int max_template_dimension{256};
    int fps_limit{30};
    int frame_skip{0};
};

struct TrackingResult {
    uint64_t frame_id{0};
    uint64_t timestamp_us{0};
    cv::Rect2f box{};
    cv::Point2f center{};
    cv::Point2f velocity{};
    cv::Point2f image_error{};
    cv::Point2f normalized_center{};
    cv::Point2f normalized_error{};
    float confidence{0.0F};
    float similarity{0.0F};
    float response_psr{0.0F};
    float flow_inlier_ratio{0.0F};
    double latency_ms{0.0};
    TrackingState state{TrackingState::Initializing};
    std::string lost_reason;
};

class SingleObjectTracker {
public:
    explicit SingleObjectTracker(TrackerConfig config = {});
    bool initialize(const cv::Mat& frame, const cv::Rect2f& roi);
    TrackingResult update(const cv::Mat& frame, uint64_t frame_id, uint64_t timestamp_us);
    bool initialized() const { return initialized_; }
    cv::Rect2f last_valid_box() const { return last_valid_box_; }
    void reset();

private:
    struct Candidate {
        cv::Rect2f box{};
        float correlation{-1.0F};
        float psr{0.0F};
        float flow_ratio{0.0F};
        float flow_agreement{0.0F};
        float motion_agreement{0.0F};
        bool found{false};
    };

    TrackerConfig config_;
    bool initialized_{false};
    cv::Mat previous_gray_;
    cv::Mat initial_template_;
    cv::Mat short_template_;
    cv::Mat recent_template_;
    cv::Rect2f box_{};
    cv::Rect2f last_valid_box_{};
    cv::KalmanFilter kalman_;
    bool kalman_ready_{false};
    int weak_frames_{0};
    int stable_frames_{0};
    int lost_frames_{0};
    TrackingState state_{TrackingState::Initializing};

    cv::Rect search_rect(const cv::Size& size, double scale) const;
    Candidate match_template(const cv::Mat& gray, const cv::Mat& templ,
                             const cv::Rect& search, const cv::Rect2f& predicted) const;
    void initialize_filter(const cv::Rect2f& box);
    cv::Rect2f filter_measurement(const cv::Rect2f& measurement, bool accepted,
                                  cv::Point2f& velocity);
    float optical_flow_check(const cv::Mat& gray, cv::Point2f& delta,
                             float& inlier_ratio) const;
};

} // namespace rv1106
