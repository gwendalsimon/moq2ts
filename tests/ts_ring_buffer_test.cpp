// Pure logic tests for the live-ingest ring buffer (no Qt/libav/SRT deps).
#include "media/TsRingBuffer.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using moq2ts::TsRingBuffer;

namespace {

bool expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << '\n';
        return false;
    }
    return true;
}

constexpr std::size_t kPkt = TsRingBuffer::kTsPacketSize;

// Build one 188-byte TS packet. rai=true sets random_access_indicator (an IDR).
std::vector<char> tsPacket(int pid, bool rai, unsigned char fill) {
    std::vector<char> p(kPkt, static_cast<char>(fill));
    p[0] = 0x47;
    p[1] = static_cast<char>((pid >> 8) & 0x1f);
    p[2] = static_cast<char>(pid & 0xff);
    if (rai) {
        p[3] = 0x30;  // adaptation field + payload
        p[4] = 0x07;  // adaptation field length
        p[5] = 0x40;  // random_access_indicator
    } else {
        p[3] = 0x10;  // payload only, no adaptation field
    }
    return p;
}

void appendPacket(TsRingBuffer& ring, int pid, bool rai, unsigned char fill) {
    const std::vector<char> p = tsPacket(pid, rai, fill);
    ring.append(p.data(), p.size());
}

// Copy the whole buffered stream out, draining as a consumer would.
std::vector<unsigned char> drainAll(TsRingBuffer& ring) {
    std::vector<unsigned char> out;
    while (!ring.empty()) {
        const std::size_t n = ring.frontContiguous();
        const char* p = ring.frontData();
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(static_cast<unsigned char>(p[i]));
        }
        ring.consume(n);
    }
    return out;
}

}  // namespace

