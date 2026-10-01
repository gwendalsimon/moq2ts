#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <QImage>
#include <QObject>

#include "../app/PublishConfig.h"
#include "../publish/MoqxrPublisher.h"
#include "M2tsPacketizer.h"

namespace moq2ts {

// State the nextObject lambda mutates while publishing.
//
// This is heap-allocated and captured BY VALUE (as a shared_ptr) rather than left on
// runLoop()'s stack and captured by reference. That matters because the lambda is
// handed to the publisher and invoked from moqxr's own publish thread, while
// waitForStopped() gives the worker 3 seconds and then DETACHES it. If the worker's
// frame unwinds while moqxr still holds the lambda, by-reference captures point at
// reclaimed stack and writing through them corrupts the heap -- observed as
// "double free or corruption" and "corrupted size vs. prev_size" at shutdown, and
// correlated with a stalled SRT source, which is what makes the 3 s deadline slip.
//
// Sharing ownership means the state simply outlives whichever side finishes last.
struct PublishState {
    std::int64_t objects = 0;
    std::int64_t bytes = 0;
    // MSF media timeline history (draft-ietf-moq-msf-01 Section 7.3): every
    // record so far, comma-separated, and the last media Group recorded.
    QByteArray timelineRecords;
    std::optional<std::uint64_t> timelineGroupId;
    std::int64_t pacingStartUs = -1;
    std::optional<PublishedObject> pendingTimeline;
    std::shared_ptr<M2tsPacketizer> packetizer;   // file / SRT path
    std::shared_ptr<void> capture;                // capture path (LibavCaptureSource)
};

class LivePipeline final : public QObject {
    Q_OBJECT
public:
    explicit LivePipeline(QObject* parent = nullptr);
    ~LivePipeline() override;

    void start(const PublishConfig& cfg, MoqxrPublisher* publisher);
    void stop();
    void requestStop();
    void waitForStopped();
    bool running() const;

signals:
    void status(const QString& message);
    void error(const QString& message);
    void stats(int64_t packets, int64_t bytes, int64_t groups);
    void previewVideoFrame(const QImage& image);
    void previewAudioLevels(double left, double right);

private:
    void runLoop();

    PublishConfig m_config;
    MoqxrPublisher* m_publisher = nullptr;

    std::atomic<bool> m_running {false};
    std::thread m_workerThread;

    std::mutex m_doneMutex;
    std::condition_variable m_doneCv;
    bool m_workerDone = false;
};

} // namespace moq2ts
