#include "app/PublishConfig.h"
#include "media/LivePipeline.h"
#include "publish/MoqxrPublisher.h"
#include "ts_test_builder.h"

#include <QCoreApplication>
#include <QFile>
#include <QMutex>
#include <QStringList>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace tb = moq2ts::test;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

QMutex g_mutex;
QStringList g_published;   // the mock publisher's "MOCK MSFTS publish" lines

void capture(QtMsgType, const QMessageLogContext&, const QString& message) {
    if (message.contains(QStringLiteral("MOCK MSFTS publish"))) {
        QMutexLocker locker(&g_mutex);
        g_published.append(message);
    }
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// One program whose video starts with a random access point, so a live source
// keeps every packet after the PSI.
QByteArray stream() {
    QByteArray ts = tb::psiPacket(0x0000, tb::patSection({{1, 0x1000}}));
    ts += tb::psiPacket(0x1000, tb::pmtSection(1, 0x100, {{0x1B, 0x100}}));
    ts += tb::tsPacket(0x100, true, tb::pesHeaderWithPts(9000), 0, 0x40);
    for (int cc = 1; cc < 8; ++cc) {
        ts += tb::tsPacket(0x100, false, QByteArray(184, char(0)), cc);
    }
    return ts;
}

// Publishes source to the mock transport and returns the published lines.
QStringList publish(const QString& source) {
    {
        QMutexLocker locker(&g_mutex);
        g_published.clear();
    }
    moq2ts::PublishConfig cfg;
    cfg.moqEndpoint = QStringLiteral("mock://local");
    cfg.videoSource = source;
    cfg.transparentMode = true;
    cfg.fragmentDurationMs = 0;   // no mock pacing
    cfg.targetSegmentBytes = 2 * 188;
    moq2ts::MoqxrPublisher publisher;
    moq2ts::LivePipeline pipeline;
    if (!publisher.connect(cfg)) {
        return {};
    }
    pipeline.start(cfg, &publisher);
    for (int i = 0; i < 1000 && pipeline.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pipeline.stop();
    QMutexLocker locker(&g_mutex);
    return g_published;
}

int catalogCount(const QStringList& lines) {
    return static_cast<int>(lines.filter(QStringLiteral("publish \"catalog\"")).size());
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler(capture);
    bool ok = true;
    QTemporaryDir dir;
    ok &= expect(dir.isValid(), "temporary directory");

    // A live source (a FIFO) ends the broadcast with the catalog of MSF
    // Section 11.3, after every media Object.
    {
        const QString fifo = dir.filePath("live.fifo");
        ok &= expect(::mkfifo(fifo.toLocal8Bit().constData(), 0600) == 0, "mkfifo");
        std::thread writer([&] { writeFile(fifo, stream()); });
        const QStringList lines = publish(fifo);
        writer.join();
        ok &= expect(lines.size() > 1, "live: media Objects published");
        ok &= expect(catalogCount(lines) == 1, "live: one closing catalog, got " + std::to_string(catalogCount(lines)));
        ok &= expect(!lines.isEmpty() && lines.last().contains(QStringLiteral("publish \"catalog\"")),
                     "live: the closing catalog is the last Object");
        ok &= expect(!lines.isEmpty() && lines.last().contains(QStringLiteral("ends_broadcast")),
                     "live: the closing catalog ends the broadcast");
    }

    // A live source that ends on an error (a packet without its sync byte)
    // makes no commitment that no content follows: no closing catalog.
    {
        const QString fifo = dir.filePath("broken.fifo");
        ok &= expect(::mkfifo(fifo.toLocal8Bit().constData(), 0600) == 0, "mkfifo");
        std::thread writer([&] { writeFile(fifo, stream() + QByteArray(188, char(0x00))); });
        const QStringList lines = publish(fifo);
        writer.join();
        ok &= expect(lines.size() > 1, "error: media Objects published");
        ok &= expect(catalogCount(lines) == 0, "error: no closing catalog");
    }

    // A file is VOD (isLive false): no closing catalog.
    {
        const QString file = dir.filePath("vod.ts");
        ok &= expect(writeFile(file, stream()), "write file");
        const QStringList lines = publish(file);
        ok &= expect(lines.size() > 1, "file: media Objects published");
        ok &= expect(catalogCount(lines) == 0, "file: no closing catalog");
    }

    if (ok) {
        std::cout << "live pipeline end tests passed\n";
    }
    return ok ? 0 : 1;
}
