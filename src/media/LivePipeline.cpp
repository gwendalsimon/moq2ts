#include "LivePipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <QDateTime>
#include <QDebug>
#include <QThread>

#include "EgressPacing.h"
#include "LibavCaptureSource.h"
#include "MsftsMuxer.h"

namespace moq2ts {

namespace {

std::uint64_t nowUnixUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// Latency slack (us) before an object is considered due. Small so steady-state
// latency stays near zero; only objects arriving ahead of their media time wait.
constexpr std::int64_t kPaceSlackUs = 8000;

// Builds an MSF media timeline payload (draft-ietf-moq-msf-01 Section 7.1): a
// JSON array of records. A single timeline object carries one record here.
QByteArray timelinePayload(std::uint64_t mediaGroupId,
                           std::uint64_t mediaObjectId,
                           std::uint64_t mediaTimeUs,
                           std::uint64_t wallClockUnixUs) {
    return '[' + MsftsMuxer::mediaTimelineRecord(mediaTimeUs, mediaGroupId, mediaObjectId, wallClockUnixUs) + ']';
}

} // namespace

LivePipeline::LivePipeline(QObject* parent)
    : QObject(parent) {}

LivePipeline::~LivePipeline() {
    stop();
}

bool LivePipeline::running() const {
    return m_running.load(std::memory_order_acquire);
}

void LivePipeline::start(const PublishConfig& cfg, MoqxrPublisher* publisher) {
    if (m_running.load(std::memory_order_acquire)) {
        emit status(QStringLiteral("Pipeline already running."));
        return;
    }

    if (!publisher) {
        emit error(QStringLiteral("No publisher instance provided."));
        return;
    }

    m_config = cfg;
    m_publisher = publisher;
    m_running.store(true, std::memory_order_release);

    const bool hasCaptureSource = !cfg.cameraDeviceId.isEmpty() || !cfg.microphoneDeviceId.isEmpty();
    if (cfg.videoSource.isEmpty() && cfg.audioSource.isEmpty() && !hasCaptureSource) {
        emit error(QStringLiteral("An M2TS source or capture device is required."));
        m_running.store(false, std::memory_order_release);
        return;
    }
    if (!cfg.videoSource.isEmpty() && !cfg.audioSource.isEmpty() && cfg.videoSource != cfg.audioSource) {
        emit error(QStringLiteral("draft-gregoire-moq-msfts carries a single ordered TS/M2TS packet stream per track. Use one multiplexed M2TS source for this scaffold."));
        m_running.store(false, std::memory_order_release);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_doneMutex);
        m_workerDone = false;
    }
    m_workerThread = std::thread([this]() { runLoop(); });
    emit status(QStringLiteral("MSFTS M2TS packet pipeline started."));
}

void LivePipeline::stop() {
    const bool wasRunning = m_running.load(std::memory_order_acquire);
    requestStop();
    waitForStopped();

    if (wasRunning) {
        emit status(QStringLiteral("Streaming pipeline stopped."));
    }
}

void LivePipeline::requestStop() {
    m_running.store(false, std::memory_order_release);
}

void LivePipeline::waitForStopped() {
    if (!m_workerThread.joinable()) {
        return;
    }
    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(m_doneMutex);
        finished = m_doneCv.wait_for(lock, std::chrono::seconds(3),
                                     [this]() { return m_workerDone; });
    }
    if (finished) {
        m_workerThread.join();
    } else {
        std::fprintf(stderr, "[moqxr][stop] worker join timed out after 3000ms; detaching\n");
        std::fflush(stderr);
        m_workerThread.detach();
    }
}

