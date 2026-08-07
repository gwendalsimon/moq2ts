#include "MsftsMuxer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>

namespace moq2ts {

QByteArray MsftsMuxer::catalogJson(const MsftsCatalog& catalog) {
    QJsonObject mediaTrack;
    mediaTrack.insert(QStringLiteral("name"), catalog.track);
    if (!catalog.namespaceName.isEmpty()) {
        mediaTrack.insert(QStringLiteral("namespace"), catalog.namespaceName);
    }
    mediaTrack.insert(QStringLiteral("packaging"), QStringLiteral("m2ts"));
    // MSF common track fields (draft-ietf-moq-msf-00).
    mediaTrack.insert(QStringLiteral("isLive"), catalog.isLive);
    mediaTrack.insert(QStringLiteral("role"), catalog.role);
    mediaTrack.insert(QStringLiteral("mimeType"), catalog.mimeType);
    if (catalog.isLive) {
        // targetLatency MUST NOT be present when isLive is false (MSF 5.1.16).
        mediaTrack.insert(QStringLiteral("targetLatency"), catalog.targetLatencyMs);
    }
    if (catalog.bitrateBps > 0) {
        mediaTrack.insert(QStringLiteral("bitrate"), catalog.bitrateBps);
    }
    // MSFTS m2ts-specific fields (draft-gregoire-moq-msfts-00 Section 6).
    mediaTrack.insert(QStringLiteral("m2tsPacketSize"), catalog.packetSize);
    mediaTrack.insert(QStringLiteral("m2tsPacketsPerObject"), catalog.packetsPerObject);
    if (catalog.m2tsMpts) {
        // Whole-multiplex profile (draft-gregoire-moq-msfts m2tsMpts): the track
        // carries every program verbatim, so the per-program identifiers do not
        // apply and MUST be omitted.
        mediaTrack.insert(QStringLiteral("m2tsMpts"), true);
    } else {
        mediaTrack.insert(QStringLiteral("m2tsProgramNumber"), catalog.programNumber);
        if (catalog.pmtPid >= 0) {
            mediaTrack.insert(QStringLiteral("m2tsPmtPid"), catalog.pmtPid);
        }
        if (catalog.pcrPid >= 0) {
            mediaTrack.insert(QStringLiteral("m2tsPcrPid"), catalog.pcrPid);
        }
    }
    // Advisory source constant mux rate (msfts#7 suggestion 4); applies to both
    // profiles and is emitted only when supplied.
    if (catalog.m2tsMuxRateBps > 0) {
        mediaTrack.insert(QStringLiteral("m2tsMuxRate"), catalog.m2tsMuxRateBps);
    }
    // MSFTS m2tsSiPids: PIDs of SI tables retained in the filtered track beyond
    // those the PMT lists. Advisory. The list is only populated when SI retention
    // is on, and it stays empty for the whole-multiplex profile because nothing is
    // filtered out there, so a plain non-empty test is the right gate.
    if (!catalog.siPids.isEmpty()) {
        QJsonArray siPids;
        for (int pid : catalog.siPids) {
            siPids.append(pid);
        }
        mediaTrack.insert(QStringLiteral("m2tsSiPids"), siPids);
    }
    // m2tsTimestampMode is only valid for 192-octet source packets (MSFTS 6.9);
    // it MUST NOT be present for 188.
    if (catalog.packetSize == 192 && !catalog.timestampMode.isEmpty()) {
        mediaTrack.insert(QStringLiteral("m2tsTimestampMode"), catalog.timestampMode);
    }
    // MSFTS 6.8: only advertised when every group begins at a random-access point.
    if (catalog.randomAccess) {
        mediaTrack.insert(QStringLiteral("m2tsRandomAccess"), true);
    }
    // MSF 5.1.37: track duration is VOD-only (MUST NOT appear when isLive true).
    if (!catalog.isLive && catalog.trackDurationMs > 0) {
        mediaTrack.insert(QStringLiteral("trackDuration"), catalog.trackDurationMs);
    }
    // Initialization data is referenced, not inlined on the track. MSF-01 replaced
    // the old track-level initData field (MSF-00 5.1.20) with initRef (5.2.13)
    // pointing into a root initDataList (5.1.7), and MSFTS 6.14 requires the
    // referenced entry's type to be "inline". Emitting the MSF-00 shape would be
    // silently dropped by a conformant receiver, since MSFTS 6.1 tells parsers to
    // ignore fields they do not understand, leaving a filtered track with no PSI
    // bootstrap at all.
    const QString initRefId = QStringLiteral("init-") + catalog.track;
    if (!catalog.initData.isEmpty()) {
        mediaTrack.insert(QStringLiteral("initRef"), initRefId);
    }

    QJsonArray tracks;
    tracks.append(mediaTrack);

    if (!catalog.timelineTrack.isEmpty()) {
        // MSF media timeline track (draft-ietf-moq-msf-00 Section 7.2): identified
        // by type "mediatimeline", a "depends" list of the track names it applies
        // to, and an application/json mime type.
        QJsonObject timelineTrack;
        timelineTrack.insert(QStringLiteral("name"), catalog.timelineTrack);
        if (!catalog.namespaceName.isEmpty()) {
            timelineTrack.insert(QStringLiteral("namespace"), catalog.namespaceName);
        }
        timelineTrack.insert(QStringLiteral("type"), QStringLiteral("mediatimeline"));
        QJsonArray depends;
        depends.append(catalog.track);
        timelineTrack.insert(QStringLiteral("depends"), depends);
        timelineTrack.insert(QStringLiteral("mimeType"), QStringLiteral("application/json"));

        tracks.append(timelineTrack);
    }

    QJsonObject root;
    // MSF-01 5.1.1 makes version a String and says to write "draft-XX" against
    // Internet-Draft releases; MSFTS 2 requires the value specified by the MSF
    // revision it references, which is draft-ietf-moq-msf-01. No "format" field is
    // emitted: it exists in neither document.
    root.insert(QStringLiteral("version"), catalog.msfVersion);
    if (catalog.isLive && catalog.generatedAtMs > 0) {
        // SHOULD NOT be included when isLive is false (MSF 5.1.6).
        root.insert(QStringLiteral("generatedAt"), catalog.generatedAtMs);
    }
    if (!catalog.initData.isEmpty()) {
        // MSF 5.1.7: each entry is {id, type, data}; MSFTS 6.14 fixes type to
        // "inline" and requires the decoded data to be whole source packets, which
        // it is because collectInitData() only ever captures complete TS packets.
        QJsonObject initEntry;
        initEntry.insert(QStringLiteral("id"), initRefId);
        initEntry.insert(QStringLiteral("type"), QStringLiteral("inline"));
        initEntry.insert(QStringLiteral("data"), QString::fromLatin1(catalog.initData.toBase64()));
        QJsonArray initDataList;
        initDataList.append(initEntry);
        root.insert(QStringLiteral("initDataList"), initDataList);
    }
    root.insert(QStringLiteral("tracks"), tracks);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool MsftsMuxer::catalogFromJson(const QByteArray& json,
                                 MsftsCatalog* out,
                                 QString* mediaTrackName,
                                 QString* error) {
    const auto fail = [&](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (out == nullptr) {
        return fail(QStringLiteral("catalogFromJson: null output."));
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        return fail(QStringLiteral("Catalog is not a JSON object: %1").arg(parseError.errorString()));
    }
    const QJsonObject root = doc.object();
    // "version" names the referenced MSF revision. The MSFTS draft writes it as a
    // string ("draft-01"); older catalogs of ours wrote the integer 1. Accept both
    // so a publisher upgrade does not strand receivers mid-rollout.
    const QJsonValue versionValue = root.value(QStringLiteral("version"));
    QString msfVersion;
    if (versionValue.isString()) {
        msfVersion = versionValue.toString();
    } else if (versionValue.isDouble()) {
        msfVersion = QString::number(versionValue.toInt());
    }
    // MSF 5.1.7 initDataList, keyed by id so a track's initRef (5.2.13) can be
    // resolved. Only "inline" entries are usable: MSFTS 6.14 defines no other type,
    // and an unknown type carries data this code cannot interpret.
    QHash<QString, QByteArray> initDataById;
    for (const QJsonValue& entryValue : root.value(QStringLiteral("initDataList")).toArray()) {
        const QJsonObject entry = entryValue.toObject();
        if (entry.value(QStringLiteral("type")).toString() != QStringLiteral("inline")) {
            continue;
        }
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            continue;
        }
        initDataById.insert(id, QByteArray::fromBase64(
            entry.value(QStringLiteral("data")).toString().toLatin1()));
    }

    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    if (tracks.isEmpty()) {
        return fail(QStringLiteral("Catalog has no tracks."));
    }

    for (const QJsonValue value : tracks) {
        const QJsonObject track = value.toObject();
        if (track.value(QStringLiteral("packaging")).toString() != QStringLiteral("m2ts")) {
            continue;
        }

        MsftsCatalog parsed;
        parsed.track = track.value(QStringLiteral("name")).toString();
        parsed.namespaceName = track.value(QStringLiteral("namespace")).toString();
        parsed.packetSize = track.value(QStringLiteral("m2tsPacketSize")).toInt(188);
        parsed.packetsPerObject = track.value(QStringLiteral("m2tsPacketsPerObject")).toInt(0);
        // The draft names this field m2tsMpts; accept the legacy m2tsTransparent
        // spelling as a fallback so catalogs from older publishers still parse.
        parsed.m2tsMpts = track.value(QStringLiteral("m2tsMpts"))
                              .toBool(track.value(QStringLiteral("m2tsTransparent")).toBool(false));
        parsed.programNumber = track.value(QStringLiteral("m2tsProgramNumber")).toInt(0);
        parsed.pmtPid = track.contains(QStringLiteral("m2tsPmtPid"))
                            ? track.value(QStringLiteral("m2tsPmtPid")).toInt(-1)
                            : -1;
        parsed.pcrPid = track.contains(QStringLiteral("m2tsPcrPid"))
                            ? track.value(QStringLiteral("m2tsPcrPid")).toInt(-1)
                            : -1;
        parsed.m2tsMuxRateBps = static_cast<qint64>(
            track.value(QStringLiteral("m2tsMuxRate")).toDouble(0.0));
        parsed.siPids.clear();
        for (const QJsonValue& pid : track.value(QStringLiteral("m2tsSiPids")).toArray()) {
            parsed.siPids.append(pid.toInt());
        }
        parsed.timestampMode = track.value(QStringLiteral("m2tsTimestampMode")).toString();
        parsed.randomAccess = track.value(QStringLiteral("m2tsRandomAccess")).toBool(false);
        parsed.isLive = track.value(QStringLiteral("isLive")).toBool(true);
        // Prefer the MSF-01 shape. The inline initData spelling is MSF-00 and is
        // accepted as a fallback so catalogs from older publishers still resolve.
        const QString initRef = track.value(QStringLiteral("initRef")).toString();
        const QString initDataB64 = track.value(QStringLiteral("initData")).toString();
        if (!initRef.isEmpty() && initDataById.contains(initRef)) {
            parsed.initData = initDataById.value(initRef);
        } else if (!initDataB64.isEmpty()) {
            parsed.initData = QByteArray::fromBase64(initDataB64.toLatin1());
        }

        if (parsed.packetSize != 188 && parsed.packetSize != 192) {
            return fail(QStringLiteral("Unsupported m2tsPacketSize %1 (expected 188 or 192).")
                            .arg(parsed.packetSize));
        }

        if (!msfVersion.isEmpty()) {
            parsed.msfVersion = msfVersion;
        }
        *out = parsed;
        if (mediaTrackName) {
            *mediaTrackName = parsed.track;
        }
        return true;
    }

    return fail(QStringLiteral("Catalog has no m2ts-packaged track."));
}

} // namespace moq2ts
