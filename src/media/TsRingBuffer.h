#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace moq2ts {

// Bounded ring buffer for live MPEG-TS ingest.
//
// Why this exists: a live receiver (the SRT thread) must never block. If it does
// - for example because the publisher is still waiting for its first subscriber
// and nothing is draining the pipe - srt_recv() stops being called, SRT's own
// receive buffer overflows and the sender begins discarding packets, so the feed
// degrades within seconds. Buffering here decouples ingest from the consumer.
//
// When the buffer fills, the OLDEST data is discarded rather than stalling the
// receiver, so what is retained is always the most recent media and a consumer
// that attaches late resumes at the live edge instead of replaying stale media.
//
// Two invariants keep the discarded data from corrupting the stream:
//   * drops are made in whole 188-byte TS packets, so the byte stream stays
//     packet aligned across a gap; and
//   * any drop raises a resync flag - resyncToRandomAccess() then tries to skip
//     forward to a random access point (IDR) before anything else is emitted.
//     Its safety fallback keeps the stream moving if no such packet arrives.
//
// Not thread-safe: intended to be appended to and drained from the same thread.
class TsRingBuffer {
public:
    static constexpr std::size_t kTsPacketSize = 188;

    explicit TsRingBuffer(std::size_t capacityBytes)
        : buf_(capacityBytes < kTsPacketSize ? kTsPacketSize : capacityBytes) {}

    std::size_t capacity() const { return buf_.size(); }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    // Total bytes discarded because the consumer could not keep up.
    std::uint64_t droppedBytes() const { return dropped_; }
    bool resyncPending() const { return resyncPending_; }

    // Byte at logical offset i (0 == front of the buffered stream).
    unsigned char at(std::size_t i) const {
        return static_cast<unsigned char>(buf_[(head_ + i) % buf_.size()]);
    }

    // Contiguous readable span at the front (the buffer may wrap, so this can be
    // shorter than size(); drain in a loop).
    const char* frontData() const { return buf_.data() + head_; }
    std::size_t frontContiguous() const {
        const std::size_t toEnd = buf_.size() - head_;
        return size_ < toEnd ? size_ : toEnd;
    }

    void consume(std::size_t n) {
        if (n > size_) {
            n = size_;
        }
        head_ = (head_ + n) % buf_.size();
        size_ -= n;
        headPos_ += n;
    }

    // Appends len bytes, discarding the oldest whole TS packets if that is the
    // only way to make room. Returns the number of bytes discarded (0 normally).
    std::size_t append(const char* data, std::size_t len) {
        if (data == nullptr || len == 0) {
            return 0;
        }
        std::size_t dropped = 0;

        // A single write larger than the whole buffer: keep only its tail.
        if (len > buf_.size()) {
            const std::size_t skip = len - buf_.size();
            data += skip;
            len -= skip;
            dropped += skip;
        }

        if (size_ + len > buf_.size()) {
            // Free space in whole TS packets so the remaining front stays aligned.
            std::size_t need = size_ + len - buf_.size();
            need = ((need + kTsPacketSize - 1) / kTsPacketSize) * kTsPacketSize;
            if (need > size_) {
                need = size_;
            }
            consume(need);
            dropped += need;
        }

        const std::size_t tail = (head_ + size_) % buf_.size();
        const std::size_t toEnd = buf_.size() - tail;
        const std::size_t first = (len < toEnd) ? len : toEnd;
        std::memcpy(buf_.data() + tail, data, first);
        if (len > first) {
            std::memcpy(buf_.data(), data + first, len - first);
        }
        size_ += len;

        if (dropped > 0) {
            dropped_ += dropped;
            if (!resyncPending_) {
                resyncPending_ = true;  // a gap was introduced; re-anchor before emitting
                resyncDropped_ = 0;
                resetScan(-1);
            }
            resyncDropped_ += dropped;
        }
        return dropped;
    }

