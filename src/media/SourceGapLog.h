#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace moq2ts {

// Where a live source's byte stream skips ahead.
//
// The SRT receive thread discards the oldest buffered media when nothing reads
// the pipe for longer than its ring holds, then resumes at a random access
// point. Only that thread knows where the gap falls, while the packetizer on
// the other end of the pipe decides the Group boundaries. A gap inside a Group
// is a PCR discontinuity inside it, which the draft forbids ("PCR and Timing"),
// and the unmodified modes may not mark it in the packets. So the writer
// records the offset of each gap in the byte stream it writes, and the reader
// starts a new Group there.
//
// An offset counts bytes written to the pipe, so the first byte written after
// the gap sits at that offset. Thread-safe: one writer, one reader.
class SourceGapLog {
public:
    // A gap at offset 0 has nothing before it, so it is no gap for the reader.
    void record(std::uint64_t offset) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (offset > 0 && (m_gaps.empty() || m_gaps.back() < offset)) {
            m_gaps.push_back(offset);
        }
    }

    // The first gap not yet taken, if any.
    std::optional<std::uint64_t> next() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_gaps.empty()) {
            return std::nullopt;
        }
        return m_gaps.front();
    }

    // Removes every gap at or before offset. True when there was one.
    bool takeUpTo(std::uint64_t offset) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool taken = false;
        while (!m_gaps.empty() && m_gaps.front() <= offset) {
            m_gaps.pop_front();
            taken = true;
        }
        return taken;
    }

private:
    mutable std::mutex m_mutex;
    std::deque<std::uint64_t> m_gaps;
};

} // namespace moq2ts
