#include "rv1106_tracker/tracker.hpp"
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <vector>

namespace rv1106 {
namespace {
cv::Mat gray_of(const cv::Mat& frame) {
    cv::Mat gray;
    if (frame.channels() == 1) gray = frame;
    else if (frame.channels() == 4) cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
    else cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    return gray;
}
float clamp01(float x) { return std::max(0.0F, std::min(1.0F, x)); }
float median(std::vector<float>& v) {
    if (v.empty()) return 0.0F;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}
cv::Rect clipped(const cv::Rect2f& b, const cv::Size& sz) {
    const int x = std::max(0, static_cast<int>(std::floor(b.x)));
    const int y = std::max(0, static_cast<int>(std::floor(b.y)));
    const int right = std::min(sz.width, static_cast<int>(std::ceil(b.x + b.width)));
    const int bottom = std::min(sz.height, static_cast<int>(std::ceil(b.y + b.height)));
    return {x, y, std::max(0, right - x), std::max(0, bottom - y)};
}
}

const char* to_string(TrackingState s) {
    switch (s) {
        case TrackingState::Initializing: return "INITIALIZING";
        case TrackingState::Tracking: return "TRACKING";
        case TrackingState::Weak: return "WEAK";
        case TrackingState::Lost: return "LOST";
    }
    return "LOST";
}

SingleObjectTracker::SingleObjectTracker(TrackerConfig config) : config_(config) {}

void SingleObjectTracker::reset() {
    initialized_ = false; previous_gray_.release(); initial_template_.release();
    short_template_.release(); recent_template_.release(); box_ = {}; last_valid_box_ = {};
    weak_frames_ = stable_frames_ = lost_frames_ = 0;
    state_ = TrackingState::Initializing; kalman_ready_ = false;
}

bool SingleObjectTracker::initialize(const cv::Mat& frame, const cv::Rect2f& roi) {
    reset();
    if (frame.empty() || roi.width < 4 || roi.height < 4) return false;
    cv::Mat gray = gray_of(frame);
    const cv::Rect r = clipped(roi, gray.size());
    if (r.width < 4 || r.height < 4) return false;
    box_ = cv::Rect2f(static_cast<float>(r.x), static_cast<float>(r.y),
                      static_cast<float>(r.width), static_cast<float>(r.height));
    cv::Mat crop = gray(r).clone();
    const double shrink = std::min(1.0, static_cast<double>(config_.max_template_dimension) /
        std::max(crop.cols, crop.rows));
    if (shrink < 1.0) cv::resize(crop, crop, {}, shrink, shrink, cv::INTER_AREA);
    initial_template_ = crop.clone(); short_template_ = crop.clone(); recent_template_ = crop.clone();
    previous_gray_ = gray;
    last_valid_box_ = box_; state_ = TrackingState::Tracking; initialized_ = true;
    initialize_filter(box_);
    return true;
}

cv::Rect SingleObjectTracker::search_rect(const cv::Size& sz, double scale) const {
    const float w = static_cast<float>(std::max(box_.width * scale, static_cast<double>(short_template_.cols)));
    const float h = static_cast<float>(std::max(box_.height * scale, static_cast<double>(short_template_.rows)));
    const float cx = box_.x + box_.width * 0.5F, cy = box_.y + box_.height * 0.5F;
    return clipped({cx - w * 0.5F, cy - h * 0.5F, w, h}, sz);
}

SingleObjectTracker::Candidate SingleObjectTracker::match_template(const cv::Mat& gray,
        const cv::Mat& templ, const cv::Rect& search, const cv::Rect2f& predicted) const {
    Candidate c;
    if (search.width < templ.cols || search.height < templ.rows || templ.empty()) return c;
    cv::Mat response;
    cv::matchTemplate(gray(search), templ, response, cv::TM_CCOEFF_NORMED);
    double maxv = -1.0; cv::Point maxp;
    cv::minMaxLoc(response, nullptr, &maxv, nullptr, &maxp);
    c.correlation = static_cast<float>(maxv);
    // PSR: peak relative to sidelobe mean/std, excluding a small neighborhood around the peak.
    cv::Mat mask(response.size(), CV_8U, cv::Scalar(255));
    const cv::Rect exclusion(std::max(0, maxp.x - 2), std::max(0, maxp.y - 2),
        std::min(response.cols - std::max(0, maxp.x - 2), 5), std::min(response.rows - std::max(0, maxp.y - 2), 5));
    mask(exclusion).setTo(0);
    cv::Scalar mean, sd; cv::meanStdDev(response, mean, sd, mask);
    c.psr = static_cast<float>((maxv - mean[0]) / std::max(sd[0], 1e-5));
    const float x = static_cast<float>(search.x + maxp.x), y = static_cast<float>(search.y + maxp.y);
    c.box = {x, y, static_cast<float>(templ.cols), static_cast<float>(templ.rows)};
    // Convert the bounded response peak to a motion agreement term.
    const float d = cv::norm(cv::Point2f(x + c.box.width * .5F, y + c.box.height * .5F) -
                              cv::Point2f(predicted.x + predicted.width * .5F, predicted.y + predicted.height * .5F));
    const float diag = std::max(1.0F, std::hypot(predicted.width, predicted.height));
    c.motion_agreement = std::exp(-d / (diag * 1.5F));
    c.found = true;
    return c;
}

void SingleObjectTracker::initialize_filter(const cv::Rect2f& b) {
    kalman_ = cv::KalmanFilter(8, 4, 0, CV_32F);
    kalman_.transitionMatrix = (cv::Mat_<float>(8,8) <<
        1,0,1,0,0,0,0,0, 0,1,0,1,0,0,0,0, 0,0,1,0,0,0,0,0, 0,0,0,1,0,0,0,0,
        0,0,0,0,1,0,0,0, 0,0,0,0,0,1,0,0, 0,0,0,0,0,0,1,0, 0,0,0,0,0,0,0,1);
    kalman_.measurementMatrix = cv::Mat::zeros(4, 8, CV_32F);
    for (int i = 0; i < 4; ++i) kalman_.measurementMatrix.at<float>(i, i == 0 ? 0 : i == 1 ? 1 : i + 2) = 1.0F;
    kalman_.measurementMatrix.at<float>(2,4) = 1.0F; kalman_.measurementMatrix.at<float>(3,5) = 1.0F;
    cv::setIdentity(kalman_.processNoiseCov, cv::Scalar::all(1e-2));
    cv::setIdentity(kalman_.measurementNoiseCov, cv::Scalar::all(1.0));
    cv::setIdentity(kalman_.errorCovPost, cv::Scalar::all(1.0));
    const float cx = b.x + b.width*.5F, cy = b.y + b.height*.5F;
    kalman_.statePost = (cv::Mat_<float>(8,1) << cx, cy, 0, 0, b.width, b.height, 0, 0);
    kalman_ready_ = true;
}

cv::Rect2f SingleObjectTracker::filter_measurement(const cv::Rect2f& m, bool accepted, cv::Point2f& vel) {
    if (!config_.use_motion_filter || !kalman_ready_) { vel = {}; return accepted ? m : box_; }
    if (accepted) {
        cv::Mat z = (cv::Mat_<float>(4,1) << m.x + m.width*.5F, m.y + m.height*.5F, m.width, m.height);
        kalman_.correct(z);
    }
    const cv::Mat& s = accepted ? kalman_.statePost : kalman_.predict();
    vel = {s.at<float>(2), s.at<float>(3)};
    const float w = std::max(4.0F, s.at<float>(4)), h = std::max(4.0F, s.at<float>(5));
    return {s.at<float>(0)-w*.5F, s.at<float>(1)-h*.5F, w, h};
}

float SingleObjectTracker::optical_flow_check(const cv::Mat& gray, cv::Point2f& delta, float& ratio) const {
    delta = {}; ratio = 0;
    if (!config_.use_optical_flow || previous_gray_.empty()) return 0;
    cv::Rect r = clipped(box_, previous_gray_.size()); if (r.empty()) return 0;
    cv::Mat mask = cv::Mat::zeros(previous_gray_.size(), CV_8U); mask(r).setTo(255);
    std::vector<cv::Point2f> a, b; cv::goodFeaturesToTrack(previous_gray_, a, config_.flow_max_points, .01, 4, mask);
    if (a.size() < static_cast<size_t>(config_.flow_min_points)) return 0;
    std::vector<unsigned char> status; std::vector<float> errors;
    cv::calcOpticalFlowPyrLK(previous_gray_, gray, a, b, status, errors, {21,21}, 3);
    std::vector<float> dx, dy;
    for (size_t i=0; i<a.size(); ++i) if (status[i] && errors[i] <= config_.flow_max_error) {
        dx.push_back(b[i].x-a[i].x); dy.push_back(b[i].y-a[i].y);
    }
    ratio = static_cast<float>(dx.size()) / static_cast<float>(a.size());
    if (dx.size() < static_cast<size_t>(config_.flow_min_points)) return 0;
    delta = {median(dx), median(dy)};
    return clamp01(ratio / std::max(config_.minimum_flow_inlier_ratio, .01F));
}

TrackingResult SingleObjectTracker::update(const cv::Mat& frame, uint64_t id, uint64_t ts) {
    TrackingResult out; out.frame_id=id; out.timestamp_us=ts; out.state=state_;
    if (!initialized_ || frame.empty()) { out.state=TrackingState::Lost; out.lost_reason="NOT_INITIALIZED"; return out; }
    const auto begin = std::chrono::steady_clock::now();
    cv::Mat gray = gray_of(frame);
    cv::Point2f velocity{}; cv::Rect2f predicted = filter_measurement({}, false, velocity);
    cv::Point2f flow_delta{}; float flow_ratio=0;
    const float flow_score = optical_flow_check(gray, flow_delta, flow_ratio);
    if (flow_score > 0.25F && (state_ == TrackingState::Tracking || state_ == TrackingState::Weak)) {
        predicted.x = .65F*predicted.x + .35F*(box_.x + flow_delta.x);
        predicted.y = .65F*predicted.y + .35F*(box_.y + flow_delta.y);
    }
    const double scale = state_ == TrackingState::Lost ? config_.search_scale_lost :
        (state_ == TrackingState::Weak ? config_.search_scale_uncertain : config_.search_scale_normal);
    cv::Rect search = clipped({predicted.x + predicted.width*.5F - predicted.width*scale*.5F,
                               predicted.y + predicted.height*.5F - predicted.height*scale*.5F,
                               predicted.width*scale, predicted.height*scale}, gray.size());
    Candidate best;
    for (const cv::Mat* t : {&initial_template_, &short_template_, &recent_template_}) {
        for (float factor : {0.80F, 1.0F, 1.25F}) {
            cv::Mat scaled;
            if (std::abs(factor-1.0F)<0.001F) scaled=*t;
            else cv::resize(*t,scaled,{},factor,factor,factor<1.0F?cv::INTER_AREA:cv::INTER_LINEAR);
            Candidate c = match_template(gray, scaled, search, predicted);
            if (c.found) {
                const float cx=c.box.x+c.box.width*.5F, cy=c.box.y+c.box.height*.5F;
                c.box.width=box_.width*factor; c.box.height=box_.height*factor;
                c.box.x=cx-c.box.width*.5F; c.box.y=cy-c.box.height*.5F;
                if(c.correlation>best.correlation) best=c;
            }
        }
    }
    best.flow_ratio = flow_ratio;
    const cv::Point2f measured_delta(best.box.x-box_.x, best.box.y-box_.y);
    if (flow_score > 0 && cv::norm(measured_delta-flow_delta) < std::max(12.0F, box_.width*.5F))
        best.flow_agreement = clamp01(flow_score);
    const float scale_ratio = std::max(best.box.width / std::max(box_.width,1.0F), box_.width / std::max(best.box.width,1.0F));
    const bool scale_ok = best.found && scale_ratio <= static_cast<float>(config_.max_scale_change_per_frame);
    const float corr = clamp01((best.correlation - config_.minimum_correlation) /
                                std::max(.01F, 1.0F-config_.minimum_correlation));
    const float psr = clamp01(best.psr / 10.0F);
    const float confidence = .55F*corr + .20F*psr + .15F*best.flow_agreement + .10F*best.motion_agreement;
    const bool accepted = best.found && scale_ok && best.correlation >= config_.minimum_correlation &&
                          best.psr >= config_.minimum_psr && confidence >= config_.weak_confidence;
    if (accepted) {
        cv::Rect2f filtered = filter_measurement(best.box, true, velocity);
        box_ = filtered; last_valid_box_ = filtered; lost_frames_=0;
        if (confidence >= config_.high_confidence) { ++stable_frames_; weak_frames_=0; state_=TrackingState::Tracking; }
        else { ++weak_frames_; stable_frames_=0; state_=TrackingState::Weak; }
        if (config_.allow_template_update && stable_frames_ >= config_.template_update_stable_frames &&
            confidence >= config_.template_update_confidence && best.flow_agreement >= .3F) {
            cv::Rect rr = clipped(box_, gray.size());
            if (!rr.empty()) {
                cv::Mat patch = gray(rr); cv::resize(patch, patch, short_template_.size());
                cv::addWeighted(short_template_, 1.0-config_.template_update_blend, patch,
                                config_.template_update_blend, 0, short_template_);
                recent_template_ = short_template_.clone();
            }
        }
    } else {
        ++lost_frames_; ++weak_frames_; stable_frames_=0;
        if (lost_frames_ <= config_.prediction_only_frames) box_ = predicted;
        if (lost_frames_ >= config_.lost_after_frames) state_=TrackingState::Lost;
        else state_=TrackingState::Weak;
    }
    previous_gray_ = gray;
    out.box = (state_ == TrackingState::Lost) ? last_valid_box_ : box_;
    out.center={out.box.x+out.box.width*.5F,out.box.y+out.box.height*.5F};
    out.velocity=velocity; out.confidence=accepted ? confidence : confidence*.35F;
    out.similarity=std::max(0.0F,best.correlation); out.response_psr=best.psr; out.flow_inlier_ratio=flow_ratio;
    out.state=state_;
    if (!accepted) out.lost_reason = !scale_ok ? "EXCESSIVE_SCALE_CHANGE" :
        (best.correlation < config_.minimum_correlation ? "LOW_SIMILARITY" : "AMBIGUOUS_RESPONSE");
    if (state_ == TrackingState::Lost && lost_frames_ >= config_.lost_after_frames) out.lost_reason="TIMEOUT";
    out.image_error={out.center.x-frame.cols*.5F,out.center.y-frame.rows*.5F};
    out.normalized_center={out.center.x/frame.cols,out.center.y/frame.rows};
    out.normalized_error={out.image_error.x/frame.cols,out.image_error.y/frame.rows};
    out.latency_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    return out;
}
} // namespace rv1106
