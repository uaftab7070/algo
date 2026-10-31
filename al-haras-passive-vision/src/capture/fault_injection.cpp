#include "capture/fault_injection.hpp"

#include "common/clock.hpp"

#include <chrono>
#include <new>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace ahpv {
namespace {

std::uint64_t parse_index(const std::string& text) {
    if (text.empty() || text.size() > 12 || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("fault spec index '" + text + "' is not a non-negative integer");
    return std::stoull(text);
}

int parse_amount(const std::string& text, int max) {
    const std::uint64_t value = parse_index(text);
    if (value == 0 || value > static_cast<std::uint64_t>(max))
        throw std::invalid_argument("fault spec amount '" + text + "' must be 1.." + std::to_string(max));
    return static_cast<int>(value);
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream stream(text);
    while (std::getline(stream, part, separator))
        if (!part.empty()) parts.push_back(part);
    return parts;
}

}  // namespace

FaultSpec parse_fault_spec(const std::string& text) {
    FaultSpec spec;
    spec.text = text;
    for (const auto& clause : split(text, ';')) {
        const auto equals = clause.find('=');
        if (equals == std::string::npos) throw std::invalid_argument("fault clause '" + clause + "' lacks '='");
        const std::string name = clause.substr(0, equals);
        const auto items = split(clause.substr(equals + 1), ',');
        if (items.empty()) throw std::invalid_argument("fault clause '" + clause + "' has no values");
        for (const auto& item : items) {
            const auto colon = item.find(':');
            if (name == "drop") spec.drop.insert(parse_index(item));
            else if (name == "corrupt") spec.corrupt.insert(parse_index(item));
            else if (name == "disconnect") spec.disconnect.insert(parse_index(item));
            else if (name == "alloc_fail") spec.alloc_fail.insert(parse_index(item));
            else if (name == "delay" || name == "nosignal") {
                if (colon == std::string::npos) throw std::invalid_argument(name + " requires INDEX:AMOUNT");
                const auto index = parse_index(item.substr(0, colon));
                if (name == "delay") spec.delay_ms[index] = parse_amount(item.substr(colon + 1), 60000);
                else spec.no_signal[index] = parse_amount(item.substr(colon + 1), 1000);
            } else {
                throw std::invalid_argument("unknown fault clause '" + name + "'");
            }
        }
    }
    if (spec.drop.empty() && spec.corrupt.empty() && spec.delay_ms.empty() && spec.no_signal.empty() &&
        spec.disconnect.empty() && spec.alloc_fail.empty())
        throw std::invalid_argument("fault specification is empty");
    return spec;
}

FaultInjectingSource::FaultInjectingSource(std::unique_ptr<FrameSource> inner, FaultSpec spec, int no_signal_timeout_ms)
    : inner_(std::move(inner)), spec_(std::move(spec)), timeout_ms_(no_signal_timeout_ms) {}

SourceInfo FaultInjectingSource::info() const {
    SourceInfo info = inner_->info();
    info.details.set("fault_injection", spec_.text);
    return info;
}

ReadResult FaultInjectingSource::read(Frame& out) {
    for (;;) {
        if (disconnected_) return {ReadStatus::Disconnected, "fault injection: simulated camera disconnect"};
        const auto no_signal = spec_.no_signal.find(index_);
        if (no_signal != spec_.no_signal.end() && no_signal->second > 0) {
            --no_signal->second;
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms_, 200)));
            return {ReadStatus::NoSignal, "fault injection: simulated no-signal interval"};
        }
        if (spec_.disconnect.count(index_) && fired_.insert(index_ * 4 + 0).second) {
            disconnected_ = true;
            return {ReadStatus::Disconnected, "fault injection: simulated camera disconnect"};
        }
        if (spec_.alloc_fail.count(index_) && fired_.insert(index_ * 4 + 1).second) throw std::bad_alloc();

        const ReadResult result = inner_->read(out);
        if (result.status != ReadStatus::Frame) return result;
        const std::uint64_t index = index_++;
        if (spec_.drop.count(index)) continue;
        if (spec_.corrupt.count(index)) {
            out.flags |= kFlagFaultInjected;
            out.corruption_reason = "fault injection: corrupted payload";
            return {ReadStatus::Corrupted, out.corruption_reason};
        }
        const auto delay = spec_.delay_ms.find(index);
        if (delay != spec_.delay_ms.end()) {
            // A stall delays this and every later frame, as a real pipeline stall would.
            std::this_thread::sleep_for(std::chrono::milliseconds(delay->second));
            offset_ns_ += static_cast<std::int64_t>(delay->second) * 1000000;
            out.acquisition_mono_ns = mono_ns();
            out.acquisition_wall_ns = wall_ns();
            out.flags |= kFlagFaultInjected;
        }
        if (!inner_->info().live) out.source_timestamp_ns += offset_ns_;
        return result;
    }
}

}  // namespace ahpv