int main() {
    bool ok = true;

    // --- 1. Under capacity: nothing is dropped and the bytes survive intact. ----
    {
        TsRingBuffer ring(10 * kPkt);
        for (int i = 0; i < 5; ++i) {
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        ok &= expect(ring.size() == 5 * kPkt, "5 packets buffered");
        ok &= expect(ring.droppedBytes() == 0, "no drops under capacity");
        ok &= expect(!ring.resyncPending(), "no resync needed under capacity");
        ok &= expect(ring.resyncToRandomAccess(), "emit allowed with no gap");
        const std::vector<unsigned char> out = drainAll(ring);
        ok &= expect(out.size() == 5 * kPkt, "drained all bytes");
        ok &= expect(out[0] == 0x47 && out[kPkt] == 0x47, "packet alignment preserved");
        ok &= expect(out[kPkt * 4 + 10] == 4, "last packet payload intact");
    }

    // --- 2. Overflow drops the OLDEST and keeps the newest (live edge). --------
    {
        TsRingBuffer ring(4 * kPkt);
        for (int i = 0; i < 6; ++i) {  // 2 packets more than it can hold
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        ok &= expect(ring.size() == 4 * kPkt, "buffer capped at capacity");
        ok &= expect(ring.droppedBytes() == 2 * kPkt, "dropped exactly the 2 oldest packets");
        ok &= expect(ring.droppedBytes() % kPkt == 0, "drops are whole TS packets");
        ok &= expect(ring.resyncPending(), "a drop raises the resync flag");
        // Payload marker of the front packet should now be 2 (0 and 1 discarded).
        ok &= expect(ring.at(10) == 2, "oldest discarded, newest retained");
        ok &= expect(ring.at(0) == 0x47, "front is still a packet boundary");
    }

    // --- 3. After a gap, output resumes at a random access point. --------------
    {
        TsRingBuffer ring(4 * kPkt);
        for (int i = 0; i < 6; ++i) {  // force a drop -> resync pending
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        ok &= expect(ring.resyncPending(), "resync pending after drop");
        // No IDR buffered yet: nothing may be emitted.
        ok &= expect(!ring.resyncToRandomAccess(), "no emit until an IDR arrives");
        // An IDR arrives (this also evicts an older packet, which is fine).
        appendPacket(ring, 0x100, true, 0xAA);
        ok &= expect(ring.resyncToRandomAccess(), "emit allowed once an IDR is buffered");
        ok &= expect(!ring.resyncPending(), "resync flag cleared");
        ok &= expect(ring.at(0) == 0x47, "resumed on a packet boundary");
        ok &= expect((ring.at(5) & 0x40) != 0, "resumed exactly at the IDR");
        ok &= expect(ring.size() == kPkt, "everything before the IDR was discarded");
    }

    // --- 3b. Resume at the NEWEST IDR (live edge), not the oldest. ------------
    //      Resuming at the oldest buffered IDR would replay everything captured
    //      while the consumer was away, leaving it permanently seconds behind.
    {
        TsRingBuffer ring(10 * kPkt);
        for (int i = 0; i < 12; ++i) {  // overflow -> resync pending
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        ok &= expect(ring.resyncPending(), "resync pending after drop");
        // Two IDRs are now buffered; the newest is 4 packets from the end.
        appendPacket(ring, 0x100, true, 100);   // older IDR
        appendPacket(ring, 0x100, false, 101);
        appendPacket(ring, 0x100, false, 102);
        appendPacket(ring, 0x100, true, 200);   // NEWEST IDR  <- expected resume
        appendPacket(ring, 0x100, false, 201);
        appendPacket(ring, 0x100, false, 202);
        appendPacket(ring, 0x100, false, 203);

        ok &= expect(ring.resyncToRandomAccess(), "resync succeeds with IDRs buffered");
        ok &= expect(ring.at(0) == 0x47, "resumed on a packet boundary");
        ok &= expect((ring.at(5) & 0x40) != 0, "resumed on an IDR");
        ok &= expect(ring.at(10) == 200, "resumed at the NEWEST IDR, not the older one");
        ok &= expect(ring.size() == 4 * kPkt, "stale backlog ahead of the live edge discarded");
    }

    // --- 4. A merely-full buffer must NOT give up waiting for an IDR. ---------
    //     (It stays full the whole time the consumer is stalled; giving up here
    //      would resume mid-GOP and defeat the point of the resync.)
    {
        TsRingBuffer ring(4 * kPkt);
        for (int i = 0; i < 5; ++i) {  // just enough to overflow by one packet
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        ok &= expect(ring.size() == ring.capacity(), "buffer is full");
        ok &= expect(ring.resyncPending(), "resync pending");
        ok &= expect(!ring.resyncToRandomAccess(), "full alone does not end the wait");
    }

    // --- 4b. Safety valve: a source that never sets RAI still makes progress. --
    {
        TsRingBuffer ring(4 * kPkt);
        // Stream a full buffer's worth past the resync point with no RAI at all.
        for (int i = 0; i < 12; ++i) {
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
            ring.resyncToRandomAccess();  // a consumer trying to drain each time
        }
        ok &= expect(ring.resyncToRandomAccess(), "valve opens after a buffer's worth with no IDR");
        ok &= expect(!ring.resyncPending(), "resync flag cleared by the valve");
        ok &= expect(ring.at(0) == 0x47, "fallback landed on a packet boundary");
    }

    // --- 4c. A preferred PID: resume at its newest IDR, not at a newer IDR of
    //      another program; fall back to any IDR after a buffer's worth. -------
    {
        TsRingBuffer ring(10 * kPkt);
        for (int i = 0; i < 12; ++i) {
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        appendPacket(ring, 0x100, true, 100);   // reference program's IDR
        appendPacket(ring, 0x100, false, 101);
        appendPacket(ring, 0x200, true, 200);   // a newer IDR of another program
        appendPacket(ring, 0x200, false, 201);
        ok &= expect(ring.resyncToRandomAccess(0x100), "resync on the preferred PID");
        ok &= expect(ring.at(10) == 100, "resumed at the preferred PID's IDR, not the newer one");

        TsRingBuffer other(4 * kPkt);
        for (int i = 0; i < 5; ++i) {
            appendPacket(other, 0x200, false, static_cast<unsigned char>(i));
        }
        appendPacket(other, 0x200, true, 50);
        ok &= expect(!other.resyncToRandomAccess(0x100), "another PID's IDR does not end the wait at once");
        appendPacket(other, 0x200, false, 60);
        ok &= expect(!other.resyncToRandomAccess(0x100), "still waiting for the preferred PID");
        appendPacket(other, 0x200, false, 61);   // a buffer's worth has now been dropped
        ok &= expect(other.resyncToRandomAccess(0x100) && other.at(10) == 50,
                     "after a buffer's worth without the preferred PID, any IDR resumes");
    }

    // --- 4d. The resync scan is incremental: a random access point that arrives
    //      over several calls is found, the newest one wins across calls, one
    //      recorded and then dropped no longer counts, framing is recovered
    //      after junk bytes, and a change of preferred PID looks again. ---------
    {
        TsRingBuffer ring(6 * kPkt);
        for (int i = 0; i < 7; ++i) {
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        ok &= expect(!ring.resyncToRandomAccess(0x100), "incremental: waiting, nothing to resume at");
        appendPacket(ring, 0x100, true, 70);
        ok &= expect(!ring.resyncToRandomAccess(0x200), "incremental: an IDR of another PID does not count");
        ok &= expect(ring.resyncPending(), "incremental: still pending");
        appendPacket(ring, 0x100, false, 71);
        appendPacket(ring, 0x100, true, 72);
        ok &= expect(ring.resyncToRandomAccess(0x100) && ring.at(10) == 72,
                     "incremental: the PID change looks again, and the newest IDR across calls wins");
        ok &= expect(ring.size() == kPkt, "incremental: the backlog before the newest IDR is gone");

        TsRingBuffer dropped(3 * kPkt);
        for (int i = 0; i < 4; ++i) {
            appendPacket(dropped, 0x100, false, static_cast<unsigned char>(i));
        }
        appendPacket(dropped, 0x100, true, 40);
        appendPacket(dropped, 0x200, false, 41);
        appendPacket(dropped, 0x200, false, 42);
        ok &= expect(dropped.resyncToRandomAccess(0x100) && dropped.at(10) == 40,
                     "incremental: resumes at the IDR while it is still buffered");
        for (int i = 0; i < 4; ++i) {
            appendPacket(dropped, 0x100, false, static_cast<unsigned char>(50 + i));
        }
        appendPacket(dropped, 0x100, true, 60);   // recorded by the next call...
        ok &= expect(dropped.resyncToRandomAccess(0x100) && dropped.at(10) == 60,
                     "incremental: a second gap resumes at its own IDR");
        for (int i = 0; i < 2; ++i) {
            appendPacket(dropped, 0x100, false, static_cast<unsigned char>(70 + i));
        }
        appendPacket(dropped, 0x100, false, 72);
        appendPacket(dropped, 0x100, false, 73);   // drops the front, raising a resync
        ok &= expect(dropped.resyncPending() && !dropped.resyncToRandomAccess(0x100),
                     "incremental: no IDR left in the buffer, so it waits");

        TsRingBuffer junk(8 * kPkt);
        const char garbage[5] = {0x11, 0x22, 0x47, 0x33, 0x44};   // a stray 0x47 among them
        junk.append(garbage, sizeof(garbage));
        for (int i = 0; i < 8; ++i) {
            appendPacket(junk, 0x100, false, static_cast<unsigned char>(i));
        }
        appendPacket(junk, 0x100, true, 90);
        appendPacket(junk, 0x100, false, 91);
        ok &= expect(junk.resyncToRandomAccess(0x100) && junk.at(0) == 0x47 && junk.at(10) == 90,
                     "incremental: framing recovered after junk bytes, resumed at the IDR");
    }

    // --- 4e. Cost: a full buffer without a random access point is examined once,
    //      not on every call. The SRT receive loop calls this on each pass while
    //      a resync is pending; a scan of the whole buffer per call starved
    //      srt_recv() at high bitrates. ------------------------------------------
    {
        TsRingBuffer ring(4u * 1024 * 1024);
        const std::vector<char> p = tsPacket(0x102, false, 0);
        for (std::size_t i = 0; i < ring.capacity() / kPkt + 10; ++i) {
            ring.append(p.data(), p.size());
        }
        const auto start = std::chrono::steady_clock::now();
        bool resumed = false;
        for (int i = 0; i < 2000; ++i) {
            resumed |= ring.resyncToRandomAccess(0x100);
            ring.append(p.data(), p.size());   // a little new data per call, as on the wire
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count();
        ok &= expect(!resumed, "cost: no IDR, no resume");
        ok &= expect(ms < 200, "cost: 2000 calls on a full 4 MB buffer take " + std::to_string(ms) +
                                   " ms (a scan of the whole buffer per call took about 4 ms each)");
    }

    // --- 5. Wrap-around: appends spanning the end of the buffer stay correct. --
    {
        TsRingBuffer ring(4 * kPkt);
        for (int i = 0; i < 3; ++i) {
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        ring.consume(2 * kPkt);            // move head forward so the next append wraps
        for (int i = 3; i < 6; ++i) {
            appendPacket(ring, 0x100, false, static_cast<unsigned char>(i));
        }
        ok &= expect(ring.droppedBytes() == 0, "no drop: consumed space was reused");
        const std::vector<unsigned char> out = drainAll(ring);
        ok &= expect(out.size() == 4 * kPkt, "4 packets survived the wrap");
        for (std::size_t i = 0; i < 4; ++i) {
            ok &= expect(out[i * kPkt] == 0x47, "each wrapped packet starts with sync");
            ok &= expect(out[i * kPkt + 10] == static_cast<unsigned char>(i + 2),
                         "wrapped packet payloads in order");
        }
    }

    // --- 6. A single append larger than the whole buffer keeps its tail. ------
    {
        TsRingBuffer ring(2 * kPkt);
        std::vector<char> big;
        for (int i = 0; i < 5; ++i) {
            const std::vector<char> p = tsPacket(0x100, false, static_cast<unsigned char>(i));
            big.insert(big.end(), p.begin(), p.end());
        }
        ring.append(big.data(), big.size());
        ok &= expect(ring.size() == 2 * kPkt, "clamped to capacity");
        ok &= expect(ring.at(0) == 0x47, "tail kept, still packet aligned");
        ok &= expect(ring.at(10) == 3, "kept the two NEWEST packets (3 and 4)");
    }

    std::cout << (ok ? "ts-ring-buffer tests passed\n" : "ts-ring-buffer tests FAILED\n");
    return ok ? 0 : 1;
}
