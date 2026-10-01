#include "media/M2tsPacketizer.h"
#include "ts_test_builder.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdint>
#include <iostream>
#include <string>

using moq2ts::M2tsObject;
using moq2ts::M2tsPacketizer;
namespace tb = moq2ts::test;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// PAT, one PMT per program, then video packets on PID 0x100.
QByteArray stream(const QList<std::pair<int, int>>& programs) {
    QByteArray ts = tb::psiPacket(0x0000, tb::patSection(programs));
    for (const auto& [program, pmtPid] : programs) {
        ts += tb::psiPacket(pmtPid, tb::pmtSection(program, 0x100, {{0x1B, 0x100}, {0x0F, 0x101}}));
    }
    for (int cc = 0; cc < 8; ++cc) {
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0x00)), cc);
    }
    return ts;
}
}  // namespace

int main() {
    bool ok = true;
    QTemporaryDir dir;
    ok &= expect(dir.isValid(), "temporary directory");

    const QString spts = dir.filePath("spts.ts");
    const QString mpts = dir.filePath("mpts.ts");
    // program_number 0 is the network PID and does not count as a program.
    ok &= expect(writeFile(spts, stream({{0, 0x10}, {1, 0x1000}})), "write SPTS");
    ok &= expect(writeFile(mpts, stream({{1, 0x1000}, {2, 0x1001}})), "write MPTS");

    // Transparent scan: the program count decides between the unmodified modes.
    for (const bool transparent : {true, false}) {
        const std::string label = transparent ? "transparent" : "filtered";
        M2tsPacketizer single(spts);
        single.setTransparent(transparent);
        QString error;
        ok &= expect(single.open(0, &error), label + " SPTS opens: " + error.toStdString());
        ok &= expect(single.patProgramCount() == 1, label + " SPTS lists one program");
        ok &= expect(single.programNumber() == 1, label + " SPTS program number");

        M2tsPacketizer multiple(mpts);
        multiple.setTransparent(transparent);
        ok &= expect(multiple.open(0, &error), label + " MPTS opens: " + error.toStdString());
        ok &= expect(multiple.patProgramCount() == 2, label + " MPTS lists two programs");
    }

    // A transparent SPTS ignores a requested program number that it does not carry.
    {
        M2tsPacketizer single(spts);
        single.setTransparent(true);
        QString error;
        ok &= expect(single.open(7, &error), "transparent SPTS opens with --program 7");
        ok &= expect(single.programNumber() == 1 && single.pcrPid() == 0x100,
                     "transparent SPTS describes its own program");
    }

    // initData holds every packet of a PMT that spans several packets.
    {
        QList<std::pair<int, int>> streams{{0x1B, 0x100}};
        for (int index = 0; index < 99; ++index) {
            streams.append({0x06, 0x200 + index});
        }
        const QByteArray pat = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        const QList<QByteArray> pmt = tb::psiPackets(0x1000, tb::pmtSection(1, 0x100, streams));
        QByteArray ts = pat;
        for (const QByteArray& packet : pmt) {
            ts += packet;
        }
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)));
        const QString path = dir.filePath("long-pmt.ts");
        ok &= expect(writeFile(path, ts) && pmt.size() == 3, "write long-PMT stream");
        M2tsPacketizer packetizer(path);
        QString error;
        ok &= expect(packetizer.open(0, &error), "long PMT opens: " + error.toStdString());
        ok &= expect(packetizer.initData() == ts.left(4 * 188), "initData holds the PAT and all three PMT packets");
    }

    // Object media time: the PTS of the first video PES in each Object, on the
    // PMT's video PID even when an audio PES comes first and the PCR has its own
    // PID. Two packets per Object.
    {
        constexpr std::uint64_t kWrap = std::uint64_t{1} << 33;
        const auto video = [](std::uint64_t pts, int cc) { return tb::tsPacket(0x100, true, tb::pesHeaderWithPts(pts), cc); };
        const auto audio = [](std::uint64_t pts, int cc) { return tb::tsPacket(0x101, true, tb::pesHeaderWithPts(pts), cc); };
        QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
        ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x1FF, {{0x0F, 0x101}, {0x1B, 0x100}}));
        ts += audio(1000, 0) + video(900900, 0);                               // Object 1
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), 1) + audio(2000, 1);  // Object 2
        ts += video(kWrap - 90, 2) + tb::tsPacket(0x1FF, false, QByteArray(184, char(0)), 0);  // Object 3
        ts += video(450, 3) + video(451, 4);                                   // Object 4
        const QString path = dir.filePath("pts.ts");
        ok &= expect(writeFile(path, ts), "write PTS stream");

        for (const bool transparent : {true, false}) {
            const std::string label = transparent ? "transparent" : "filtered";
            M2tsPacketizer packetizer(path);
            packetizer.setTransparent(transparent);
            QString error;
            ok &= expect(packetizer.open(0, &error), label + " PTS stream opens: " + error.toStdString());
            M2tsObject object;
            ok &= expect(packetizer.readObject(2, &object, &error) && !object.ptsUs.has_value(),
                         label + ": PAT and PMT carry no PTS");
            ok &= expect(packetizer.readObject(2, &object, &error) && object.ptsUs == std::uint64_t{10010000},
                         label + ": video PTS, not the audio PTS before it");
            ok &= expect(packetizer.readObject(2, &object, &error) && !object.ptsUs.has_value(),
                         label + ": no video PES start, no PTS");
            ok &= expect(packetizer.readObject(2, &object, &error) && object.ptsUs == (kWrap - 90) * 100 / 9,
                         label + ": PTS before the wrap");
            ok &= expect(packetizer.readObject(2, &object, &error) && object.ptsUs == (kWrap + 450) * 100 / 9,
                         label + ": PTS unwrapped past 2^33");
        }
    }

    // A looped source steps the PTS back; the unwrapped value never goes back by
    // more than the B-frame reordering allowance.
    {
        moq2ts::PtsUnwrapper unwrapper;
        ok &= expect(unwrapper.unwrap(1000000) == 1000000, "unwrap: first value");
        ok &= expect(unwrapper.unwrap(997000) == 997000, "unwrap: B-frame step back kept");
        ok &= expect(unwrapper.unwrap(9000) == 997000, "unwrap: loop does not go back");
        ok &= expect(unwrapper.unwrap(12003) == 1000003, "unwrap: media time continues after the loop");
    }

    if (ok) {
        std::cout << "m2ts packetizer tests passed\n";
    }
    return ok ? 0 : 1;
}
