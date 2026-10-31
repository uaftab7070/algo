#include "rv1106_tracker/nanotrack_decoder.hpp"
#include "rv1106_tracker/nanotrack_session.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace rv1106::nano;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
SessionConfig config() {
    SessionConfig c;
    c.max_observation_age_us = 200000;
    return c;
}
Candidate strong() { return {{140, 65, 60, 90}, 0.95F, 6.0F, false, 8, 8}; }
VisualEvidence evidence() { return {0.95F, 0.95F, 0.95F, 1.0F, false, 0, false}; }

struct Fixture {
    Session session{config()};
    FrameStamp frame{0, 1000000, 1, 400, 240};
    std::uint64_t selection{0};
    Fixture() {
        const auto r = session.initialize(frame, strong().box, frame.capture_us);
        require(r.state == State::Initializing && !r.accepted && !r.has_box, "selection cannot confirm a target");
        selection = r.selection_id;
    }
    Observation step(Candidate c = strong(), VisualEvidence e = evidence(), std::uint64_t dt = 33000) {
        ++frame.frame_id;
        frame.capture_us += dt;
        return session.submit(frame, c, e, frame.capture_us, selection);
    }
    Observation confirm() {
        require(!step().accepted, "first confirmation accepted");
        require(!step().accepted, "second confirmation accepted");
        const auto r = step();
        require(r.accepted && r.state == State::Tracking && r.has_box, "third confirmation was not accepted");
        return r;
    }
};

void confirmation_and_duplicate_frames() {
    Fixture f;
    auto r = f.step();
    const auto first = r.frame;
    require(r.confirmations == 1, "missing initial confirmation");
    for (int i = 0; i < 5; ++i) {
        r = f.session.submit(first, strong(), evidence(), first.capture_us + i, f.selection);
        require(!r.accepted && r.confirmations == 1, "duplicate confirmed recovery");
    }
    require(!f.step().accepted, "duplicate advanced confirmation count");
    require(f.step().accepted, "distinct frames failed confirmation");
}

void frame_id_and_timestamp_must_both_advance() {
    Fixture f;
    const auto first = f.step();
    auto wrong = first.frame;
    ++wrong.frame_id;
    auto r = f.session.submit(wrong, strong(), evidence(), wrong.capture_us, f.selection);
    require(r.confirmations == 1, "same timestamp with newer ID advanced state");
    wrong = first.frame;
    ++wrong.capture_us;
    r = f.session.submit(wrong, strong(), evidence(), wrong.capture_us, f.selection);
    require(r.confirmations == 1, "same ID with newer timestamp advanced state");
}

void watchdog_expires_without_frames() {
    Fixture f;
    const auto good = f.confirm();
    auto r = f.session.snapshot(good.frame.capture_us + 199999);
    require(r.accepted, "observation expired before deadline");
    r = f.session.snapshot(good.frame.capture_us + 200000);
    require(!r.accepted && r.manual_required && r.state == State::Lost && r.confidence == 0,
            "deadline did not invalidate accepted result");
    require(r.has_box && r.last_box.x == good.last_box.x, "last-seen box was not retained");
    r = f.session.submit({20, good.frame.capture_us + 200001, 1, 400, 240},
                         strong(), evidence(), good.frame.capture_us + 200001, f.selection);
    require(!r.accepted && r.reason == Reason::Expired, "late inference cleared watchdog fault");
}

void rejected_frames_never_move_last_accepted_box() {
    Fixture f;
    const auto good = f.confirm();
    auto bad = strong(); bad.score = 0.1F; bad.box.x = 250;
    const auto r = f.step(bad);
    require(!r.accepted && r.state == State::Weak && r.last_box.x == good.last_box.x,
            "rejected proposal moved last accepted target");
}

void lost_state_uses_elapsed_time_and_recovery_needs_confirmation() {
    Fixture f;
    f.confirm();
    auto bad = strong(); bad.score = 0.1F;
    for (int i = 0; i < 9; ++i)
        require(f.step(bad).state == State::Weak, "frame count caused premature loss");
    require(f.step(bad).state == State::Lost, "time-based loss threshold ignored");
    require(!f.step().accepted && !f.step().accepted, "recovery accepted too early");
    require(f.step().accepted, "recovery did not regain confirmed target");
}

void missing_evidence_breaks_confirmation_streak() {
    Fixture f;
    f.step(); f.step();
    auto bad = strong(); bad.score = 0.1F;
    require(f.step(bad).confirmations == 0, "missing evidence preserved confirmations");
    require(!f.step().accepted && !f.step().accepted && f.step().accepted,
            "confirmation streak did not restart");
}

