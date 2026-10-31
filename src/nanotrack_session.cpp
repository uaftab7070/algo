#include "rv1106_tracker/nanotrack_session.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rv1106::nano {
namespace {
bool unit(float f) { return std::isfinite(f) && f >= 0 && f <= 1; }
bool box_valid(const Box& b) {
    return std::isfinite(b.x) && std::isfinite(b.y) &&
           std::isfinite(b.width) && std::isfinite(b.height) && b.width > 0 && b.height > 0;
}
bool inside(const Box& b, const FrameStamp& frame) {
    return box_valid(b) && b.x >= 0 && b.y >= 0 &&
           static_cast<double>(b.x) + b.width <= frame.width &&
           static_cast<double>(b.y) + b.height <= frame.height;
}
double distance(const Box& a, const Box& b) {
    return std::hypot((static_cast<double>(a.x) + a.width * 0.5) - (b.x + b.width * 0.5),
                      (static_cast<double>(a.y) + a.height * 0.5) - (b.y + b.height * 0.5));
}
double scale_jump(const Box& a, const Box& b) {
    return std::max({static_cast<double>(a.width) / b.width, static_cast<double>(b.width) / a.width,
                     static_cast<double>(a.height) / b.height, static_cast<double>(b.height) / a.height});
}
} // namespace

const char* name(State state) {
    switch (state) {
        case State::Uninitialized: return "SELECT TARGET";
        case State::Initializing: return "INITIALIZING";
        case State::Tracking: return "TRACKING";
        case State::Weak: return "WEAK";
        case State::Lost: return "LOST";
    }
    return "LOST";
}
const char* name(Reason reason) {
    switch (reason) {
        case Reason::None: return "";
        case Reason::SelectTarget: return "SELECT TARGET";
        case Reason::Confirming: return "CONFIRMING TARGET";
        case Reason::LowEvidence: return "LOW VISUAL EVIDENCE";
        case Reason::Ambiguous: return "AMBIGUOUS TARGET";
        case Reason::InvalidEvidence: return "INVALID VISUAL EVIDENCE";
        case Reason::SourceChanged: return "SOURCE CHANGED — RESELECT";
        case Reason::Expired: return "OBSERVATION EXPIRED — RESELECT";
        case Reason::CameraFailure: return "CAMERA FAILURE — RESELECT";
        case Reason::ModelFailure: return "MODEL FAILURE — RESELECT";
        case Reason::ClockError: return "CLOCK ERROR — RESELECT";
    }
    return "INVALID VISUAL EVIDENCE";
}

Session::Session(SessionConfig config) : config_(config) {
    if (!config.max_observation_age_us || !config.lost_after_us ||
        !config.confirmation_frames || config.confirmation_frames > 100 ||
        !unit(config.min_score) || !unit(config.min_appearance) ||
        !unit(config.min_confidence) || !unit(config.high_confidence) ||
        config.high_confidence < config.min_confidence ||
        !std::isfinite(config.min_psr) || config.min_psr < 0 ||
        !std::isfinite(config.max_scale_jump) || config.max_scale_jump < 1)
        throw std::invalid_argument("Invalid NanoTrack session configuration");
}

void Session::fail(Reason reason) {
    if (!result_.manual_required) {
        result_.state = State::Lost;
        result_.reason = reason;
        result_.manual_required = true;
    }
    result_.accepted = false;
    result_.confidence = 0;
    result_.confirmations = 0;
    has_pending_ = false;
}

void Session::age(std::uint64_t now_us) {
    if (has_clock_ && now_us < last_now_us_) {
        fail(Reason::ClockError);
        return;
    }
    last_now_us_ = now_us;
    has_clock_ = true;
    if (result_.state == State::Uninitialized) return;
    if (now_us < result_.frame.capture_us) {
        fail(Reason::ClockError);
        return;
    }
    result_.age_us = now_us - result_.frame.capture_us;
    if (result_.age_us >= config_.max_observation_age_us) fail(Reason::Expired);
}

Observation Session::initialize(const FrameStamp& frame, const Box& selected, std::uint64_t now_us) {
    std::lock_guard<std::mutex> guard(mutex_);
    // A failed new selection invalidates any earlier target.
    result_ = {};
    has_pending_ = false;
    initialized_capture_us_ = frame.capture_us;
    if (selection_counter_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(Reason::InvalidEvidence);
        return result_;
    }
    result_.selection_id = ++selection_counter_;
    if (has_clock_ && now_us < last_now_us_) {
        fail(Reason::ClockError);
        return result_;
    }
    last_now_us_ = now_us;
    has_clock_ = true;
    if (frame.width <= 0 || frame.height <= 0 || !inside(selected, frame) ||
        selected.width < 12 || selected.height < 12) {
        fail(Reason::InvalidEvidence);
        return result_;
    }
    result_.frame = frame;
    if (frame.capture_us > now_us) {
        fail(Reason::ClockError);
        return result_;
    }
    selected_ = selected;
    result_.state = State::Initializing;
    result_.reason = Reason::Confirming;
    age(now_us);
    return result_;
}

