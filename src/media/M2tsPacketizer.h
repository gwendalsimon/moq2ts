#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>

#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

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

// MPEG-2 CRC_32 (ISO/IEC 13818-1 Annex A) of size octets.
std::uint32_t mpegCrc32(const char* data, qsizetype size);

// Reassembles the PSI sections of one PID (ISO/IEC 13818-1 Section 2.4.4). It
// follows the pointer_field, joins a section across packets, and reads several
// sections from one packet. It drops a section with a bad CRC_32, and a partial
// section when the continuity counter shows a lost packet.
class PsiAssembler {
public:
    struct Section {
        QByteArray bytes;                  // table_id through CRC_32
        QList<QByteArray> sourcePackets;   // the packets that carried it, in order
    };

    // Feeds one 188-octet TS packet and the source packet it came from (188 or
    // 192 octets). Returns the sections that this packet completes.
    QList<Section> push(const QByteArray& tsPacket, const QByteArray& sourcePacket);
    void reset();

private:
    // Appends to the section in progress and stops at its end. Returns the
    // octets used.
    int append(const char* data, int size, const QByteArray& sourcePacket, QList<Section>* done);

    QByteArray m_section;
    QList<QByteArray> m_packets;
    bool m_collecting = false;
    bool m_packetRecorded = false;   // the current packet is in m_packets
    int m_lastCc = -1;
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
    // Reads up to 4096 packets for the PAT and the selected program's PMT.
    bool scanPsi(QString* error);
    void handlePsiPacket(const QByteArray& tsPacket, const QByteArray& sourcePacket);
    void onPat(const PsiAssembler::Section& section);
    void onPmt(const PsiAssembler::Section& section);
    // Per-program carriage: the PIDs to keep, and initData.
    bool selectProgramPids(QString* error);
    void selectPids();
    // Stops the track: readObject returns false with this reason from now on.
    void endTrack(const QString& reason);
    // Per-program carriage: the PAT that lists the selected program only.
    void rewritePat();
    QByteArray rewrittenPatPacket(const QByteArray& sourcePacket, int continuityCounter) const;
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

    // Current PSI of the source: the last PAT, the last PMT of the selected
    // program, and what they list.
    PsiAssembler m_patAssembler;
    PsiAssembler m_pmtAssembler;
    PsiAssembler::Section m_pat;
    PsiAssembler::Section m_pmt;
    std::vector<std::pair<int, int>> m_patPrograms;
    int m_networkPid = -1;
    std::set<int> m_elementaryPids;

    // The rewritten PAT of per-program carriage.
    QByteArray m_rewrittenPat;          // the section
    QByteArray m_rewrittenPatContent;   // transport_stream_id and entries
    int m_rewrittenPatVersion = -1;
    int m_patContinuityCounter = 0;

    // Live PSI tracking.
    bool m_opened = false;        // open() is done; PSI changes now apply
    int m_openProgramCount = 0;   // PAT programs at open(), which set the mode
    bool m_ended = false;
    QString m_endReason;
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
