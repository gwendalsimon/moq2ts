#pragma once

#include <chrono>
#include <cstdint>

namespace moq2ts {

// Microseconds on the monotonic steady clock. Use for deltas only (the absolute
// value is arbitrary). The capture epoch and the pacing start are both readings
// of this clock, so an object's media time (epoch-relative) and the elapsed time
// (start-relative) are comparable.
inline std::int64_t nowSteadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// How long (microseconds) to wait before releasing an object whose media time is
// objectMediaUs, given elapsedUs since the first object and a latency slack.
// Returns 0 when the object is due or overdue (never adds catch-up delay).
inline std::int64_t paceDelayUs(std::int64_t objectMediaUs, std::int64_t elapsedUs,
                                std::int64_t slackUs) {
    const std::int64_t delay = objectMediaUs - slackUs - elapsedUs;
    return delay > 0 ? delay : 0;
}

// Media time of a transport stream, from its PCR samples (27 MHz). Pacing on
// this releases a source at its own rate, whatever the size of an Object.
// The PCR wraps at 2^33 * 300 and may jump (a discontinuity, a source that
// restarted); a step of more than kMaxStepUs forward, or any step backward,
// adds no time, so the clock never stalls the egress for the length of the jump.
class PcrClock {
public:
    static constexpr std::int64_t kRange = (std::int64_t{1} << 33) * 300;
    static constexpr std::int64_t kMaxStepUs = 5000000;

    // pcr27 is the first PCR of an Object, or -1 for none. Returns media time in
    // microseconds since the first PCR seen, or -1 before any.
    std::int64_t update(std::int64_t pcr27) {
        if (pcr27 < 0) {
            return mediaUs_;
        }
        if (mediaUs_ >= 0) {
            const std::int64_t delta = (pcr27 - lastPcr_ + kRange) % kRange;
            if (delta / 27 <= kMaxStepUs) {
                mediaUs_ += delta / 27;
            }
        } else {
            mediaUs_ = 0;
        }
        lastPcr_ = pcr27;
        return mediaUs_;
    }

private:
    std::int64_t lastPcr_ = 0;
    std::int64_t mediaUs_ = -1;
};

}  // namespace moq2ts
