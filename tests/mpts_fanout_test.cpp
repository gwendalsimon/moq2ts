#include "media/MptsFanOut.h"
#include "media/PacketQueueDevice.h"
#include "media/SourceGapLog.h"
#include "ts_test_builder.h"

#include <QFile>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <vector>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using moq2ts::MptsFanOut;
using moq2ts::PacketQueueDevice;
using moq2ts::SourceGapLog;
namespace tb = moq2ts::test;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

constexpr int kPacket = 188;
const auto kWait = std::chrono::seconds(10);

// Packet number n: a distinct counter in the payload, so a lost, duplicated, or
// reordered packet shows up as a byte difference.
QByteArray stream(int first, int count) {
    QByteArray out;
    for (int n = first; n < first + count; ++n) {
        QByteArray payload(184, char(n & 0xFF));
        payload[0] = static_cast<char>((n >> 8) & 0xFF);
        out.append(tb::tsPacket(0x100 + (n % 3), n % 7 == 0, payload, n));
    }
    return out;
}

// A FIFO whose write end the test keeps open, so reading it never blocks on
// open() and closing the write end is the end of the source.
struct Fifo {
    std::shared_ptr<QFile> reader;
    int writeFd = -1;

    explicit Fifo(const QString& path) {
        ::mkfifo(path.toLocal8Bit().constData(), 0600);
        writeFd = ::open(path.toLocal8Bit().constData(), O_RDWR);
        reader = std::make_shared<QFile>(path);
        reader->open(QIODevice::ReadOnly);
    }
    ~Fifo() { closeWriter(); }
    void write(const QByteArray& data) {
        qsizetype done = 0;
        while (done < data.size()) {
            const ssize_t n = ::write(writeFd, data.constData() + done, data.size() - done);
            if (n <= 0) { return; }
            done += n;
        }
    }
    void closeWriter() {
        if (writeFd >= 0) { ::close(writeFd); writeFd = -1; }
    }
};

// Reads an output to its end on another thread. The result is empty-handed when
// the end does not come in time, after which stop() frees the reader.
bool drain(const std::shared_ptr<PacketQueueDevice>& out, MptsFanOut& fanOut, QByteArray* data) {
    auto done = std::async(std::launch::async, [out] { return out->readAll(); });
    if (done.wait_for(kWait) != std::future_status::ready) {
        fanOut.stop();
        done.wait();
        return false;
    }
    *data = done.get();
    return true;
}

std::vector<std::uint64_t> takeAll(const std::shared_ptr<SourceGapLog>& log) {
    std::vector<std::uint64_t> all;
    while (const auto next = log->next()) {
        all.push_back(*next);
        log->takeUpTo(*next);
    }
    return all;
}

// The same stream in 192-octet packets: a 4-octet prefix before each TS packet.
QByteArray stream192(int count) {
    QByteArray out;
    const QByteArray ts = stream(0, count);
    for (int n = 0; n < count; ++n) {
        out.append(QByteArray(4, char(n)));
        out.append(ts.mid(n * kPacket, kPacket));
    }
    return out;
}
}  // namespace

