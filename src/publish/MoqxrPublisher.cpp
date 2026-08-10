#include "MoqxrPublisher.h"

#include <QDebug>
#include <QMutexLocker>
#include <QThread>

#include <chrono>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef MOQ2TS_HAS_MOQXR
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/publisher_api.h"
#endif

namespace {

bool useMockTransport(const moq2ts::PublishConfig& cfg) {
    return cfg.moqEndpoint.trimmed().startsWith(QStringLiteral("mock://"), Qt::CaseInsensitive);
}

#ifdef MOQ2TS_HAS_MOQXR
openmoq::publisher::transport::EndpointConfig parseEndpoint(const QString& rawEndpoint) {
    using namespace openmoq::publisher::transport;

    EndpointConfig endpoint;
    std::string authority = rawEndpoint.trimmed().toStdString();
    const auto consumeScheme = [&](const char* prefix) {
        const std::string value(prefix);
        if (authority.rfind(value, 0) == 0) {
            authority = authority.substr(value.size());
            return true;
        }
        return false;
    };

    const bool hadMoqt = consumeScheme("moqt://");
    const bool hadHttps = consumeScheme("https://");
    const bool hadHttp = consumeScheme("http://");
    if (hadMoqt || hadHttps || hadHttp) {
        const std::size_t slash = authority.find('/');
        if (slash != std::string::npos) {
            endpoint.path = authority.substr(slash);
            endpoint.path_explicit = true;
            authority = authority.substr(0, slash);
        } else if (hadHttps || hadHttp) {
            endpoint.path.clear();
            endpoint.path.push_back('/');
            endpoint.path_explicit = true;
        }
        endpoint.transport = (hadHttps || hadHttp) ? TransportKind::kWebTransport : TransportKind::kRawQuic;
    }

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size()) {
        throw std::runtime_error("endpoint must be host:port, moqt://host:port/path, or https://host:port/path");
    }
    endpoint.host = authority.substr(0, colon);
    const int port = std::stoi(authority.substr(colon + 1));
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("endpoint port must be between 1 and 65535");
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    if (endpoint.transport == TransportKind::kWebTransport && !endpoint.path_explicit) {
        endpoint.path.clear();
        endpoint.path.push_back('/');
        endpoint.path.append("moq");
        endpoint.path_explicit = true;
    }
    return endpoint;
}

std::vector<std::uint8_t> toVector(const QByteArray& bytes) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    return std::vector<std::uint8_t>(begin, begin + bytes.size());
}

openmoq::publisher::DraftVersion defaultDraftVersion() {
    return openmoq::publisher::DraftVersion::kDraft16;
}

#endif

} // namespace

