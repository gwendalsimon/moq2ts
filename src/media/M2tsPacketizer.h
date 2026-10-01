#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>

#include <cstdint>
#include <optional>
#include <set>
#include <utility>

namespace moq2ts {

struct M2tsObject {
    QByteArray payload;
    std::uint64_t groupId = 0;
    std::uint64_t objectId = 0;
    // True when this object is the first of a new MOQT group, which means it
    // contains a random access point.
    bool startsGroup = false;
    // Media time (microseconds, capture-epoch relative) of the most recent video
    // frame whose bytes are in this object. 0 for the file-source path.
    std::uint64_t mediaTimeUs = 0;
    // PTS of the first video PES packet that starts in this object, in
    // microseconds, unwrapped past the 33-bit range. Empty when no video PES
    // starts in it.
    std::optional<std::uint64_t> ptsUs;
};

// PTS of a PES packet that starts in a 188-octet TS packet, in 90 kHz units.
// Returns -1 when the packet starts no PES packet or the header carries no PTS.
std::int64_t pesPts(const QByteArray& tsPacket);

// Turns successive 33-bit PTS values into one 64-bit count that does not wrap
// and does not step back by more than 5 seconds.
class PtsUnwrapper {
public:
    std::uint64_t unwrap(std::int64_t pts);

private:
    std::int64_t m_lastPts = -1;
    std::uint64_t m_offset = 0;
};

class M2tsPacketizer final {
public:
    explicit M2tsPacketizer(QString sourcePath);

    // MSFTS carriage-profile knobs (msfts#7). Call before open(); defaults
    // preserve the historical filtered single-program behavior.
    void setTransparent(bool transparent);      // carry the whole multiplex verbatim
    void setRetainSiTables(bool retain);         // keep SDT/EIT/TDT-TOT/NIT PIDs
    void setRetainNullPackets(bool retain);      // keep null (0x1FFF) packets

    bool open(int requestedProgramNumber, QString* error);
    bool readObject(int packetsPerObject, M2tsObject* object, QString* error);
    int packetSize() const;
    int programNumber() const;
    int pmtPid() const;
    int pcrPid() const;
    // Programs listed in the first PAT read at open(), not counting the network
    // PID entry. 0 when no PAT was parsed. A source whose PAT lists one program
    // is a single-program transport stream.
    int patProgramCount() const;
    // DVB SI PIDs kept alongside the selected program by setRetainSiTables().
    // Empty unless retention is on, which is what the catalog advertises as
    // m2tsSiPids: PIDs retained in the filtered track beyond those in the PMT.
    QList<int> retainedSiPids() const;
    QByteArray initData() const;
    std::uint64_t objectsRead() const;
    // True when the source is a non-seekable stream (FIFO, pipe, /dev/stdin),
    // i.e. a live feed rather than a seekable VOD file. Valid after open().
    bool sequential() const;

    // Probes the file with libav and returns its duration in integer
    // milliseconds, or 0 if unknown / libav unavailable / on any failure.
    static qint64 probeDurationMs(const QString& sourcePath);

private:
    bool detectPacketSize(QString* error);
    bool collectInitData(QString* error);
    bool identifyVideoPid(QString* error);
    bool packetHasSync(const QByteArray& packet) const;
    QByteArray tsPacketView(const QByteArray& sourcePacket) const;
    bool hasRandomAccessIndicator(const QByteArray& tsPacket) const;

    QString m_sourcePath;
    QFile m_file;
    int m_packetSize = 0;
    int m_requestedProgramNumber = 0;
    int m_programNumber = 1;
    int m_pmtPid = -1;
    int m_pcrPid = -1;
    int m_patProgramCount = 0;
    // First video elementary PID of the PMT; the PTS source for Object media time.
    int m_videoPid = -1;
    PtsUnwrapper m_ptsUnwrapper;
    std::set<int> m_selectedPids;
    QByteArray m_initData;
    std::uint64_t m_nextObjectId = 0;

    // MSFTS group numbering (draft-gregoire-moq-msfts Section 6.3): groups start at
    // random access points (IDR boundaries), objects increment within a group.
    std::uint64_t m_currentGroupId = 0;
    std::uint64_t m_nextObjectIdInGroup = 0;
    bool m_sawFirstRap = false;
    // The PID on which we trigger group boundaries. Set to the first PID where
    // random_access_indicator is observed; in practice this is the video PID.
    int m_rapPid = -1;

    // Carriage-profile state (msfts#7).
    bool m_transparent = false;
    bool m_retainSiTables = false;
    QList<int> m_retainedSiPids;
    bool m_retainNullPackets = false;
    // True for non-seekable streams (FIFO, /dev/stdin, pipe). In that mode
    // collectInitData cannot rewind, so packets consumed while scanning for
    // PAT/PMT are buffered here and drained by readObject before further reads,
    // preserving byte-faithful ordering.
    bool m_sequential = false;
    QByteArray m_prebuffer;
    int m_prebufferPos = 0;
};

} // namespace moq2ts
