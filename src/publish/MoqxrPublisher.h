#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "../app/PublishConfig.h"

#ifdef MOQ2TS_HAS_MOQXR
namespace openmoq::publisher {
class Publisher;
}
#endif

namespace moq2ts {

// The Track Name of the catalog track: MSF -01 Section 5 makes it "catalog".
inline constexpr char kCatalogTrackName[] = "catalog";

struct PublishedObject {
    QString trackName;
    QByteArray payload;
    std::uint64_t groupId = 0;
    std::uint64_t objectId = 0;
    std::uint64_t mediaTimeUs = 0;
    std::uint64_t mediaDurationUs = 0;
    // True when this object contains a random-access point and therefore opens a
    // new group. Carried explicitly rather than derived from objectId == 0: the
    // very first object of a stream has objectId 0 without any RAP
    // (M2tsPacketizer.cpp:465 only bumps the group once sawFirstRap), so deriving
    // it would mislabel exactly one object per run.
    bool startsGroup = false;
    // True when no later object shares this group (a timeline or catalog
    // object), so the publisher closes the group without looking ahead.
    bool finalInGroup = false;
    // A marker, never sent: the track named trackName has no more Objects, so
    // its last Object closes its Group (see ObjectLookahead).
    bool endsTrack = false;
    // The track this Object describes, which must go out first: a timeline
    // Object records a media Object (MSF -01 Section 7: data about previously
    // published Groups). Empty when it depends on none.
    QString afterTrack;
};

class IMoqOutput {
public:
    virtual ~IMoqOutput() = default;
    virtual bool connect(const PublishConfig& cfg) = 0;
    virtual bool publishLiveObjects(const PublishConfig& cfg,
                                    const QString& mediaTrackName,
                                    const QStringList& sideTrackNames,
                                    const QByteArray& catalog,
                                    std::function<std::optional<PublishedObject>()> nextObject,
                                    std::atomic<bool>& running) = 0;
    virtual void stop() = 0;
    virtual bool connected() const = 0;
};

class MoqxrPublisher final : public QObject, public IMoqOutput {
    Q_OBJECT
public:
    explicit MoqxrPublisher(QObject* parent = nullptr);
    ~MoqxrPublisher() override;

    bool connect(const PublishConfig& cfg) override;
    bool publishLiveObjects(const PublishConfig& cfg,
                            const QString& mediaTrackName,
                            const QStringList& sideTrackNames,
                            const QByteArray& catalog,
                            std::function<std::optional<PublishedObject>()> nextObject,
                            std::atomic<bool>& running) override;
    void stop() override;
    bool connected() const override;

    // The MOQT drafts that --draft accepts with the pinned moqxr SDK.
    // A relay accepts only the drafts in its config, so an
    // unsupported value is refused rather than replaced by another draft.
    static bool supportsDraft(int draft);
    static QString supportedDrafts();

signals:
    void connectionStateChanged(bool connected, const QString& message);
    void framePublished(const QString& track, int64_t bytes, int64_t objects);
    void publishError(const QString& error);

private:
    bool connectMock(const PublishConfig& cfg);
    bool publishLiveObjectsMock(const QByteArray& catalog,
                                std::function<std::optional<PublishedObject>()> nextObject,
                                std::atomic<bool>& running,
                                int pacingMs);

    bool m_connected = false;
    QString m_namespace;
    QString m_endpoint;
    mutable QMutex m_mutex;
#ifdef MOQ2TS_HAS_MOQXR
    std::shared_ptr<openmoq::publisher::Publisher> m_activePublisher;
#endif
};

} // namespace moq2ts
