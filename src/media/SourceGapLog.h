#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace moq2ts {

// Where a live source's byte stream skips ahead.
//
// The SRT receive thread discards the oldest buffered media when nothing reads
// the pipe for longer than its ring holds, then tries to resume at a random
// access point. Its safety fallback can resume at a packet boundary. Only that
// thread knows where the gap falls, while the packetizer on the other end of
// the pipe decides the Group boundaries. A gap inside a Group
// is a PCR discontinuity inside it, which the draft forbids ("PCR and Timing"),
// and the unmodified modes may not mark it in the packets. So the writer
// records the offset of each gap in the byte stream it writes, and the reader
// starts a new Group there.
//
// An offset counts bytes written to the pipe, so the first byte written after
// the gap sits at that offset. Thread-safe: one writer, one reader.
class SourceGapLog {
public:
    // The hop that skipped ahead, for the log lines that report a gap. Two
    // hops can skip ahead at one offset, so a gap holds a set of these bits.
    enum Cause : unsigned {
        SrtReceiveBuffer = 1u << 0,   // the SRT ring kept only its newest media
        ProgramQueue = 1u << 1,       // a program's PacketQueueDevice overflowed
        SrtNewCaller = 1u << 3,       // an SRT listener accepted a caller after the first one
    };

    // A gap at offset 0 has nothing before it, so it is no gap for the reader.
    // A second gap at the offset of the last one adds its causes to it.
    void record(std::uint64_t offset, unsigned causes = SrtReceiveBuffer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (offset == 0) {
            return;
        }
        if (m_gaps.empty() || m_gaps.back().first < offset) {
            m_gaps.emplace_back(offset, causes);
        } else if (m_gaps.back().first == offset) {
            m_gaps.back().second |= causes;
        }
    }

    // The first gap not yet taken, if any.
    std::optional<std::uint64_t> next() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_gaps.empty()) {
            return std::nullopt;
        }
        return m_gaps.front().first;
    }

    // Removes every gap at or before offset, and adds their causes to causes
    // when given. True when there was one.
    bool takeUpTo(std::uint64_t offset, unsigned* causes = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool taken = false;
        while (!m_gaps.empty() && m_gaps.front().first <= offset) {
            if (causes) {
                *causes |= m_gaps.front().second;
            }
            m_gaps.pop_front();
            taken = true;
        }
        return taken;
    }

private:
    mutable std::mutex m_mutex;
    std::deque<std::pair<std::uint64_t, unsigned>> m_gaps;   // offset, causes
};

} // namespace moq2ts
