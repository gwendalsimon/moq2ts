#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace moq2ts {

struct MsftsCatalog {
    QString track;
    int packetSize = 188;
    // Encoder-only knob (source packets per Object); not a catalog field.
    int packetsPerObject = 7;
    int programNumber = 1;
    int pcrPid = -1;
    // draft-gregoire-moq-msfts mpeg2tsSiPids: SI table PIDs retained in the
    // filtered track in addition to those listed in the PMT. Advisory; emitted
    // only when non-empty, which is only when SI retention is on and the track
    // is filtered.
    QList<int> siPids;
    // Per draft-gregoire-moq-msfts, mpeg2tsTimestampMode applies only to
    // 192-octet source packets ("arrival-time" or "opaque") and MUST NOT be
    // present for 188.
    QString timestampMode;
    QByteArray initData;
    QString timelineTrack;

    // Root "version" identifies the referenced MSF revision, not a version of the
    // MSFTS packaging format (draft-gregoire-moq-msfts). Every catalog example
    // in that draft carries it as the string "draft-01".
    QString msfVersion = QStringLiteral("draft-01");
    // MSF track namespace (per-track, MSF 5.1.10). Emitted only when non-empty.
    QString namespaceName;
    // VOD-only track duration in integer milliseconds (MSF 5.1.37); emitted only
    // when isLive is false and the value is > 0.
    qint64 trackDurationMs = 0;
    // When true, advertise mpeg2tsRandomAccess (draft-gregoire-moq-msfts): every
    // MOQT group begins at a random-access point.
    bool randomAccess = false;

    // Whole-multiplex (transparent) profile: when true the track carries the
    // full multiplex verbatim. Advertised as mpeg2tsMode "unmodified-multiplex"
    // (draft-gregoire-moq-msfts); when false, advertised as "per-program" and
    // the per-program fields (mpeg2tsProgramNumber/mpeg2tsPcrPid) are included.
    bool wholeMultiplex = false;
    // Advisory source constant mux rate in bits/s. Emitted as mpeg2tsMuxRate
    // only when > 0 and the track is not whole-multiplex.
    qint64 mpeg2tsMuxRateBps = 0;

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

    // Inverse of catalogJson: parse an MSFTS catalog document and fill the
    // mpeg2ts media-track fields a receiver needs (packetSize, wholeMultiplex,
    // program/pcr, muxRate, randomAccess, timestampMode, isLive, decoded
    // initData). The media track's name is returned via mediaTrackName. Returns
    // false (and sets error) when the document is invalid or has no mpeg2ts
    // track.
    static bool catalogFromJson(const QByteArray& json,
                                MsftsCatalog* out,
                                QString* mediaTrackName,
                                QString* error);
};

} // namespace moq2ts
