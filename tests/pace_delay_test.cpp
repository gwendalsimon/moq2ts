#include "media/EgressPacing.h"

#include <iostream>
#include <string>

using moq2ts::paceDelayUs;
using moq2ts::PcrClock;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}
}  // namespace

int main() {
    bool ok = true;

    // Ahead of clock: wait ~ mediaUs - slack - elapsed.
    ok &= expect(paceDelayUs(100000, 0, 8000) == 92000, "ahead -> positive delay");

    // Within slack: due now.
    ok &= expect(paceDelayUs(5000, 0, 8000) == 0, "within slack -> 0");

    // Exactly at media time minus slack boundary.
    ok &= expect(paceDelayUs(8000, 0, 8000) == 0, "boundary -> 0");

    // Overdue (elapsed past media time): no catch-up delay.
    ok &= expect(paceDelayUs(50000, 200000, 8000) == 0, "overdue -> 0");

    // Negative media time (garbage/clock-backward): never sleeps.
    ok &= expect(paceDelayUs(-1000, 0, 8000) == 0, "negative media -> 0");

    // Steady-state: object 1s out, 0.9s elapsed, 8ms slack -> ~92ms wait.
    ok &= expect(paceDelayUs(1000000, 900000, 8000) == 92000, "steady-state delay");

    // PCR clock: nothing before the first PCR, then the source's own rate.
    {
        PcrClock clock;
        ok &= expect(clock.update(-1) == -1, "no PCR yet -> -1");
        ok &= expect(clock.update(27000000) == 0, "first PCR is time 0");
        ok &= expect(clock.update(-1) == 0, "object without PCR holds");
        ok &= expect(clock.update(27000000 + 6750000) == 250000, "250 ms of PCR");
        // A 9.95 Mb/s clip in 64 KiB objects: ~52.7 ms each, not 250 ms.
        ok &= expect(clock.update(27000000 + 6750000 + 1422000) == 302666, "object time follows the PCR");
    }

    // PCR wrap at 2^33 * 300 keeps counting forward.
    {
        PcrClock clock;
        clock.update(PcrClock::kRange - 2700000);
        ok &= expect(clock.update(2700000) == 200000, "wrap adds the forward step");
    }

    // A backward step or a jump of more than 5 s adds no time.
    {
        PcrClock clock;
        clock.update(100000000);
        ok &= expect(clock.update(50000000) == 0, "backward step adds nothing");
        ok &= expect(clock.update(50000000 + 27000000) == 1000000, "resumes from the new PCR");
        ok &= expect(clock.update(50000000 + 27000000 + 27ll * 6000000) == 1000000, "jump over 5 s adds nothing");
    }

    return ok ? 0 : 1;
}