void spatially_inconsistent_proposals_do_not_confirm() {
    Fixture f;
    for (int i = 0; i < 8; ++i) {
        auto c = strong(); c.box.x = i % 2 ? 280.0F : 20.0F;
        const auto r = f.step(c);
        require(!r.accepted && r.confirmations == 1, "separate candidates confirmed a target");
    }
}

void ambiguous_target_is_rejected_even_with_flow() {
    Fixture f; f.confirm();
    auto c = strong(); c.ambiguous = true;
    auto e = evidence(); e.has_flow = e.flow_support = true; e.flow_agreement = 1;
    const auto r = f.step(c, e);
    require(!r.accepted && r.reason == Reason::Ambiguous, "ambiguous competing identities accepted");
}

void camera_restart_is_a_latched_fault() {
    Fixture f; f.confirm();
    auto restart = f.frame;
    ++restart.source_epoch; restart.frame_id = 0;
    const auto r = f.session.submit(restart, strong(), evidence(), restart.capture_us, f.selection);
    require(!r.accepted && r.manual_required && r.reason == Reason::SourceChanged,
            "camera restart with reset frame IDs was missed");
    require(!f.step().accepted, "camera restart fault was not latched");
}

void source_resolution_change_requires_reselection() {
    Fixture f; f.confirm();
    f.frame.width = 800;
    require(f.step().reason == Reason::SourceChanged, "resolution change accepted");
}

void stale_selection_cannot_overwrite_a_new_target() {
    Fixture f;
    const auto old_selection = f.selection;
    auto r = f.session.initialize(f.frame, {200, 65, 60, 90}, f.frame.capture_us);
    require(r.selection_id != old_selection, "selection generation was reused");
    f.selection = r.selection_id;
    ++f.frame.frame_id; f.frame.capture_us += 33000;
    r = f.session.submit(f.frame, strong(), evidence(), f.frame.capture_us, old_selection);
    require(!r.accepted && r.confirmations == 0, "old inference affected new selection");
    r = f.session.model_failure(old_selection);
    require(!r.manual_required, "old worker fault invalidated new selection");
}

void failed_selection_clears_previous_acceptance() {
    Fixture f; f.confirm();
    const auto r = f.session.initialize(f.frame, {0, 0, 2, 2}, f.frame.capture_us);
    require(!r.accepted && !r.has_box && r.manual_required, "bad selection retained old target");
}

void invalid_numeric_evidence_fails_closed() {
    Fixture f; f.confirm();
    auto c = strong(); c.box.x = std::numeric_limits<float>::quiet_NaN();
    auto r = f.step(c);
    require(!r.accepted && r.manual_required && r.reason == Reason::InvalidEvidence, "NaN box accepted");
    Fixture g; g.confirm();
    auto e = evidence(); e.structure = std::numeric_limits<float>::infinity();
    r = g.step(strong(), e);
    require(!r.accepted && r.manual_required, "infinite visual evidence accepted");
}

void unsupported_flow_cannot_override_neural_evidence() {
    Fixture f;
    auto e = evidence(); e.flow_support = true;
    require(f.step(strong(), e).reason == Reason::InvalidEvidence, "unavailable flow used as evidence");
}

void cropped_or_excessively_scaled_targets_are_rejected() {
    for (const auto box : {Box{-10, 65, 60, 90}, Box{140, 65, 90, 90}, Box{140, 65, 60, 140}}) {
        Fixture f; f.confirm();
        auto c = strong(); c.box = box;
        require(!f.step(c).accepted, "unverified target extent accepted");
    }
}

void camera_and_model_faults_invalidate_immediately() {
    Fixture f; f.confirm();
    auto r = f.session.camera_failure();
    require(!r.accepted && r.confidence == 0 && r.reason == Reason::CameraFailure, "camera failure ignored");
    Fixture g; g.confirm();
    r = g.session.model_failure(g.selection);
    require(!r.accepted && r.confidence == 0 && r.reason == Reason::ModelFailure, "model failure ignored");
}

void clock_errors_do_not_underflow_age() {
    Fixture f; f.confirm();
    auto r = f.session.snapshot(f.frame.capture_us - 1);
    require(!r.accepted && r.reason == Reason::ClockError, "clock rollback ignored");
    Fixture g;
    auto future = g.frame; ++future.frame_id; future.capture_us += 50000;
    r = g.session.submit(future, strong(), evidence(), g.frame.capture_us, g.selection);
    require(!r.accepted && r.reason == Reason::ClockError, "future capture timestamp accepted");
}

