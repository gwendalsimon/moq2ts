#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace moq2ts {

struct MsftsCatalog {
    QString track;
    int packetSize = 188;
    int packetsPerObject = 7;
    int programNumber = 1;
    int pmtPid = -1;
    int pcrPid = -1;
    // MSFTS 6.x m2tsSiPids: SI table PIDs retained in the filtered track in
    // addition to those listed in the PMT. Advisory; emitted only when non-empty,
    // which is only when SI retention is on and the track is filtered.
    QList<int> siPids;
    // Per MSFTS Section 6.9, m2tsTimestampMode applies only to 192-octet source
    // packets ("arrival-time" or "opaque") and MUST NOT be present for 188.
    QString timestampMode;
    QByteArray initData;
    QString timelineTrack;

    // Root "version" identifies the referenced MSF revision, not a version of the
    // MSFTS packaging format (draft-gregoire-moq-msfts 2.4). Every catalog example
    // in that draft carries it as the string "draft-01".
    QString msfVersion = QStringLiteral("draft-01");
    // MSF track namespace (per-track, MSF 5.1.10). Emitted only when non-empty.
    QString namespaceName;
    // VOD-only track duration in integer milliseconds (MSF 5.1.37); emitted only
    // when isLive is false and the value is > 0.
    qint64 trackDurationMs = 0;
    // When true, advertise m2tsRandomAccess (MSFTS 6.8): every MOQT group begins
    // at a random-access point.
    bool randomAccess = false;

    // Whole-multiplex (transparent) profile: when true the track carries the full
    // multiplex verbatim. Advertised as m2tsMpts (draft-gregoire-moq-msfts) and the
    // per-program fields (m2tsProgramNumber/m2tsPmtPid/m2tsPcrPid) are omitted.
    bool m2tsMpts = false;
    // Advisory source constant mux rate in bits/s (msfts#7 suggestion 4). Emitted
    // as m2tsMuxRate only when > 0.
    qint64 m2tsMuxRateBps = 0;

    // MSF common track/root fields (draft-ietf-moq-msf-00).
    bool isLive = true;
    int targetLatencyMs = 1000;
    QString role = QStringLiteral("video");
    QString mimeType = QStringLiteral("video/mp2t");
    qint64 bitrateBps = 0;
    qint64 generatedAtMs = 0;
};

class MsftsMuxer {
public:
    static QByteArray catalogJson(const MsftsCatalog& catalog);

    // Inverse of catalogJson: parse an MSFTS catalog document and fill the m2ts
    // media-track fields a receiver needs (packetSize, transparent, program/pmt/
    // pcr, muxRate, randomAccess, timestampMode, isLive, decoded initData). The
    // media track's name is returned via mediaTrackName. Returns false (and sets
    // error) when the document is invalid or has no m2ts track.
    static bool catalogFromJson(const QByteArray& json,
                                MsftsCatalog* out,
                                QString* mediaTrackName,
                                QString* error);
};

} // namespace moq2ts