namespace moq2ts {

MoqxrPublisher::MoqxrPublisher(QObject* parent)
    : QObject(parent) {}

MoqxrPublisher::~MoqxrPublisher() {
    stop();
}

bool MoqxrPublisher::connect(const PublishConfig& cfg) {
    QMutexLocker locker(&m_mutex);
    m_endpoint = cfg.moqEndpoint;
    m_namespace = cfg.namespaceName;

#ifdef MOQ2TS_HAS_MOQXR
    if (useMockTransport(cfg)) {
        return connectMock(cfg);
    }

    m_connected = true;
    emit connectionStateChanged(true, QString("moqxr publisher configured for %1; namespace=%2").arg(m_endpoint, m_namespace));
    return true;
#else
    if (!useMockTransport(cfg)) {
        emit publishError("Mock publisher builds only accept mock:// endpoints. Set MOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF and rebuild to publish to a real relay.");
        return false;
    }
    return connectMock(cfg);
#endif
}

bool MoqxrPublisher::connectMock(const PublishConfig& cfg) {
    if (cfg.moqEndpoint.isEmpty() || cfg.namespaceName.isEmpty()) {
        emit publishError("Invalid MOQ endpoint/namespace configuration.");
        return false;
    }

    m_connected = true;
    emit connectionStateChanged(true, QString("Mock publisher connected to %1; namespace=%2").arg(m_endpoint, m_namespace));
    return true;
}

bool MoqxrPublisher::publishLiveObjects(const PublishConfig& cfg,
                                        const QString& mediaTrackName,
                                        const QStringList& sideTrackNames,
                                        const QByteArray& catalog,
                                        std::function<std::optional<PublishedObject>()> nextObject,
                                        std::atomic<bool>& running) {
#ifdef MOQ2TS_HAS_MOQXR
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected) {
            emit publishError("Cannot publish: no active moqxr session.");
            return false;
        }
    }

    if (useMockTransport(cfg)) {
        return publishLiveObjectsMock(catalog, std::move(nextObject), running, cfg.fragmentDurationMs);
    }

    try {
        openmoq::publisher::PublisherConfig publisherConfig;
        publisherConfig.draft_version = cfg.draftVersion == 14
            ? openmoq::publisher::DraftVersion::kDraft14
            : defaultDraftVersion();
        publisherConfig.track_namespace = cfg.namespaceName.toStdString();
        // Await-subscribe mode: only send data after the relay forwards a
        // SUBSCRIBE from a downstream subscriber. The moqxr library no longer
        // pre-announces tracks with PUBLISH messages, so the relay must forward
        // actual SUBSCRIBE messages - matching the moqxr SRT ingest flow.
        publisherConfig.forward = false;
        publisherConfig.publish_catalog = false;
        publisherConfig.paced = false;
        publisherConfig.loop = false;
        publisherConfig.subscriber_timeout = std::chrono::seconds(120);

        openmoq::publisher::LiveObjectSource source;
        source.tracks.push_back({.track_name = "catalog"});
        source.tracks.push_back({.track_name = mediaTrackName.toStdString()});
        for (const QString& trackName : sideTrackNames) {
            source.tracks.push_back({.track_name = trackName.toStdString()});
        }

        bool catalogSent = false;
        int64_t publisherObjects = 0;
        int64_t publisherBytes = 0;
        // Look-ahead queue. final_in_subgroup must mark the last object of a group
        // ON ITS OWN TRACK, and next_object() interleaves the media, catalog and
        // timeline tracks, so the next object overall is often a different track.
        // A single-slot look-ahead cannot answer that: we peek forward until an
        // object on the same track appears, holding the skipped ones in order so
        // the wire sequence is unchanged. In practice that is one or two objects,
        // because the timeline is interleaved at most once per media object.
        std::deque<PublishedObject> lookahead;
        source.next_object = [this,
                              catalogSent,
                              publisherObjects,
                              publisherBytes,
                              catalog,
                              lookahead,
                              mediaTrack = mediaTrackName,
                              nextObject = std::move(nextObject)]() mutable -> std::optional<openmoq::publisher::LiveObject> {
            if (!catalogSent) {
                catalogSent = true;
                ++publisherObjects;
                publisherBytes += catalog.size();
                emit framePublished(QStringLiteral("catalog"), publisherBytes, publisherObjects);
                return openmoq::publisher::LiveObject{
                    .track_name = "catalog",
                    .group_id = 0,
                    .subgroup_id = 0,
                    .object_id = 0,
                    .media_time_us = 0,
                    .media_duration_us = 0,
                    .payload = toVector(catalog),
                    .subgroup_contains_group_largest = true,
                    .final_in_subgroup = true,
                };
            }

            if (lookahead.empty()) {
                std::optional<PublishedObject> first = nextObject();
                if (!first.has_value()) {
                    return std::nullopt;
                }
                lookahead.push_back(std::move(*first));
            }

            PublishedObject current = std::move(lookahead.front());
            lookahead.pop_front();

            // Find the next object on the SAME track, pulling more in as needed.
            // With none (end of stream) current is the last on its track, so it
            // closes its group.
            bool isFinalInGroup = true;
            std::size_t scan = 0;
            for (;;) {
                if (scan < lookahead.size()) {
                    if (lookahead[scan].trackName == current.trackName) {
                        isFinalInGroup = lookahead[scan].groupId != current.groupId;
                        break;
                    }
                    ++scan;
                    continue;
                }
                std::optional<PublishedObject> more = nextObject();
                if (!more.has_value()) {
                    break;
                }
                lookahead.push_back(std::move(*more));
            }
            ++publisherObjects;
            publisherBytes += current.payload.size();
            emit framePublished(current.trackName, publisherBytes, publisherObjects);

            return openmoq::publisher::LiveObject{
                .track_name = current.trackName.toStdString(),
                .group_id = static_cast<std::size_t>(current.groupId),
                .subgroup_id = current.subgroupId,
                .object_id = static_cast<std::size_t>(current.objectId),
                .media_time_us = current.mediaTimeUs,
                .media_duration_us = current.mediaDurationUs,
                .payload = toVector(current.payload),
                .subgroup_contains_group_largest = isFinalInGroup,
                .final_in_subgroup = isFinalInGroup,
            };
        };

        auto activePublisher = std::make_shared<openmoq::publisher::Publisher>(publisherConfig);
        {
            QMutexLocker locker(&m_mutex);
            m_activePublisher = activePublisher;
        }

        openmoq::publisher::transport::TlsConfig tls;
        tls.insecure_skip_verify = true;
        const auto status = activePublisher->publish_live_objects(source, parseEndpoint(cfg.moqEndpoint), tls, false);
        if (!status.ok && running.load(std::memory_order_acquire)) {
            emit publishError(QString::fromStdString(status.message));
            QMutexLocker locker(&m_mutex);
            if (m_activePublisher == activePublisher) {
                m_activePublisher.reset();
            }
            return false;
        }
        const auto disconnectStatus = activePublisher->disconnect(0);
        if (!disconnectStatus.ok) {
            emit publishError(QString::fromStdString(disconnectStatus.message));
            QMutexLocker locker(&m_mutex);
            if (m_activePublisher == activePublisher) {
                m_activePublisher.reset();
            }
            return false;
        }

        const auto stats = activePublisher->stats();
        {
            QMutexLocker locker(&m_mutex);
            if (m_activePublisher == activePublisher) {
                m_activePublisher.reset();
            }
        }
        emit framePublished(mediaTrackName, static_cast<int64_t>(stats.bytes_published), static_cast<int64_t>(stats.objects_published));
        return status.ok;
    } catch (const std::exception& error) {
        QMutexLocker locker(&m_mutex);
        m_activePublisher.reset();
        emit publishError(QString::fromStdString(error.what()));
        return false;
    }