void invalid_config_requires_explicit_deadline() {
    bool threw = false;
    try { Session s(SessionConfig{}); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "missing observation age budget accepted");
    auto c = config(); c.confirmation_frames = 0;
    threw = false;
    try { Session s(c); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "zero confirmation frames accepted");
}

void decoder_uses_raw_score_and_detects_separate_rival() {
    std::array<float, 2 * 256> cls{};
    std::array<float, 4 * 256> dist{};
    dist.fill(30);
    cls[256 + 8 * 16 + 8] = 4;
    TensorView a{cls.data(), sizeof(cls), 2}, b{dist.data(), sizeof(dist), 4};
    auto r = decode(a, b, strong().box);
    require(r.peak_x == 8 && r.peak_y == 8 && !r.ambiguous, "wrong response peak");
    require(std::abs(r.score - 1.0 / (1 + std::exp(-4.0))) < 1e-6, "window contaminated raw confidence");
    cls[256 + 1 * 16 + 1] = 4;
    r = decode(a, b, strong().box);
    require(r.ambiguous, "spatially separate equally strong rival not detected");
}

void decoder_rejects_short_and_nonfinite_buffers() {
    std::array<float, 2 * 256> cls{};
    std::array<float, 4 * 256> dist{}; dist.fill(30);
    TensorView a{cls.data(), 1, 2}, b{dist.data(), sizeof(dist), 4};
    bool threw = false;
    try { decode(a, b, strong().box); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "undersized buffer was read");
    a.byte_size = sizeof(cls);
    dist[0] = std::numeric_limits<float>::quiet_NaN();
    threw = false;
    try { decode(a, b, strong().box); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "nonfinite model output was decoded");
}

void snapshots_can_run_concurrently_with_measurement_submission() {
    Session s(config());
    FrameStamp frame{0, 1000000, 1, 400, 240};
    const auto initial = s.initialize(frame, strong().box, 1001000);
    std::atomic<bool> bad{false};
    std::thread reader([&] {
        for (int i = 0; i < 1000; ++i) {
            const auto r = s.snapshot(1001000);
            if (r.accepted && (!r.has_box || r.manual_required || r.confidence <= 0 || r.confidence > 1))
                bad = true;
        }
    });
    for (int i = 0; i < 1000; ++i) {
        ++frame.frame_id; ++frame.capture_us;
        s.submit(frame, strong(), evidence(), 1001000, initial.selection_id);
    }
    reader.join();
    require(!bad && s.snapshot(1001000).accepted, "concurrent snapshot observed inconsistent state");
}
} // namespace

int main() {
    const std::array<std::pair<const char*, void(*)()>, 21> checks{{
        {"confirmation and duplicates", confirmation_and_duplicate_frames},
        {"frame ordering", frame_id_and_timestamp_must_both_advance},
        {"watchdog", watchdog_expires_without_frames},
        {"rejected localization", rejected_frames_never_move_last_accepted_box},
        {"loss and recovery", lost_state_uses_elapsed_time_and_recovery_needs_confirmation},
        {"broken confirmations", missing_evidence_breaks_confirmation_streak},
        {"inconsistent recovery", spatially_inconsistent_proposals_do_not_confirm},
        {"ambiguity", ambiguous_target_is_rejected_even_with_flow},
        {"source epoch", camera_restart_is_a_latched_fault},
        {"source size", source_resolution_change_requires_reselection},
        {"selection generation", stale_selection_cannot_overwrite_a_new_target},
        {"bad selection", failed_selection_clears_previous_acceptance},
        {"invalid evidence", invalid_numeric_evidence_fails_closed},
        {"unsupported flow", unsupported_flow_cannot_override_neural_evidence},
        {"target extent", cropped_or_excessively_scaled_targets_are_rejected},
        {"camera and model failure", camera_and_model_faults_invalidate_immediately},
        {"clock errors", clock_errors_do_not_underflow_age},
        {"configuration", invalid_config_requires_explicit_deadline},
        {"raw score and rival", decoder_uses_raw_score_and_detects_separate_rival},
        {"invalid buffers", decoder_rejects_short_and_nonfinite_buffers},
        {"concurrent snapshots", snapshots_can_run_concurrently_with_measurement_submission}
    }};
    for (const auto& check : checks) {
        try { check.second(); }
        catch (const std::exception& e) {
            std::cerr << "FAIL " << check.first << ": " << e.what() << '\n';
            return 1;
        }
    }
    std::cout << checks.size() << " native checks passed\n";
    return 0;
}