    // True when the front of the buffer may be emitted. After a drop this skips
    // forward to the next random access point and returns false until one is
    // buffered, so a consumer never receives an undecodable fragment. With
    // preferredPid >= 0, only that PID's random access points count until a
    // whole buffer's worth has passed without one.
    //
    // The receive loop calls this on every pass while a resync is pending, so
    // it must cost in proportion to the data received since the last call, not
    // to the buffer size: a scan of the whole buffer per call starved srt_recv()
    // at high bitrates and turned one drop into a collapse of the SRT link. Each
    // packet is therefore examined once, and the newest random access points
    // seen so far are remembered.
    bool resyncToRandomAccess(int preferredPid = -1) {
        if (!resyncPending_) {
            return true;
        }
        advanceScan(preferredPid);
        // Resume at the NEWEST random access point - the live edge. Resuming at
        // the oldest one would replay everything buffered while the consumer was
        // away, permanently leaving it seconds behind live (the buffer holds
        // several seconds by design, to absorb stalls).
        if (preferredPid >= 0 ? resumeAt(newestPreferredRap_) : resumeAt(newestAnyRap_)) {
            return true;
        }
        if (preferredPid >= 0 && resyncDropped_ >= buf_.size() && resumeAt(newestAnyRap_)) {
            return true;
        }
        const std::size_t n = size_;
        // Safety valve: the buffer stays full for as long as the consumer is
        // stalled, so "full" alone must NOT end the wait - that would resume
        // mid-GOP. Only give up once a whole buffer's worth of data has streamed
        // past without a single random access point, which means the source never
        // sets the indicator; then resume at the next packet boundary so the feed
        // cannot stall permanently.
        if (resyncDropped_ >= buf_.size()) {
            for (std::size_t off = 0; off + 2 * kTsPacketSize <= n; ++off) {
                if (at(off) == 0x47 && at(off + kTsPacketSize) == 0x47) {
                    consume(off);
                    endResync();
                    return true;
                }
            }
        }
        return false;
    }

private:
    static constexpr std::uint64_t kNone = ~std::uint64_t{0};

    // Positions below are absolute stream offsets: headPos_ is the offset of
    // the front byte, so they stay valid as the front moves.
    void resetScan(int pid) {
        scanPid_ = pid;
        scanPos_ = headPos_;
        scanAligned_ = true;
        newestPreferredRap_ = kNone;
        newestAnyRap_ = kNone;
    }

    void endResync() {
        resyncPending_ = false;
        resyncDropped_ = 0;
        resetScan(-1);
    }

    // Examines the whole packets appended since the last call, in 188-byte
    // steps from a packet boundary, and records the newest random access point
    // on the preferred PID and on any PID.
    void advanceScan(int pid) {
        if (pid != scanPid_) {
            resetScan(pid);   // the preferred PID changed: look at the buffer again
        }
        if (scanPos_ < headPos_) {
            scanPos_ = headPos_;   // drops moved the front past the cursor
        }
        const std::uint64_t end = headPos_ + size_;
        while (scanPos_ + kTsPacketSize <= end) {
            const std::size_t off = static_cast<std::size_t>(scanPos_ - headPos_);
            if (at(off) != 0x47) {
                scanAligned_ = false;   // lost the packet framing: search byte by byte
                ++scanPos_;
                continue;
            }
            if (!scanAligned_) {
                // Confirm a sync byte found by search against the next packet.
                if (scanPos_ + 2 * kTsPacketSize > end) {
                    return;   // wait until the next packet is buffered
                }
                if (at(off + kTsPacketSize) != 0x47) {
                    ++scanPos_;
                    continue;
                }
                scanAligned_ = true;
            }
            const bool randomAccess = ((at(off + 3) >> 4) & 0x03) >= 2   // adaptation field present
                                      && at(off + 4) >= 1                 // and not empty
                                      && (at(off + 5) & 0x40) != 0;       // random_access_indicator
            if (randomAccess) {
                newestAnyRap_ = scanPos_;
                if (pid >= 0 && (((at(off + 1) & 0x1F) << 8) | at(off + 2)) == pid) {
                    newestPreferredRap_ = scanPos_;
                }
            }
            scanPos_ += kTsPacketSize;
        }
    }

    // Drops the stale backlog ahead of the random access point at pos.
    bool resumeAt(std::uint64_t pos) {
        if (pos == kNone || pos < headPos_) {
            return false;
        }
        consume(static_cast<std::size_t>(pos - headPos_));
        endResync();
        return true;
    }

    std::vector<char> buf_;
    std::size_t head_ = 0;      // read position
    std::size_t size_ = 0;      // bytes currently buffered
    std::uint64_t dropped_ = 0;    // lifetime bytes discarded
    std::uint64_t resyncDropped_ = 0;  // bytes discarded since the current resync began
    bool resyncPending_ = false;
    // Incremental resync scan (see resyncToRandomAccess).
    std::uint64_t headPos_ = 0;         // absolute stream offset of the front byte
    std::uint64_t scanPos_ = 0;         // next absolute offset to examine
    bool scanAligned_ = true;
    int scanPid_ = -1;
    std::uint64_t newestPreferredRap_ = kNone;
    std::uint64_t newestAnyRap_ = kNone;
};

}  // namespace moq2ts
