#pragma once
// Test fixture: wraps a source and injects declared faults so detection and
// reporting can be verified. Every affected frame carries kFlagFaultInjected and
// the session manifest records the specification; injected data is never presented
// as genuine input.
//
// Spec grammar (indices count frames read from the wrapped source):
//   drop=I[,I...]        discard frames upstream (produces a sequence gap)
//   corrupt=I[,I...]     deliver as corrupted (must be rejected)
//   delay=I:MS[,I:MS]    stall: wait MS before frame I; file timestamps shift from I on
//   nosignal=I:COUNT     report COUNT no-signal intervals before frame I
//   disconnect=I         simulated disconnect before frame I (reopen resumes)
//   alloc_fail=I         throw std::bad_alloc before frame I (memory-pressure path)
// Clauses are separated by ';', e.g. "drop=5;corrupt=9;disconnect=30".
#include "capture/source.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>

namespace ahpv {

struct FaultSpec {
    std::set<std::uint64_t> drop, corrupt;
    std::map<std::uint64_t, int> delay_ms, no_signal;
    std::set<std::uint64_t> disconnect, alloc_fail;
    std::string text;
};

FaultSpec parse_fault_spec(const std::string& text);  // Throws std::invalid_argument.

class FaultInjectingSource : public FrameSource {
public:
    FaultInjectingSource(std::unique_ptr<FrameSource> inner, FaultSpec spec, int no_signal_timeout_ms);
    SourceInfo info() const override;
    ReadResult read(Frame& out) override;
    bool reopenable() const override { return true; }
    void reopen() override { disconnected_ = false; }

private:
    std::unique_ptr<FrameSource> inner_;
    FaultSpec spec_;
    int timeout_ms_;
    std::uint64_t index_{0};
    bool disconnected_{false};
    std::int64_t offset_ns_{0};
    std::set<std::uint64_t> fired_;
};

}  // namespace ahpv
