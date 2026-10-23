#include "MptsFanOut.h"

#include "M2tsPacketizer.h"
#include "PacketQueueDevice.h"
#include "SourceGapLog.h"

#include <QDebug>
#include <QFileDevice>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <optional>
#include <utility>

namespace moq2ts {

namespace {
constexpr qsizetype kChunkBytes = 64 * 1024;
constexpr qsizetype kProbeBytes = 192 * 4;   // what the packetizer peeks to find the packet size
constexpr int kPollMs = 50;                  // how often a silent source is left to look at stop()
}

MptsFanOut::MptsFanOut(std::shared_ptr<QIODevice> source, std::shared_ptr<SourceGapLog> sourceGaps)
    : m_source(std::move(source)), m_sourceGaps(std::move(sourceGaps)) {}

MptsFanOut::~MptsFanOut() {
    stop();
}

std::shared_ptr<PacketQueueDevice> MptsFanOut::addOutput(qsizetype capacityBytes) {
    Q_ASSERT(!m_started);
    auto output = std::make_shared<PacketQueueDevice>(capacityBytes, std::make_shared<SourceGapLog>());
    m_outputs.push_back(output);
    return output;
}

// A blocked read(2) on a FIFO cannot be interrupted from another thread, and
// closing the descriptor does not wake it, so the reader polls with a timeout
// and looks at m_stop between polls. The descriptor is read directly, which
// bypasses the device's buffer: the source must be handed over unread. A device
// without a descriptor (a buffer, a socket) goes through QIODevice instead.
qint64 MptsFanOut::readSome(char* data, qint64 maxSize) {
    const auto* file = qobject_cast<QFileDevice*>(m_source.get());
    const int fd = file ? file->handle() : -1;
    if (fd >= 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, kPollMs);
        if (ready < 0) {
            return errno == EINTR ? 0 : -1;
        }
        if (ready == 0) {
            return 0;
        }
        const ssize_t n = ::read(fd, data, static_cast<size_t>(maxSize));
        if (n < 0) {
            return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
        }
        return n == 0 ? -1 : n;   // POLLHUP with nothing to read: the writer closed
    }
    if (m_source->bytesAvailable() <= 0 && !m_source->waitForReadyRead(kPollMs)) {
        if (m_source->atEnd()) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs / 5));
        return m_source->bytesAvailable() > 0 ? m_source->read(data, maxSize) : 0;
    }
    const qint64 n = m_source->read(data, maxSize);
    return n == 0 && m_source->atEnd() ? -1 : n;
}

bool MptsFanOut::start(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (m_started) {
        return fail(QStringLiteral("The fan-out is already started."));
    }
    if (!m_source->isOpen() && !m_source->open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("Cannot open the source: %1").arg(m_source->errorString()));
    }

    // Probe like the packetizer does: up to 768 octets, or the whole of a
    // shorter source. Nothing is discarded: the probe is the start of the
    // stream the outputs get.
    bool ended = false;
    while (m_carry.size() < kProbeBytes && !ended && !m_stop) {
        char buffer[kProbeBytes];
        const qint64 n = readSome(buffer, kProbeBytes - m_carry.size());
        if (n < 0) {
            ended = true;
        } else if (n > 0) {
            m_carry.append(buffer, static_cast<qsizetype>(n));
        }
    }
    if (m_stop) {
        return fail(QStringLiteral("Stopped before the source sent a packet."));
    }
    if (m_carry.size() < 188) {
        return fail(QStringLiteral("Input is too small to contain a TS packet."));
    }
    m_packetSize = detectTsPacketSize(m_carry);
    if (m_packetSize == 0) {
        return fail(QStringLiteral("Input is not packet-aligned TS/M2TS. Expected sync byte at offset 0 for 188-byte TS or offset 4 for 192-byte M2TS."));
    }
    for (const auto& output : m_outputs) {
        output->setPacketSize(m_packetSize);
    }

    // stop() may also come between the check above and here: either it set
    // m_stop before this lock, and no thread starts, or it finds the thread.
    std::lock_guard<std::mutex> lock(m_threadMutex);
    if (m_stop) {
        return fail(QStringLiteral("Stopped before the source sent a packet."));
    }
    m_started = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void MptsFanOut::run() {
    // The probe is still in m_carry and goes out first.
    flush();
    QByteArray buffer(kChunkBytes, Qt::Uninitialized);
    while (!m_stop) {
        const qint64 n = readSome(buffer.data(), buffer.size());
        if (n < 0) {
            break;
        }
        if (n > 0) {
            m_carry.append(buffer.constData(), static_cast<qsizetype>(n));
            flush();
        }
    }
    if (!m_stop) {
        if (!m_carry.isEmpty()) {
            qWarning() << "[MPTS] Dropping" << m_carry.size() << "octets of a partial packet at the end of the source";
        }
        finishOutputs();
    }
}

void MptsFanOut::flush() {
    // Whole packets only: PacketQueueDevice drops overflow in whole packets
    // counted from its start, so every push must keep that alignment.
    const qsizetype whole = m_carry.size() / m_packetSize * m_packetSize;
    qsizetype done = 0;
    while (done < whole) {
        qsizetype end = std::min(whole, done + kChunkBytes / m_packetSize * m_packetSize);
        const std::uint64_t base = m_carryOffset + static_cast<std::uint64_t>(done);
        const std::optional<std::uint64_t> gap = m_sourceGaps ? m_sourceGaps->next() : std::nullopt;
        bool atGap = false;
        if (gap && *gap <= base + static_cast<std::uint64_t>(end - done)) {
            // The gap falls in this chunk, or before it when the reader was late
            // in seeing it. Cut the chunk at the packet boundary at or before it
            // (a gap inside a packet cannot be honored in a packet stream), so
            // the packets after the gap are the ones that follow it.
            const std::uint64_t into = *gap > base ? *gap - base : 0;
            end = done + static_cast<qsizetype>(into / m_packetSize * m_packetSize);
            atGap = true;
        }
        if (end > done) {
            const QByteArray chunk = m_carry.mid(done, end - done);
            for (const auto& output : m_outputs) {
                output->push(chunk);
            }
            done = end;
        }
        if (atGap) {
            unsigned causes = 0;
            m_sourceGaps->takeUpTo(*gap, &causes);
            for (const auto& output : m_outputs) {
                output->markGap(causes);
            }
        }
    }
    m_carryOffset += static_cast<std::uint64_t>(whole);
    m_carry.remove(0, whole);
}

void MptsFanOut::finishOutputs() {
    for (const auto& output : m_outputs) {
        output->finish();
    }
}

void MptsFanOut::stop() {
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(m_threadMutex);
        m_stop = true;
        thread = std::move(m_thread);
    }
    if (thread.joinable()) {
        thread.join();
    }
    for (const auto& output : m_outputs) {
        output->abort();
    }
}

} // namespace moq2ts
