#pragma once

#include "rv1106_tracker/nanotrack_decoder.hpp"
#include <cstdint>
#include <mutex>

namespace rv1106::nano {

enum class State { Uninitialized, Initializing, Tracking, Weak, Lost };
enum class Reason {
    None, SelectTarget, Confirming, LowEvidence, Ambiguous, InvalidEvidence,
    SourceChanged, Expired, CameraFailure, ModelFailure, ClockError
};
const char* name(State state);
const char* name(Reason reason);

struct SessionConfig {
    // Must be selected explicitly from a measured camera-to-result budget.
    std::uint64_t max_observation_age_us{0};
    std::uint64_t lost_after_us{300000};
    unsigned confirmation_frames{3};
    // Development thresholds; not calibrated on aerial drone footage.
    float min_score{0.48F};
    float min_psr{1.8F};
    float min_appearance{0.32F};
    float min_confidence{0.53F};
    float high_confidence{0.69F};
    float max_scale_jump{1.3F};
};

struct FrameStamp {
    std::uint64_t frame_id{0};
    std::uint64_t capture_us{0}; // Same monotonic clock as now_us; not media time.
    std::uint64_t source_epoch{0}; // Change on camera restart or source replacement.
    int width{0};
    int height{0};
};

// Measured by the camera/vision adapter for THIS frame and proposal. These are
// similarities/evidence, not probabilities. Unavailable flow must be marked so.
struct VisualEvidence {
    float appearance{0};
    float original_appearance{0};
    float structure{0};       // Normalized texture correlation in [-1,1].
    float motion_agreement{0};
    bool has_flow{false};
    float flow_agreement{0};
    bool flow_support{false};
};

struct Observation {
    State state{State::Uninitialized};
    Reason reason{Reason::SelectTarget};
    bool accepted{false};
    bool manual_required{false};
    bool has_box{false};
    Box last_box{};           // Last accepted box; never a current prediction.
    FrameStamp frame{};      // Most recent processed frame, including rejections.
    std::uint64_t last_accepted_capture_us{0};
    std::uint64_t age_us{0}; // Age of frame above at snapshot time.
    float confidence{0};
    unsigned confirmations{0};
    std::uint64_t selection_id{0}; // Attach to every asynchronous inference result.
};

// Camera/model execution stays outside this class. All methods synchronize
// their short state transitions so snapshot() can run during stalled inference.
class Session {
public:
    explicit Session(SessionConfig config);
    Observation initialize(const FrameStamp& frame, const Box& selected, std::uint64_t now_us);
    Observation submit(const FrameStamp& frame, const Candidate& candidate,
                       const VisualEvidence& evidence, std::uint64_t now_us,
                       std::uint64_t selection_id);
    Observation snapshot(std::uint64_t now_us);
    Observation camera_failure();
    Observation model_failure(std::uint64_t selection_id);

private:
    SessionConfig config_;
    std::mutex mutex_;
    Observation result_;
    Box selected_{};
    Box pending_{};
    bool has_pending_{false};
    std::uint64_t initialized_capture_us_{0};
    std::uint64_t last_now_us_{0};
    bool has_clock_{false};
    std::uint64_t selection_counter_{0};

    void fail(Reason reason);
    void age(std::uint64_t now_us);
};

} // namespace rv1106::nano
