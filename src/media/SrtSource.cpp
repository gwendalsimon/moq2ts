#include "SrtSource.h"

#include "TsRingBuffer.h"

#include <srt/srt.h>

#include <QDebug>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace moq2ts {

namespace {
bool& srtInitialized() {
    static bool initialized = false;
    return initialized;
}

void ensureSrtInit() {
    if (!srtInitialized()) {
        srt_startup();
        srtInitialized() = true;
    }
}
} // namespace

QString SrtSource::connect(const Config& cfg, int& sockOut) {
    ensureSrtInit();
    sockOut = SRT_INVALID_SOCK;

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    const std::string portStr = std::to_string(cfg.port);
    if (getaddrinfo(cfg.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return QStringLiteral("SRT: cannot resolve %1:%2").arg(QString::fromStdString(cfg.host)).arg(cfg.port);
    }

    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        freeaddrinfo(res);
        return QStringLiteral("SRT: srt_create_socket failed: %1").arg(srt_getlasterror_str());
    }

    // Options
    int latency = cfg.latencyMs;
    srt_setsockflag(sock, SRTO_RCVLATENCY, &latency, sizeof(latency));
    srt_setsockflag(sock, SRTO_PEERLATENCY, &latency, sizeof(latency));
    int timeout = cfg.timeoutMs;
    srt_setsockflag(sock, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
    // Receive headroom (must be set before connect). Absorbs transient scheduling
    // delays so the kernel/SRT buffers do not overflow and force packet drops.
    if (cfg.rcvBufBytes > 0) {
        int rcvBuf = cfg.rcvBufBytes;
        srt_setsockflag(sock, SRTO_RCVBUF, &rcvBuf, sizeof(rcvBuf));
    }
    if (cfg.udpRcvBufBytes > 0) {
        int udpRcvBuf = cfg.udpRcvBufBytes;
        srt_setsockflag(sock, SRTO_UDP_RCVBUF, &udpRcvBuf, sizeof(udpRcvBuf));
    }
    int no = 0;
    srt_setsockflag(sock, SRTO_SNDSYN, &no, sizeof(no)); // non-blocking send (unused but safe)

    // Connect (caller mode)
    if (srt_connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SRT_ERROR) {
        const QString err = QStringLiteral("SRT: connect to %1:%2 failed: %3")
                                .arg(QString::fromStdString(cfg.host))
                                .arg(cfg.port)
                                .arg(srt_getlasterror_str());
        srt_close(sock);
        freeaddrinfo(res);
        return err;
    }
    freeaddrinfo(res);

    sockOut = sock;
    return QString();
}

void SrtSource::receiveLoop(int sock, int pipeFd, std::atomic<bool>& stopRequested) {
    // SRT delivers TS in pkt_size chunks (typically 1316 = 7x188).
    //
    // The receive path must never block. Writing straight to the pipe did block:
    // while the publisher waits for its first subscriber nothing drains the pipe,
    // so once it filled, ::write() stalled, srt_recv() stopped being called, SRT's
    // receive buffer overflowed and the sender began discarding packets - the feed
    // degraded within seconds.
    //
    // Instead, received data goes into a bounded ring buffer and the pipe is
    // written non-blocking. While the reader is stalled the ring keeps the most
    // recent media and discards the oldest, so the wait is effectively unbounded;
    // when the reader starts draining, output resumes at the next random access
    // point, so it stays decodable and the subscriber joins at the live edge
    // rather than replaying stale media.
    constexpr int kBufSize = 1316 * 4;                            // up to 28 TS packets per recv
    constexpr std::size_t kRingCapacityBytes = 4u * 1024 * 1024;  // ~5 s at 6 Mbit/s

    // Non-blocking pipe: a stalled reader must never stall the receiver.
    const int flags = ::fcntl(pipeFd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(pipeFd, F_SETFL, flags | O_NONBLOCK);
    }

    TsRingBuffer ring(kRingCapacityBytes);
    std::uint64_t reportedDrops = 0;
    auto lastDropLog = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto lastStatsLog = lastDropLog;
    int64_t lastRcvDrop = 0;
    int64_t lastRcvLoss = 0;
    char buf[kBufSize];

    // Push as much as the reader will currently take; never blocks.
    const auto drainToPipe = [&]() {
        if (ring.empty()) {
            return;
        }
        // Only touch the pipe - and only pay for a resync scan - when the reader
        // can actually accept data; otherwise leave everything buffered.
        struct pollfd pfd{};
        pfd.fd = pipeFd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLOUT) == 0) {
            return;
        }
        if (!ring.resyncToRandomAccess()) {
            return;  // gap not yet re-anchored on an IDR; nothing safe to emit
        }
        while (!ring.empty()) {
            const ssize_t n = ::write(pipeFd, ring.frontData(), ring.frontContiguous());
            if (n > 0) {
                ring.consume(static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                return;  // nothing accepted; keep the rest buffered (never fatal)
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  // reader is behind; keep the rest buffered
            }
            qWarning() << "[SRT] Pipe write failed:" << strerror(errno);
            stopRequested.store(true, std::memory_order_release);
            return;
        }
    };

    while (!stopRequested.load(std::memory_order_acquire)) {
        drainToPipe();

        int received = srt_recv(sock, buf, kBufSize);
        if (received == SRT_ERROR) {
            int err = srt_getlasterror(nullptr);
            if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) {
                continue; // timeout - check stop flag and retry
            }
            SRT_SOCKSTATUS st = srt_getsockstate(sock);
            if (st == SRTS_BROKEN || st == SRTS_CLOSING || st == SRTS_CLOSED) {
                qWarning() << "[SRT] Connection lost:" << srt_getlasterror_str();
                break;
            }
            continue;
        }
        if (received <= 0) {
            continue;
        }

        ring.append(buf, static_cast<std::size_t>(received));

        // Report ANY ring drop, rate-limited by time rather than volume. Reporting
        // only per buffer's-worth hid small drops entirely, which made a real media
        // gap look like it had no local cause.
        const std::uint64_t dropped = ring.droppedBytes();
        const auto now = std::chrono::steady_clock::now();
        if (dropped > reportedDrops && now - lastDropLog >= std::chrono::seconds(2)) {
            qWarning() << "[SRT] ring dropped" << (dropped - reportedDrops)
                       << "bytes (total" << dropped << ") - consumer behind";
            reportedDrops = dropped;
            lastDropLog = now;
        }

        // SRT's own receive-side loss counters. These are the only way to see
        // packets SRT discarded because they missed the latency budget
        // (TLPKTDROP); such loss is invisible to every check downstream of here.
        if (now - lastStatsLog >= std::chrono::seconds(2)) {
            lastStatsLog = now;
            SRT_TRACEBSTATS st{};
            if (srt_bstats(sock, &st, 0) == 0) {
                if (st.pktRcvDropTotal != lastRcvDrop || st.pktRcvLossTotal != lastRcvLoss) {
                    qWarning() << "[SRT] receiver loss: dropped(TLPKTDROP)="
                               << st.pktRcvDropTotal << "(+" << (st.pktRcvDropTotal - lastRcvDrop)
                               << ") lost=" << st.pktRcvLossTotal
                               << "(+" << (st.pktRcvLossTotal - lastRcvLoss)
                               << ") retrans=" << st.pktRetransTotal;
                    lastRcvDrop = st.pktRcvDropTotal;
                    lastRcvLoss = st.pktRcvLossTotal;
                }
            }
        }
    }
}

void SrtSource::close(int sock) {
    if (sock != SRT_INVALID_SOCK) {
        srt_close(sock);
    }
}

void SrtSource::shutdown() {
    // srt_startup() must be paired with srt_cleanup() before the process exits.
    // Without it, libsrt's own static destructors run at exit (via __cxa_finalize)
    // while its internals are still live and jump through a stale function
    // pointer - a SIGSEGV during teardown, after all useful work is done.
    // Call this once, after every SRT socket is closed and the receive thread has
    // been joined. Safe to call when SRT was never started.
    if (srtInitialized()) {
        srt_cleanup();
        srtInitialized() = false;
    }
}

} // namespace moq2ts
