#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace moq2ts {

class PacketQueueDevice;
class SourceGapLog;

// What each program of a multiplex may queue before overflow drops its oldest
// packets. Every output holds the packets of the whole multiplex, so this is
// multiplex time, about 1.7 s at 40 Mbit/s: how long a program's packetizer
// may stall before its queue drops packets and marks a gap.
constexpr qsizetype kMptsOutputCapacityBytes = 8 * 1024 * 1024;

// Reads a live multi-program source once and hands every packet to every
// output, so that one M2tsPacketizer per program, each on its own thread,
// reads the whole multiplex without a second reader on the source.
//
// The reader thread never waits for an output: PacketQueueDevice::push() drops
// the oldest packets of an output that falls behind and records the gap in its
// SourceGapLog, so one slow program cannot stall the others or the source.
//
// A gap in the source's gap log (SRT ring overflow, encoder reconnection) is
// marked in every output before the next byte pushed to it. The output keeps
// the marker in the offset scale of its reader (the packetizer counts the bytes
// it has read, and the dropped ones never reach it) and moves it when a later
// overflow drops the bytes before it. See PacketQueueDevice::markGap().
class MptsFanOut {
public:
    MptsFanOut(std::shared_ptr<QIODevice> source, std::shared_ptr<SourceGapLog> sourceGaps);
    ~MptsFanOut();
    MptsFanOut(const MptsFanOut&) = delete;
    MptsFanOut& operator=(const MptsFanOut&) = delete;

    // Before start() only. The caller gives output->gaps() to its packetizer.
    std::shared_ptr<PacketQueueDevice> addOutput(qsizetype capacityBytes = kMptsOutputCapacityBytes);

    // Reads the first bytes of the source to find the packet size (waiting for
    // them on the calling thread, so stop() from another thread ends the wait),
    // then reads on a thread of its own. False with an error when the source is
    // not TS, ends before one packet, or was stopped.
    bool start(QString* error);

    // Joins the thread and aborts the outputs. Returns promptly even when the
    // source is silent.
    void stop();

private:
    struct Output;

    // Reads up to maxSize bytes into data, waiting at most a short while, so
    // the caller can look at m_stop. Returns the count, 0 for none yet, -1 at
    // the end of the source or on an error.
    qint64 readSome(char* data, qint64 maxSize);
    void run();
    // Pushes the whole packets of m_carry, splitting at the source gaps.
    void flush();
    void finishOutputs();

    std::shared_ptr<QIODevice> m_source;
    std::shared_ptr<SourceGapLog> m_sourceGaps;
    std::vector<std::shared_ptr<PacketQueueDevice>> m_outputs;

    int m_packetSize = 0;
    QByteArray m_carry;                  // read from the source, not yet pushed
    std::uint64_t m_carryOffset = 0;     // source offset of m_carry[0]
    // Guards m_thread, and m_stop where start() decides to start the thread,
    // so stop() from another thread during start() is race-free.
    std::mutex m_threadMutex;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    bool m_started = false;
};

} // namespace moq2ts