Observation Session::submit(const FrameStamp& frame, const Candidate& candidate,
                            const VisualEvidence& e, std::uint64_t now_us,
                            std::uint64_t selection_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    age(now_us);
    if (result_.manual_required || result_.state == State::Uninitialized) return result_;
    if (selection_id != result_.selection_id) return result_;
    if (frame.source_epoch != result_.frame.source_epoch) {
        fail(Reason::SourceChanged);
        return result_;
    }
    // Duplicate/out-of-order source frames cannot confirm or refresh a result.
    if (frame.frame_id <= result_.frame.frame_id || frame.capture_us <= result_.frame.capture_us)
        return result_;
    if (frame.width != result_.frame.width || frame.height != result_.frame.height) {
        fail(Reason::SourceChanged);
        return result_;
    }
    if (frame.capture_us > now_us) {
        fail(Reason::ClockError);
        return result_;
    }
    if (now_us - frame.capture_us >= config_.max_observation_age_us) {
        fail(Reason::Expired);
        return result_;
    }
    result_.frame = frame;
    result_.age_us = now_us - frame.capture_us;
    if (!box_valid(candidate.box) || !unit(candidate.score) || !std::isfinite(candidate.psr) ||
        !unit(e.appearance) || !unit(e.original_appearance) || !unit(e.motion_agreement) ||
        !unit(e.flow_agreement) || !std::isfinite(e.structure) || e.structure < -1 || e.structure > 1 ||
        (e.flow_support && !e.has_flow)) {
        fail(Reason::InvalidEvidence);
        return result_;
    }
    const Box anchor = result_.has_box ? result_.last_box : selected_;
    const double score = std::clamp((candidate.score - 0.20) / 0.75, 0.0, 1.0);
    const double sharpness = std::clamp(candidate.psr / 6.0, 0.0, 1.0);
    const float confidence = static_cast<float>(e.has_flow ?
        0.48 * score + 0.18 * sharpness + 0.18 * e.appearance + 0.16 * e.flow_agreement :
        0.58 * score + 0.20 * sharpness + 0.17 * e.appearance + 0.05 * e.motion_agreement);
    // Fully visible geometry is required here. Partial-target behavior needs
    // separate validation before it can be used by the live native pipeline.
    bool valid = inside(candidate.box, frame) && candidate.score >= config_.min_score &&
        e.appearance >= config_.min_appearance && confidence >= config_.min_confidence &&
        (candidate.psr >= config_.min_psr || e.flow_support) &&
        scale_jump(candidate.box, anchor) <= config_.max_scale_jump && !candidate.ambiguous;
    const bool needs_confirmation = !result_.accepted;
    result_.reason = candidate.ambiguous ? Reason::Ambiguous : Reason::LowEvidence;
    if (needs_confirmation) {
        const bool strong = valid && e.original_appearance >= 0.5F &&
            (e.structure >= 0.78F || (candidate.score > 0.78F && candidate.psr > 2.5F));
        const double diagonal = std::max(1.0, std::hypot(static_cast<double>(anchor.width), anchor.height));
        const bool consistent = !has_pending_ || distance(candidate.box, pending_) < diagonal * 0.7;
        result_.confirmations = strong ? (consistent ? result_.confirmations + 1 : 1) : 0;
        has_pending_ = strong;
        if (strong) pending_ = candidate.box;
        valid = strong && result_.confirmations >= config_.confirmation_frames;
        if (strong && !valid) result_.reason = Reason::Confirming;
    }
    result_.accepted = valid;
    result_.confidence = valid ? confidence : std::min(0.49F, confidence * 0.5F);
    if (valid) {
        result_.state = confidence >= config_.high_confidence ? State::Tracking : State::Weak;
        result_.reason = Reason::None;
        result_.last_box = candidate.box;
        result_.has_box = true;
        result_.last_accepted_capture_us = frame.capture_us;
        result_.confirmations = 0;
        has_pending_ = false;
    } else {
        const auto last_visual = result_.has_box ? result_.last_accepted_capture_us : initialized_capture_us_;
        result_.state = frame.capture_us - last_visual >= config_.lost_after_us ? State::Lost : State::Weak;
    }
    return result_;
}

Observation Session::snapshot(std::uint64_t now_us) {
    std::lock_guard<std::mutex> guard(mutex_);
    age(now_us);
    return result_;
}
Observation Session::camera_failure() {
    std::lock_guard<std::mutex> guard(mutex_);
    fail(Reason::CameraFailure);
    return result_;
}
Observation Session::model_failure(std::uint64_t selection_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (selection_id == result_.selection_id) fail(Reason::ModelFailure);
    return result_;
}
} // namespace rv1106::nano
