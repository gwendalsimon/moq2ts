#include "media/MsftsMuxer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <string>

using moq2ts::MsftsCatalog;
using moq2ts::MsftsMuxer;
using moq2ts::Mpeg2tsMode;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}

MsftsCatalog baseCatalog(Mpeg2tsMode mode) {
    MsftsCatalog catalog;
    catalog.track = QStringLiteral("program-1");
    catalog.mode = mode;
    catalog.programNumber = 1;
    catalog.pcrPid = 256;
    catalog.mpeg2tsMuxRateBps = 8000000;
    catalog.siPids = {0x11};
    catalog.initData = QByteArray(188, char(0x47));
    catalog.timelineTrack = QStringLiteral("program-1.timeline");
    return catalog;
}

QJsonObject mediaTrack(const QByteArray& json) {
    return QJsonDocument::fromJson(json).object().value(QStringLiteral("tracks")).toArray().at(0).toObject();
}
}  // namespace

int main() {
    bool ok = true;

    // unmodified-program describes its one program; SI PIDs are per-program only.
    {
        const QJsonObject track = mediaTrack(MsftsMuxer::catalogJson(baseCatalog(Mpeg2tsMode::UnmodifiedProgram)));
        ok &= expect(track.value("mpeg2tsMode").toString() == "unmodified-program", "unmodified-program: mode");
        ok &= expect(track.value("mpeg2tsProgramNumber").toInt() == 1, "unmodified-program: program number");
        ok &= expect(track.value("mpeg2tsPcrPid").toInt() == 256, "unmodified-program: PCR PID");
        ok &= expect(static_cast<qint64>(track.value("mpeg2tsMuxRate").toDouble()) == 8000000, "unmodified-program: mux rate");
        ok &= expect(!track.contains("mpeg2tsSiPids"), "unmodified-program: no SI PIDs");
    }

    // unmodified-multiplex selects no program: every program field is absent.
    {
        const QJsonObject track = mediaTrack(MsftsMuxer::catalogJson(baseCatalog(Mpeg2tsMode::UnmodifiedMultiplex)));
        ok &= expect(track.value("mpeg2tsMode").toString() == "unmodified-multiplex", "multiplex: mode");
        for (const char* field : {"mpeg2tsProgramNumber", "mpeg2tsPcrPid", "mpeg2tsMuxRate", "mpeg2tsSiPids"}) {
            ok &= expect(!track.contains(QLatin1String(field)), std::string("multiplex: no ") + field);
        }
    }

    // per-program keeps every program field.
    {
        const QJsonObject track = mediaTrack(MsftsMuxer::catalogJson(baseCatalog(Mpeg2tsMode::PerProgram)));
        ok &= expect(track.value("mpeg2tsMode").toString() == "per-program", "per-program: mode");
        ok &= expect(track.value("mpeg2tsProgramNumber").toInt() == 1, "per-program: program number");
        ok &= expect(track.value("mpeg2tsSiPids").toArray().size() == 1, "per-program: SI PIDs");
    }

    // initDataList follows the tracks array (MSF 5.1.7), and the document parses.
    {
        MsftsCatalog catalog = baseCatalog(Mpeg2tsMode::PerProgram);
        catalog.generatedAtMs = 1;
        const QByteArray json = MsftsMuxer::catalogJson(catalog);
        const qsizetype tracksAt = json.indexOf("\"tracks\"");
        const qsizetype listAt = json.indexOf("\"initDataList\"");
        ok &= expect(tracksAt >= 0 && listAt > tracksAt, "initDataList after tracks");
        MsftsCatalog parsed;
        ok &= expect(MsftsMuxer::catalogFromJson(json, &parsed, nullptr, nullptr) &&
                         parsed.initData == catalog.initData,
                     "initData resolves through initRef");
    }

    // The media timeline track is identified by packaging (MSF Table 3).
    {
        const QJsonArray tracks = QJsonDocument::fromJson(MsftsMuxer::catalogJson(baseCatalog(Mpeg2tsMode::PerProgram)))
                                      .object().value(QStringLiteral("tracks")).toArray();
        const QJsonObject timeline = tracks.at(1).toObject();
        ok &= expect(timeline.value("packaging").toString() == "mediatimeline", "timeline: packaging");
        ok &= expect(!timeline.contains("type"), "timeline: no type field");
        ok &= expect(timeline.value("depends").toArray().at(0).toString() == "program-1", "timeline: depends");
    }

    // The parser reads each TS-packet mode back, and skips es-packets.
    for (const Mpeg2tsMode mode : {Mpeg2tsMode::UnmodifiedProgram, Mpeg2tsMode::UnmodifiedMultiplex,
                                   Mpeg2tsMode::PerProgram}) {
        MsftsCatalog parsed;
        QString error;
        ok &= expect(MsftsMuxer::catalogFromJson(MsftsMuxer::catalogJson(baseCatalog(mode)), &parsed, nullptr, &error),
                     "round trip parses: " + error.toStdString());
        ok &= expect(parsed.mode == mode, "round trip keeps the mode");
    }
    {
        MsftsCatalog parsed;
        QString error;
        const bool accepted = MsftsMuxer::catalogFromJson(MsftsMuxer::catalogJson(baseCatalog(Mpeg2tsMode::EsPackets)),
                                                          &parsed, nullptr, &error);
        ok &= expect(!accepted && error.contains("es-packets"), "es-packets track is skipped");
    }

    // Legacy boolean keys still map to a mode.
    {
        MsftsCatalog parsed;
        const QByteArray legacy = R"({"version":"draft-01","tracks":[{"name":"t","packaging":"mpeg2ts","m2tsMpts":true}]})";
        ok &= expect(MsftsMuxer::catalogFromJson(legacy, &parsed, nullptr, nullptr) &&
                         parsed.mode == Mpeg2tsMode::UnmodifiedMultiplex,
                     "legacy m2tsMpts maps to unmodified-multiplex");
    }

    if (ok) {
        std::cout << "msfts catalog tests passed\n";
    }
    return ok ? 0 : 1;
}