int main() {
    bool ok = true;
    QTemporaryDir dir;
    ok &= expect(dir.isValid(), "temporary directory");

    // Two outputs receive the whole stream byte-identical, and the end of the
    // source finishes both.
    {
        const QByteArray data = stream(0, 1500);   // more than one 64 KiB chunk
        Fifo fifo(dir.filePath("a.fifo"));
        MptsFanOut fanOut(fifo.reader, std::make_shared<SourceGapLog>());
        const auto a = fanOut.addOutput(4 * 1024 * 1024);
        const auto b = fanOut.addOutput(4 * 1024 * 1024);
        fifo.write(data.left(1000));   // also a first read shorter than the probe
        std::thread writer([&] { fifo.write(data.mid(1000)); fifo.closeWriter(); });
        QString error;
        ok &= expect(fanOut.start(&error), "start: " + error.toStdString());
        QByteArray gotA, gotB;
        ok &= expect(drain(a, fanOut, &gotA) && drain(b, fanOut, &gotB), "end of source finishes every output");
        writer.join();
        ok &= expect(gotA == data, "output A is byte-identical");
        ok &= expect(gotB == data, "output B is byte-identical");
        ok &= expect(a->atEnd() && b->atEnd(), "outputs are at their end");
    }

    // An output nobody reads overflows while the other receives every packet.
    {
        const QByteArray data = stream(0, 3000);
        Fifo fifo(dir.filePath("b.fifo"));
        MptsFanOut fanOut(fifo.reader, std::make_shared<SourceGapLog>());
        const auto slow = fanOut.addOutput(10 * kPacket);
        const auto fast = fanOut.addOutput(8 * 1024 * 1024);
        std::thread writer([&] { fifo.write(data); fifo.closeWriter(); });
        QString error;
        ok &= expect(fanOut.start(&error), "overflow: start: " + error.toStdString());
        QByteArray got;
        ok &= expect(drain(fast, fanOut, &got), "fan-out is not blocked by the slow output");
        writer.join();
        ok &= expect(got == data, "fast output receives every packet");
        ok &= expect(slow->droppedBytes() > 0 && slow->droppedBytes() % kPacket == 0, "slow output dropped whole packets");
        ok &= expect(fast->droppedBytes() == 0, "fast output dropped nothing");
        // What the slow output keeps is the tail of the stream.
        const QByteArray kept = slow->readAll();
        ok &= expect(kept == data.right(kept.size()), "slow output keeps the newest packets");
        ok &= expect(kept.size() + slow->droppedBytes() == data.size(), "slow output accounts for every byte");
    }

    // A gap in the source log appears in every output, at that output's
    // current offset in the stream its reader sees.
    {
        Fifo fifo(dir.filePath("c.fifo"));
        const auto sourceGaps = std::make_shared<SourceGapLog>();
        MptsFanOut fanOut(fifo.reader, sourceGaps);
        const auto a = fanOut.addOutput(4 * 1024 * 1024);
        const auto small = fanOut.addOutput(12 * kPacket);   // then drops 3 packets of the 15
        fifo.write(stream(0, 10));
        QString error;
        ok &= expect(fanOut.start(&error), "gap: start: " + error.toStdString());
        // Wait until the first ten packets reached the outputs.
        for (int i = 0; i < 1000 && small->bytesAvailable() < 10 * kPacket; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        sourceGaps->record(10 * kPacket);   // the writer records, then writes on
        fifo.write(stream(10, 5));
        fifo.closeWriter();
        QByteArray got;
        ok &= expect(drain(a, fanOut, &got), "gap: end of source");
        ok &= expect(got == stream(0, 15), "gap: output A keeps every packet");
        ok &= expect(a->gaps()->next() == std::optional<std::uint64_t>(10 * kPacket), "gap: output A records the source offset");
        // The small output holds 12: pushing the 5 packets after the gap drops 3
        // before it, so its reader sees the gap after 7 packets. The gap reaches
        // its log when the reader gets there.
        small->readAll();
        ok &= expect(small->gaps()->next() == std::optional<std::uint64_t>(7 * kPacket),
                     "gap: slow output records the offset in its reader's stream");
        ok &= expect(a->gaps()->takeUpTo(100 * kPacket) && !a->gaps()->next(), "gap: exactly one gap in output A");
    }

    // Gaps inside a chunk, inside a packet, past a 64 KiB chunk, and several in
    // one read. They are in the log before the source is read, so the cut
    // points do not depend on how the reads fall. A gap inside a packet is
    // cut at the packet before it.
    for (const int packetSize : {188, 192}) {
        const std::string label = packetSize == 192 ? "192-octet: " : "188-octet: ";
        const QByteArray data = packetSize == 192 ? stream192(1000) : stream(0, 1000);
        Fifo fifo(dir.filePath(packetSize == 192 ? "g192.fifo" : "g188.fifo"));
        const auto sourceGaps = std::make_shared<SourceGapLog>();
        const std::uint64_t p = packetSize;
        sourceGaps->record(5 * p);          // on a boundary, inside the probe
        sourceGaps->record(7 * p + 50);     // inside a packet: cut at 7
        sourceGaps->record(8 * p + 1);      // inside the next packet: cut at 8
        sourceGaps->record(600 * p);        // in a later 64 KiB chunk
        sourceGaps->record(600 * p + 10);   // the same packet: no second gap
        MptsFanOut fanOut(fifo.reader, sourceGaps);
        const auto a = fanOut.addOutput(4 * 1024 * 1024);
        const auto small = fanOut.addOutput(20 * packetSize);
        std::thread writer([&] { fifo.write(data); fifo.closeWriter(); });
        QString error;
        ok &= expect(fanOut.start(&error), label + "start: " + error.toStdString());
        QByteArray got;
        ok &= expect(drain(a, fanOut, &got), label + "end of source");
        writer.join();
        ok &= expect(got == data, label + "output A keeps every byte");
        ok &= expect(takeAll(a->gaps()) == std::vector<std::uint64_t>({5 * p, 7 * p, 8 * p, 600 * p}),
                     label + "gaps cut at packet boundaries");
        const QByteArray tail = small->readAll();
        ok &= expect(tail == data.right(tail.size()) && tail.size() <= 20 * packetSize, label + "small output keeps the newest packets");
        // Every gap lies before the 20 packets it keeps and its reader never
        // read before: they collapse into overflow gaps at offset 0, which is no gap.
        ok &= expect(takeAll(small->gaps()).empty(), label + "gaps before the dropped packets collapse");
    }

    // stop() returns promptly when the source is silent, and aborts the outputs.
    {
        Fifo fifo(dir.filePath("d.fifo"));
        MptsFanOut fanOut(fifo.reader, std::make_shared<SourceGapLog>());
        const auto out = fanOut.addOutput(1024 * 1024);
        fifo.write(stream(0, 20));
        QString error;
        ok &= expect(fanOut.start(&error), "stop: start: " + error.toStdString());
        auto reader = std::async(std::launch::async, [out] { return out->read(1000 * kPacket).size(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto begin = std::chrono::steady_clock::now();
        fanOut.stop();
        ok &= expect(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2), "stop returns promptly with a blocked source");
        ok &= expect(reader.wait_for(kWait) == std::future_status::ready, "stop wakes a blocked reader of an output");
        if (reader.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { reader.get(); }
    }

    // stop() from another thread while start() waits for the first packets:
    // start() fails, and no reader thread is left behind.
    {
        Fifo fifo(dir.filePath("s.fifo"));
        MptsFanOut fanOut(fifo.reader, std::make_shared<SourceGapLog>());
        const auto out = fanOut.addOutput(1024 * 1024);
        auto starting = std::async(std::launch::async, [&fanOut] {
            QString error;
            const bool started = fanOut.start(&error);
            return std::make_pair(started, error);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        fanOut.stop();
        ok &= expect(starting.wait_for(kWait) == std::future_status::ready, "stop during start: start returns");
        if (starting.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto [started, error] = starting.get();
            ok &= expect(!started && error.contains("Stopped"), "stop during start: start fails: " + error.toStdString());
        }
        fifo.write(stream(0, 20));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ok &= expect(out->bytesAvailable() == 0, "stop during start: nothing reads the source after stop");
    }

    // A source that is not TS is refused; a trailing partial packet is dropped.
    {
        Fifo fifo(dir.filePath("e.fifo"));
        MptsFanOut fanOut(fifo.reader, std::make_shared<SourceGapLog>());
        fanOut.addOutput(1024 * 1024);
        fifo.write(QByteArray(1000, char(0x55)));
        fifo.closeWriter();
        QString error;
        ok &= expect(!fanOut.start(&error) && !error.isEmpty(), "a source that is not TS is refused");
    }
    {
        Fifo fifo(dir.filePath("f.fifo"));
        MptsFanOut fanOut(fifo.reader, std::make_shared<SourceGapLog>());
        const auto out = fanOut.addOutput(1024 * 1024);
        fifo.write(stream(0, 5) + stream(5, 1).left(100));
        fifo.closeWriter();
        QString error;
        ok &= expect(fanOut.start(&error), "partial: start: " + error.toStdString());
        QByteArray got;
        ok &= expect(drain(out, fanOut, &got) && got == stream(0, 5), "only whole packets are pushed");
    }

    std::cerr << (ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
