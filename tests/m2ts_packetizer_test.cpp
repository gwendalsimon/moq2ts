#include "media/M2tsPacketizer.h"
#include "ts_test_builder.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

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

    if (ok) {
        std::cout << "m2ts packetizer tests passed\n";
    }
    return ok ? 0 : 1;
}
