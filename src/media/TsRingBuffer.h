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
//   * any drop raises a resync flag - resyncToRandomAccess() then skips forward
//     to a random access point (IDR) before anything else is emitted, so the
//     output remains decodable across the discontinuity.
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
            }
            resyncDropped_ += dropped;
        }
        return dropped;
    }

    // True when the front of the buffer may be emitted. After a drop this skips
    // forward to the next random access point and returns false until one is
    // buffered, so a consumer never receives an undecodable fragment.
    bool resyncToRandomAccess() {
        if (!resyncPending_) {
            return true;
        }
        const std::size_t n = size_;
        // Scan BACKWARD so we resume at the NEWEST random access point - the live
        // edge. Resuming at the oldest one would replay everything buffered while
        // the consumer was away, permanently leaving it seconds behind live (the
        // buffer holds several seconds by design, to absorb stalls). Searching from
        // the end also costs less: it stops within one GOP instead of scanning all.
        if (n >= kTsPacketSize) {
            for (std::size_t off = n - kTsPacketSize + 1; off-- > 0;) {
                if (at(off) != 0x47) {
                    continue;
                }
                // Confirm framing against the following packet when it is buffered.
                if (off + 2 * kTsPacketSize <= n && at(off + kTsPacketSize) != 0x47) {
                    continue;
                }
                const int adaptationControl = (at(off + 3) >> 4) & 0x03;
                if (adaptationControl < 2) {
                    continue;  // no adaptation field, so no random_access_indicator
                }
                if (at(off + 4) < 1) {
                    continue;  // empty adaptation field
                }
                if ((at(off + 5) & 0x40) == 0) {
                    continue;  // random_access_indicator clear
                }
                consume(off);  // drop the stale backlog ahead of the newest IDR
                resyncPending_ = false;
                resyncDropped_ = 0;
                return true;
            }
        }

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
                    resyncPending_ = false;
                    resyncDropped_ = 0;
                    return true;
                }
            }
        }
        return false;
    }

private:
    std::vector<char> buf_;
    std::size_t head_ = 0;      // read position
    std::size_t size_ = 0;      // bytes currently buffered
    std::uint64_t dropped_ = 0;    // lifetime bytes discarded
    std::uint64_t resyncDropped_ = 0;  // bytes discarded since the current resync began
    bool resyncPending_ = false;
};

}  // namespace moq2ts