#else
    Q_UNUSED(cfg);
    Q_UNUSED(mediaTrackName);
    Q_UNUSED(sideTrackNames);
    return publishLiveObjectsMock(catalog, std::move(nextObject), running, cfg.fragmentDurationMs);
#endif
}

bool MoqxrPublisher::publishLiveObjectsMock(const QByteArray& catalog,
                                            std::function<std::optional<PublishedObject>()> nextObject,
                                            std::atomic<bool>& running,
                                            int pacingMs) {
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected) {
            emit publishError("Cannot publish: mock publisher not connected.");
            return false;
        }
    }

    qDebug() << "MOCK MSFTS catalog bytes=" << catalog.size();
    int64_t publisherObjects = 1;
    int64_t publisherBytes = catalog.size();
    emit framePublished(QStringLiteral("catalog"), publisherBytes, publisherObjects);

    while (running.load(std::memory_order_acquire)) {
        std::optional<PublishedObject> object = nextObject();
        if (!object.has_value()) {
            return true;
        }
        qDebug() << "MOCK MSFTS publish" << object->trackName << "group=" << object->groupId
                 << "object=" << object->objectId << "bytes=" << object->payload.size();
        ++publisherObjects;
        publisherBytes += object->payload.size();
        emit framePublished(object->trackName, publisherBytes, publisherObjects);
        if (pacingMs > 0) {
            QThread::msleep(static_cast<unsigned long>(pacingMs));
        }
    }
    return true;
}


void MoqxrPublisher::stop() {
#ifdef MOQ2TS_HAS_MOQXR
    std::shared_ptr<openmoq::publisher::Publisher> publisherToStop;
#endif
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected) {
            return;
        }
        m_connected = false;
#ifdef MOQ2TS_HAS_MOQXR
        publisherToStop = m_activePublisher;
#endif
    }
#ifdef MOQ2TS_HAS_MOQXR
    if (publisherToStop) {
        const auto disconnectStatus = publisherToStop->disconnect(0);
        if (!disconnectStatus.ok) {
            emit publishError(QString::fromStdString(disconnectStatus.message));
        }
    }
#endif
    emit connectionStateChanged(false, "Publisher stopped");
}

bool MoqxrPublisher::connected() const {
    QMutexLocker locker(&m_mutex);
    return m_connected;
}

} // namespace moq2ts