void LivePipeline::runLoop() {
    struct DoneSignal {
        LivePipeline* self;
        ~DoneSignal() {
            {
                std::lock_guard<std::mutex> lock(self->m_doneMutex);
                self->m_workerDone = true;
            }
            self->m_doneCv.notify_all();
        }
    } doneSignal{this};

    if (!m_publisher) {
        emit error(QStringLiteral("Pipeline is missing publisher."));
        m_running.store(false, std::memory_order_release);
        return;
    }

    if ((m_config.videoSource.isEmpty() && m_config.audioSource.isEmpty()) &&
        (!m_config.cameraDeviceId.isEmpty() || !m_config.microphoneDeviceId.isEmpty())) {
        // Heap-allocated and shared with the lambda; see PublishState in LivePipeline.h.
        auto capturePtr = std::make_shared<LibavCaptureSource>(m_config);
        LibavCaptureSource& capture = *capturePtr;
        capture.setPreviewCallbacks(
            [this](const QImage& image) {
                emit previewVideoFrame(image);
            },
            [this](double left, double right) {
                emit previewAudioLevels(left, right);
            });
        QString captureError;
        if (!capture.open(&captureError)) {
            emit error(captureError);
            m_running.store(false, std::memory_order_release);
            return;
        }

        const int packetsPerObject = std::max(1, m_config.targetSegmentBytes / capture.packetSize());
        // Track name defaults to "program-1" when not supplied by other means.
        const QString trackName = QStringLiteral("program-1");
        const QString timelineTrackName = trackName + QStringLiteral(".timeline");
        const QByteArray catalog = MsftsMuxer::catalogJson({
            .track = trackName,
            .packetSize = capture.packetSize(),
            .packetsPerObject = packetsPerObject,
            .programNumber = capture.programNumber(),
            .pcrPid = capture.pcrPid(),
            .initData = capture.initData(),
            .timelineTrack = timelineTrackName,
            .namespaceName = m_config.namespaceName,
            .randomAccess = capture.randomAccessActive(),
            .isLive = true,
            .bitrateBps = static_cast<qint64>(m_config.videoTargetBitrateKbps) * 1000,
            .generatedAtMs = QDateTime::currentMSecsSinceEpoch(),
        });

        const int timelineEveryObjects = std::max(1, 1000 / std::max(1, m_config.fragmentDurationMs));

        auto st = std::make_shared<PublishState>();
        st->capture = capturePtr;   // keeps the source alive alongside the counters
        auto* capturePtrRaw = capturePtr.get();

        auto nextObject = [this, st, capturePtrRaw, packetsPerObject, trackName, timelineTrackName, timelineEveryObjects]() -> std::optional<PublishedObject> {
            if (!m_running.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            if (st->pendingTimeline.has_value()) {
                auto timeline = std::move(st->pendingTimeline);
                st->pendingTimeline.reset();
                return timeline;
            }

            M2tsObject object;
            QString readError;
            if (!(*capturePtrRaw).readObject(packetsPerObject, &object, m_running, &readError)) {
                if (!readError.isEmpty()) {
                    emit error(readError);
                }
                return std::nullopt;
            }

            const bool startsGroup = object.startsGroup;

            PublishedObject published;
            published.trackName = trackName;
            published.payload = std::move(object.payload);
            published.groupId = object.groupId;
            published.objectId = object.objectId;
            published.startsGroup = startsGroup;
            published.mediaTimeUs = object.mediaTimeUs;
            published.mediaDurationUs = static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
            if (startsGroup || (st->objects % timelineEveryObjects) == 0) {
                PublishedObject timeline;
                timeline.trackName = timelineTrackName;
                timeline.payload = timelinePayload(published.groupId,
                                                   published.objectId,
                                                   published.mediaTimeUs,
                                                   nowUnixUs());
                timeline.groupId = published.groupId;
                timeline.objectId = st->timelineObjectId++;
                timeline.mediaTimeUs = published.mediaTimeUs;
                timeline.mediaDurationUs = 0;
                st->pendingTimeline = std::move(timeline);
            }
            ++st->objects;
            st->bytes += published.payload.size();
            emit stats(st->objects, st->bytes, static_cast<int64_t>(published.groupId + 1));
            if (m_config.paceEgress) {
                if (st->pacingStartUs < 0) {
                    // Anchor the pace clock so the first object is due immediately
                    // (subtract its media time), avoiding a one-time startup wait
                    // equal to the encode/mux buffering offset. Subsequent st->objects
                    // pace relative to this anchor on the same steady clock.
                    st->pacingStartUs = nowSteadyUs() - static_cast<std::int64_t>(published.mediaTimeUs);
                }
                while (m_running.load(std::memory_order_acquire)) {
                    const std::int64_t delay = paceDelayUs(static_cast<std::int64_t>(published.mediaTimeUs),
                                                           nowSteadyUs() - st->pacingStartUs, kPaceSlackUs);
                    if (delay <= 0) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::int64_t>(delay, 5000)));
                }
            }
            return published;
        };

        if (!m_publisher->publishLiveObjects(m_config, trackName, QStringList{timelineTrackName}, catalog, std::move(nextObject), m_running)) {
            if (m_running.load(std::memory_order_acquire)) {
                emit error(QStringLiteral("Failed to publish captured MSFTS live object stream."));
            }
        }

        m_running.store(false, std::memory_order_release);
        emit status(QStringLiteral("Pipeline exiting."));
        return;
    }

    const QString sourcePath = !m_config.videoSource.isEmpty() ? m_config.videoSource : m_config.audioSource;
    // Heap-allocated and shared with the nextObject lambda below, NOT a stack local.
    // See the comment on PublishState in LivePipeline.h: the lambda outlives this
    // frame whenever waitForStopped() has to detach the worker, so anything it
    // touches has to outlive the frame too.
    auto packetizerPtr = std::make_shared<M2tsPacketizer>(sourcePath);
    M2tsPacketizer& packetizer = *packetizerPtr;
    // MSFTS carriage-profile knobs (msfts#7); must be set before open().
    packetizer.setTransparent(m_config.transparentMode);
    packetizer.setRetainSiTables(m_config.retainSiTables);
    packetizer.setRetainNullPackets(m_config.retainNullPackets);
    QString packetizerError;
    if (!packetizer.open(m_config.programNumber, &packetizerError)) {
        emit error(packetizerError);
        m_running.store(false, std::memory_order_release);
        return;
    }

    const int packetsPerObject = std::max(1, m_config.targetSegmentBytes / packetizer.packetSize());
    // Track name defaults to "program-1" when not supplied by other means.
    const QString trackName = QStringLiteral("program-1");
    const QString timelineTrackName = trackName + QStringLiteral(".timeline");
    // A non-seekable source (FIFO / stdin / pipe) is a live feed: advertise it as
    // live and skip VOD-only duration probing, which would open and consume the
    // pipe a second time.
    const bool liveStream = packetizer.sequential();
    const qint64 fileDurationMs = liveStream ? 0 : M2tsPacketizer::probeDurationMs(sourcePath);
    MsftsCatalog catalogSpec{
        .track = trackName,
        .packetSize = packetizer.packetSize(),
        .packetsPerObject = packetsPerObject,
        .programNumber = packetizer.programNumber(),
        .pcrPid = packetizer.pcrPid(),
        // Advertised as mpeg2tsSiPids. Empty unless --retain-si kept the DVB SI
        // PIDs alongside the selected program, which is exactly the case the
        // field describes: tables retained in the filtered track beyond the
        // PMT's list.
        .siPids = packetizer.retainedSiPids(),
        // For 192-octet source packets the timestamp prefix is carried without
        // specified semantics ("opaque", draft-gregoire-moq-msfts); omitted for
        // 188.
        .timestampMode = packetizer.packetSize() == 192 ? QStringLiteral("opaque") : QString(),
        .initData = packetizer.initData(),
        .timelineTrack = timelineTrackName,
        .namespaceName = m_config.namespaceName,
        .trackDurationMs = fileDurationMs,
        .randomAccess = true,
        // Transparent carriage forwards the source unchanged. The draft calls it
        // unmodified-program when the PAT lists one program, and forbids
        // unmodified-multiplex for such a source. A PAT that was not parsed
        // (count 0) cannot prove a single program, so it stays a multiplex.
        .mode = !m_config.transparentMode             ? Mpeg2tsMode::PerProgram
                : packetizer.patProgramCount() == 1   ? Mpeg2tsMode::UnmodifiedProgram
                                                      : Mpeg2tsMode::UnmodifiedMultiplex,
        .mpeg2tsMuxRateBps = m_config.mpeg2tsMuxRateBps,
        .isLive = false,
        .bitrateBps = static_cast<qint64>(m_config.videoTargetBitrateKbps) * 1000,
        // generatedAt is suppressed for VOD by catalogJson (isLive false).
    };
    if (liveStream) {
        // Live pipe feed: mark the track live (VOD duration does not apply).
        catalogSpec.isLive = true;
        // generatedAt is Optional in MSF 5.1.6 everywhere, so this is a choice rather
        // than a requirement: it is genuinely useful on a live feed for telling one
        // catalog instance from another, and every live example in both drafts carries
        // it. 5.1.6 only says it SHOULD NOT appear when isLive is false, which is why
        // it is set here rather than in the initializer above. The capture path
        // already sets it; this is the path production uses.
        catalogSpec.generatedAtMs = QDateTime::currentMSecsSinceEpoch();
    }
    if (m_config.transparentMode) {
        // Transparent mode now detects random_access_indicator in TS adaptation
        // fields and starts new groups at those points (MSFTS Section 6.3). Groups begin
        // at RAP when the source signals RAI; leave randomAccess true.
    }
    const QByteArray catalog = MsftsMuxer::catalogJson(catalogSpec);

    constexpr std::int64_t kPaceSlackUs = 10000; // 10 ms slack
    const int timelineEveryObjects = std::max(1, 1000 / std::max(1, m_config.fragmentDurationMs));

    // Shared with the lambda by VALUE, so nothing it touches lives on this frame.
    auto st = std::make_shared<PublishState>();
    st->packetizer = packetizerPtr;

    auto nextObject = [this, st, packetsPerObject, trackName, timelineTrackName, timelineEveryObjects, liveStream]() -> std::optional<PublishedObject> {
        if (!m_running.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        if (st->pendingTimeline.has_value()) {
            auto timeline = std::move(st->pendingTimeline);
            st->pendingTimeline.reset();
            return timeline;
        }

        M2tsObject object;
        QString readError;
        if (!(*st->packetizer).readObject(packetsPerObject, &object, &readError)) {
            if (!readError.isEmpty()) {
                emit error(readError);
            }
            return std::nullopt;
        }

        PublishedObject published;
        published.trackName = trackName;
        published.payload = std::move(object.payload);
        published.groupId = object.groupId;
        published.objectId = object.objectId;
        published.startsGroup = object.startsGroup;
        published.mediaTimeUs = static_cast<std::uint64_t>(st->objects) * static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
        published.mediaDurationUs = static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
        if ((st->objects % timelineEveryObjects) == 0) {
            PublishedObject timeline;
            timeline.trackName = timelineTrackName;
            // MSF 7.1.1: the wallclock time SHOULD be 0 for a VOD asset.
            timeline.payload = timelinePayload(published.groupId,
                                               published.objectId,
                                               published.mediaTimeUs,
                                               liveStream ? nowUnixUs() : 0);
            timeline.groupId = published.groupId;
            timeline.objectId = st->timelineObjectId++;
            timeline.mediaTimeUs = published.mediaTimeUs;
            timeline.mediaDurationUs = 0;
            st->pendingTimeline = std::move(timeline);
        }
        ++st->objects;
        st->bytes += published.payload.size();
        emit stats(st->objects, st->bytes, static_cast<int64_t>(published.groupId + 1));

        // Pace file-source publishing at media-time rate so data isn't dumped
        // at wire speed before subscribers can connect.
        if (m_config.pacedFileSource) {
            if (st->pacingStartUs < 0) {
                st->pacingStartUs = nowSteadyUs() - static_cast<std::int64_t>(published.mediaTimeUs);
            }
            while (m_running.load(std::memory_order_acquire)) {
                const std::int64_t delay = paceDelayUs(static_cast<std::int64_t>(published.mediaTimeUs),
                                                       nowSteadyUs() - st->pacingStartUs, kPaceSlackUs);
                if (delay <= 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::int64_t>(delay, 5000)));
            }
        }

        return published;
    };

    if (!m_publisher->publishLiveObjects(m_config, trackName, QStringList{timelineTrackName}, catalog, std::move(nextObject), m_running)) {
        if (m_running.load(std::memory_order_acquire)) {
            emit error(QStringLiteral("Failed to publish MSFTS live object stream."));
        }
    }

    m_running.store(false, std::memory_order_release);
    emit status(QStringLiteral("Pipeline exiting."));
}

} // namespace moq2ts
