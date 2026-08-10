#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace moq2ts {

/// Connects to an SRT source in caller mode and delivers received TS packets
/// to a callback. The callback receives raw TS data (188-byte packets) in the
/// same format as a file read - the existing M2tsPacketizer/LivePipeline can
/// consume it transparently via a pipe or QFile on /dev/stdin.
///
/// For moq2ts's m2ts packaging the SRT data is raw TS - no CMAF remux needed.
class SrtSource {
public:
    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 9000;
        int latencyMs = 120;
        int timeoutMs = 200; // recv timeout for stop-check polling
        // Receive headroom. These cost no added latency (unlike latencyMs) and
        // protect against the receiver briefly falling behind: if the kernel UDP
        // socket or SRT's own buffer overflows, SRT reports loss and drops packets
        // that miss the latency budget, punching a hole in the transport stream.
        // Sized for a 6 Mbit/s CBR feed with plenty of margin; 0 = library default.
        int rcvBufBytes = 8 * 1024 * 1024;    // SRTO_RCVBUF
        int udpRcvBufBytes = 8 * 1024 * 1024; // SRTO_UDP_RCVBUF (kernel socket)
    };

    /// Opens an SRT caller connection to host:port. Blocks until connected or error.
    /// Returns empty string on success, error message on failure.
    static QString connect(const Config& cfg, int& sockOut);

    /// Receive loop: reads TS data from the SRT socket and writes it to the
    /// given QIODevice (typically the write end of a pipe). Runs until
    /// stopRequested is set. Call from a worker thread.
    static void receiveLoop(int sock, int pipeFd, std::atomic<bool>& stopRequested);

    /// Clean up the SRT socket.
    static void close(int sock);

    /// Shut the SRT library down (pairs with the implicit srt_startup() done by
    /// connect()). Call once at process shutdown, after close() and after the
    /// receive thread has been joined. No-op when SRT was never started.
    static void shutdown();
};

} // namespace moq2ts
