#include "M2tsPacketizer.h"

#include <algorithm>
#include <vector>

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif

namespace moq2ts {

namespace {

int pidOf(const QByteArray& tsPacket) {
    if (tsPacket.size() < 4) {
        return -1;
    }
    return ((static_cast<unsigned char>(tsPacket[1]) & 0x1f) << 8) |
           static_cast<unsigned char>(tsPacket[2]);
}

bool payloadUnitStart(const QByteArray& tsPacket) {
    return tsPacket.size() >= 2 && (static_cast<unsigned char>(tsPacket[1]) & 0x40) != 0;
}

int payloadOffset(const QByteArray& tsPacket) {
    if (tsPacket.size() < 4) {
        return -1;
    }
    const int adaptationControl = (static_cast<unsigned char>(tsPacket[3]) >> 4) & 0x03;
    if (adaptationControl == 0 || adaptationControl == 2) {
        return -1;
    }
    int offset = 4;
    if (adaptationControl == 3) {
        if (tsPacket.size() < 5) {
            return -1;
        }
        offset += 1 + static_cast<unsigned char>(tsPacket[4]);
    }
    return offset < tsPacket.size() ? offset : -1;
}

// The (program_number, PMT PID) entries of a PAT section, without the network
// PID entry (program_number 0). Returns false when the section is not a PAT.
bool parsePat(const QByteArray& section, std::vector<std::pair<int, int>>* programs) {
    programs->clear();
    if (section.size() < 12 || static_cast<unsigned char>(section[0]) != 0x00) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(section[1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(section[2]);
    const int entriesEnd = std::min<int>(3 + sectionLength - 4, section.size());
    for (int offset = 8; offset + 4 <= entriesEnd; offset += 4) {
        const int program = (static_cast<unsigned char>(section[offset]) << 8) |
                            static_cast<unsigned char>(section[offset + 1]);
        const int pid = ((static_cast<unsigned char>(section[offset + 2]) & 0x1f) << 8) |
                        static_cast<unsigned char>(section[offset + 3]);
        if (program != 0) {
            programs->emplace_back(program, pid);
        }
    }
    return true;
}

// The requested program of a PAT, or its first program when requestedProgram is 0.
bool findPatProgram(const std::vector<std::pair<int, int>>& programs, int requestedProgram,
                    int* programNumber, int* pmtPid) {
    for (const auto& [program, pid] : programs) {
        if (requestedProgram == 0 || requestedProgram == program) {
            *programNumber = program;
            *pmtPid = pid;
            return true;
        }
    }
    return false;
}

// Video stream_type values (ISO/IEC 13818-1 Table 2-34): MPEG-1, MPEG-2,
// MPEG-4 Part 2, H.264, H.265, and H.266.
bool isVideoStreamType(int streamType) {
    switch (streamType) {
    case 0x01: case 0x02: case 0x10: case 0x1B: case 0x24: case 0x33:
        return true;
    default:
        return false;
    }
}

bool parsePmt(const QByteArray& section, int* pcrPid, std::set<int>* elementaryPids, int* videoPid) {
    if (section.size() < 16 || static_cast<unsigned char>(section[0]) != 0x02) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(section[1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(section[2]);
    const int sectionEnd = 3 + sectionLength - 4;
    if (sectionEnd > section.size() || sectionEnd < 12) {
        return false;
    }

    elementaryPids->clear();
    *videoPid = -1;
    *pcrPid = ((static_cast<unsigned char>(section[8]) & 0x1f) << 8) |
              static_cast<unsigned char>(section[9]);
    const int programInfoLength = ((static_cast<unsigned char>(section[10]) & 0x0f) << 8) |
                                  static_cast<unsigned char>(section[11]);
    int offset = 12 + programInfoLength;
    while (offset + 5 <= sectionEnd) {
        const int streamType = static_cast<unsigned char>(section[offset]);
        const int elementaryPid = ((static_cast<unsigned char>(section[offset + 1]) & 0x1f) << 8) |
                                  static_cast<unsigned char>(section[offset + 2]);
        const int esInfoLength = ((static_cast<unsigned char>(section[offset + 3]) & 0x0f) << 8) |
                                 static_cast<unsigned char>(section[offset + 4]);
        elementaryPids->insert(elementaryPid);
        if (*videoPid < 0 && isVideoStreamType(streamType)) {
            *videoPid = elementaryPid;
        }
        offset += 5 + esInfoLength;
    }
    return true;
}

} // namespace

std::uint32_t mpegCrc32(const char* data, qsizetype size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (qsizetype index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(data[index])) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

void PsiAssembler::reset() {
    m_section.clear();
    m_packets.clear();
    m_collecting = false;
    m_packetRecorded = false;
    m_lastCc = -1;
}

QList<PsiAssembler::Section> PsiAssembler::push(const QByteArray& tsPacket, const QByteArray& sourcePacket) {
    QList<Section> done;
    const int offset = payloadOffset(tsPacket);
    if (offset < 0) {
        return done;   // no payload; the continuity counter does not advance
    }
    const int cc = static_cast<unsigned char>(tsPacket[3]) & 0x0F;
    if (m_lastCc >= 0 && cc == m_lastCc) {
        return done;   // a duplicate packet (ISO/IEC 13818-1 Section 2.4.3.3)
    }
    if (m_lastCc >= 0 && cc != ((m_lastCc + 1) & 0x0F)) {
        m_collecting = false;   // a lost packet breaks the section in progress
    }
    m_lastCc = cc;
    m_packetRecorded = false;

    const char* payload = tsPacket.constData() + offset;
    const int size = static_cast<int>(tsPacket.size()) - offset;
    if (!payloadUnitStart(tsPacket)) {
        if (m_collecting) {
            append(payload, size, sourcePacket, &done);
        }
        return done;
    }
    // The pointer_field counts the bytes that end the section in progress.
    const int pointer = static_cast<unsigned char>(payload[0]);
    if (1 + pointer > size) {
        m_collecting = false;
        return done;
    }
    if (m_collecting) {
        append(payload + 1, pointer, sourcePacket, &done);
        m_collecting = false;   // a section still incomplete here is corrupt
    }
    // New sections follow until the packet ends or stuffing (0xFF) begins.
    int position = 1 + pointer;
    while (position < size && static_cast<unsigned char>(payload[position]) != 0xFF) {
        m_collecting = true;
        m_section.clear();
        m_packets.clear();
        m_packetRecorded = false;
        position += append(payload + position, size - position, sourcePacket, &done);
        if (m_collecting) {
            break;   // the section continues in the next packet
        }
    }
    return done;
}

int PsiAssembler::append(const char* data, int size, const QByteArray& sourcePacket, QList<Section>* done) {
    int used = 0;
    while (used < size && m_collecting) {
        const int total = m_section.size() < 3
            ? 3
            : 3 + (((static_cast<unsigned char>(m_section[1]) & 0x0F) << 8) |
                   static_cast<unsigned char>(m_section[2]));
        const int take = std::min(total - static_cast<int>(m_section.size()), size - used);
        m_section.append(data + used, take);
        used += take;
        if (!m_packetRecorded) {
            m_packets.append(sourcePacket);
            m_packetRecorded = true;
        }
        if (m_section.size() < 3) {
            continue;
        }
        const int length = 3 + (((static_cast<unsigned char>(m_section[1]) & 0x0F) << 8) |
                                static_cast<unsigned char>(m_section[2]));
        if (length > 4096) {
            m_collecting = false;   // longer than any PSI or private section
        } else if (m_section.size() == length) {
            m_collecting = false;
            // With section_syntax_indicator set, the section ends with a CRC_32,
            // and the CRC over the whole section is then 0.
            const bool syntax = (static_cast<unsigned char>(m_section[1]) & 0x80) != 0;
            if (!syntax || (length >= 8 && mpegCrc32(m_section.constData(), length) == 0)) {
                done->append({m_section, m_packets});
            }
        }
    }
    return used;
}

std::int64_t pesPts(const QByteArray& tsPacket) {
    if (!payloadUnitStart(tsPacket)) {
        return -1;
    }
    const int offset = payloadOffset(tsPacket);
    if (offset < 0 || offset + 14 > tsPacket.size()) {
        return -1;
    }
    const auto byte = [&](int index) { return static_cast<std::int64_t>(static_cast<unsigned char>(tsPacket[offset + index])); };
    if (byte(0) != 0x00 || byte(1) != 0x00 || byte(2) != 0x01) {
        return -1;
    }
    // '10' marker bits, then PTS_DTS_flags '10' or '11'.
    if ((byte(6) & 0xC0) != 0x80 || (byte(7) & 0x80) == 0) {
        return -1;
    }
    return (((byte(9) >> 1) & 0x07) << 30) | (byte(10) << 22) | ((byte(11) >> 1) << 15) |
           (byte(12) << 7) | (byte(13) >> 1);
}

std::uint64_t PtsUnwrapper::unwrap(std::int64_t pts) {
    // The PTS wraps at 2^33. A step back of more than half that range is a wrap.
    // A shorter step back of more than 5 seconds is a discontinuity, as in a
    // looped file; B-frame reordering steps back by far less. The offset then
    // absorbs the step, so the output never goes back.
    constexpr std::int64_t kPtsRange = std::int64_t{1} << 33;
    constexpr std::int64_t kMaxStepBack = 5 * 90000;
    if (m_lastPts >= 0 && pts + kPtsRange / 2 < m_lastPts) {
        m_offset += static_cast<std::uint64_t>(kPtsRange);
    } else if (m_lastPts >= 0 && pts + kMaxStepBack < m_lastPts) {
        qWarning("PTS discontinuity: %lld after %lld (90 kHz); media time continues from the last value.",
                 static_cast<long long>(pts), static_cast<long long>(m_lastPts));
        m_offset += static_cast<std::uint64_t>(m_lastPts - pts);
    }
    m_lastPts = pts;
    return m_offset + static_cast<std::uint64_t>(pts);
}

M2tsPacketizer::M2tsPacketizer(QString sourcePath)
    : m_sourcePath(std::move(sourcePath)),
      m_file(m_sourcePath) {}

void M2tsPacketizer::setTransparent(bool transparent) {
    m_transparent = transparent;
}

QList<int> M2tsPacketizer::retainedSiPids() const {
    return m_retainedSiPids;
}

void M2tsPacketizer::setRetainSiTables(bool retain) {
    m_retainSiTables = retain;
}

void M2tsPacketizer::setRetainNullPackets(bool retain) {
    m_retainNullPackets = retain;
}

bool M2tsPacketizer::open(int requestedProgramNumber, QString* error) {
    m_requestedProgramNumber = requestedProgramNumber;
    if (!m_file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Failed to open M2TS source: %1").arg(m_file.errorString());
        }
        return false;
    }
    // FIFOs, pipes and /dev/stdin are non-seekable; readObject drains a prebuffer
    // instead of rewinding (see collectInitData).
    m_sequential = m_file.isSequential();
    if (!detectPacketSize(error)) {
        return false;
    }
    // Both modes read the PAT and the PMT of the selected program first. The
    // PCR (video) PID drives the group boundaries, and per-program carriage
    // also needs the PIDs to keep and the initData.
    const bool psiFound = scanPsi(error);
    if (!m_transparent) {
        if (!psiFound || !selectProgramPids(error)) {
            return false;
        }
    } else if (!psiFound) {
        // Non-fatal for unmodified carriage: without the PCR PID, the group
        // boundaries latch on the first PID with a random access indicator.
        if (error) {
            *error = QString();
        }
    }
    if (m_pcrPid >= 0) {
        m_rapPid = m_pcrPid;
    }
    // Non-seekable sources cannot rewind; the prebuffer (or the peeked bytes) is
    // replayed forward-only by readObject.
    return m_sequential ? true : m_file.seek(0);
}

bool M2tsPacketizer::detectPacketSize(QString* error) {
    const QByteArray probe = m_file.peek(192 * 4);
    if (probe.size() < 188) {
        if (error) {
            *error = QStringLiteral("Input is too small to contain a TS packet.");
        }
        return false;
    }

    const auto syncAt = [&probe](int offset) {
        return offset >= 0 && offset < probe.size() && static_cast<unsigned char>(probe[offset]) == 0x47;
    };

    if (syncAt(0) && (probe.size() < 188 * 2 || syncAt(188))) {
        m_packetSize = 188;
        return true;
    }
    if (syncAt(4) && (probe.size() < 192 * 2 || syncAt(196))) {
        m_packetSize = 192;
        return true;
    }

    if (error) {
        *error = QStringLiteral("Input is not packet-aligned TS/M2TS. Expected sync byte at offset 0 for 188-byte TS or offset 4 for 192-byte M2TS.");
    }
    return false;
}

bool M2tsPacketizer::packetHasSync(const QByteArray& packet) const {
    if (packet.size() != m_packetSize) {
        return false;
    }
    const int syncOffset = m_packetSize == 192 ? 4 : 0;
    return static_cast<unsigned char>(packet[syncOffset]) == 0x47;
}

QByteArray M2tsPacketizer::tsPacketView(const QByteArray& sourcePacket) const {
    if (m_packetSize == 192) {
        return sourcePacket.mid(4, 188);
    }
    return sourcePacket;
}

bool M2tsPacketizer::scanPsi(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    const qint64 originalPos = m_file.pos();
    if (!m_sequential && !m_file.seek(0)) {
        return fail(QStringLiteral("Failed to seek M2TS source while reading the PAT and PMT."));
    }

    constexpr int maxPacketsToScan = 4096;
    for (int index = 0; index < maxPacketsToScan && m_pmt.bytes.isEmpty(); ++index) {
        const QByteArray sourcePacket = m_file.read(m_packetSize);
        if (sourcePacket.size() != m_packetSize) {
            break;
        }
        // On non-seekable input we cannot rewind after the scan, so retain every
        // packet consumed here for readObject to replay in order.
        if (m_sequential) {
            m_prebuffer += sourcePacket;
        }
        if (!packetHasSync(sourcePacket)) {
            return fail(QStringLiteral("Invalid TS sync byte while reading the PAT and PMT."));
        }
        handlePsiPacket(tsPacketView(sourcePacket), sourcePacket);
    }
    // Non-seekable sources keep their forward-only position; readObject drains the
    // prebuffer captured above.
    if (!m_sequential) {
        m_file.seek(originalPos);
    }

    if (m_pmtPid < 0 && m_requestedProgramNumber != 0 && !m_patPrograms.empty()) {
        return fail(QStringLiteral("Requested program %1 was not found in PAT.").arg(m_requestedProgramNumber));
    }
    if (m_pmt.bytes.isEmpty()) {
        return fail(QStringLiteral("Failed to collect PAT/PMT packets for catalog initData."));
    }
    return true;
}

void M2tsPacketizer::handlePsiPacket(const QByteArray& tsPacket, const QByteArray& sourcePacket) {
    const int pid = pidOf(tsPacket);
    if (pid == 0x0000) {
        for (const PsiAssembler::Section& section : m_patAssembler.push(tsPacket, sourcePacket)) {
            onPat(section);
        }
    } else if (pid == m_pmtPid) {
        for (const PsiAssembler::Section& section : m_pmtAssembler.push(tsPacket, sourcePacket)) {
            onPmt(section);
        }
    }
}

void M2tsPacketizer::onPat(const PsiAssembler::Section& section) {
    std::vector<std::pair<int, int>> programs;
    if (section.bytes == m_pat.bytes || !parsePat(section.bytes, &programs)) {
        return;   // a repeat, or not a PAT
    }
    m_pat = section;
    m_patPrograms = programs;
    m_patProgramCount = static_cast<int>(programs.size());
    if (m_pmtPid < 0) {
        // A single-program source has only one program to describe, so the
        // requested number does not apply to it in unmodified carriage.
        const int requested = m_transparent && m_patProgramCount == 1 ? 0 : m_requestedProgramNumber;
        findPatProgram(programs, requested, &m_programNumber, &m_pmtPid);
    }
}

void M2tsPacketizer::onPmt(const PsiAssembler::Section& section) {
    // A PMT PID can carry the PMTs of several programs; keep the selected one.
    if (section.bytes == m_pmt.bytes || section.bytes.size() < 5 ||
        ((static_cast<unsigned char>(section.bytes[3]) << 8) | static_cast<unsigned char>(section.bytes[4])) !=
            m_programNumber) {
        return;
    }
    int pcrPid = -1;
    int videoPid = -1;
    std::set<int> elementaryPids;
    if (!parsePmt(section.bytes, &pcrPid, &elementaryPids, &videoPid)) {
        return;
    }
    m_pmt = section;
    m_pcrPid = pcrPid;
    m_videoPid = videoPid;
    m_elementaryPids = elementaryPids;
}

bool M2tsPacketizer::selectProgramPids(QString* error) {
    // initData carries every packet of the PAT and of the PMT, including a
    // table that spans several packets.
    m_initData.clear();
    for (const QByteArray& packet : m_pat.sourcePackets) {
        m_initData += packet;
    }
    for (const QByteArray& packet : m_pmt.sourcePackets) {
        m_initData += packet;
    }

    m_selectedPids.clear();
    m_selectedPids.insert(0x0000);
    m_selectedPids.insert(m_pmtPid);
    if (m_pcrPid >= 0) {
        m_selectedPids.insert(m_pcrPid);
    }
    m_selectedPids.insert(m_elementaryPids.begin(), m_elementaryPids.end());
    if (m_selectedPids.size() <= 2) {
        if (error) {
            *error = QStringLiteral("Failed to parse selected program PMT elementary PIDs.");
        }
        return false;
    }

    // Optional SI-table retention (msfts#7 suggestion 1). Added after the
    // elementary-PID sanity check above so it cannot mask an unparsed PMT: keep
    // the well-known DVB PSI/SI PIDs (NIT 0x10, SDT/BAT 0x11, EIT 0x12,
    // TDT/TOT 0x14) alongside the selected program.
    if (m_retainSiTables) {
        m_retainedSiPids.clear();
        for (int siPid : {0x0010, 0x0011, 0x0012, 0x0014}) {
            m_selectedPids.insert(siPid);
            m_retainedSiPids.append(siPid);
        }
    }
    return true;
}

bool M2tsPacketizer::readObject(int packetsPerObject, M2tsObject* object, QString* error) {
    if (object == nullptr || m_packetSize <= 0) {
        if (error) {
            *error = QStringLiteral("Packetizer is not open.");
        }
        return false;
    }

    const int packets = std::max(1, packetsPerObject);
    QByteArray payload;
    payload.reserve(packets * m_packetSize);

    for (int index = 0; index < packets; ++index) {
        // Drain any packets buffered during the init scan (non-seekable sources)
        // before reading further from the device, preserving stream order.
        QByteArray packet;
        if (m_prebufferPos < m_prebuffer.size()) {
            packet = m_prebuffer.mid(m_prebufferPos, m_packetSize);
            m_prebufferPos += m_packetSize;
        } else {
            packet = m_file.read(m_packetSize);
        }
        if (packet.isEmpty()) {
            break;
        }
        if (packet.size() != m_packetSize) {
            if (error) {
                *error = QStringLiteral("Source ended on a partial TS/M2TS packet.");
            }
            return false;
        }
        if (!packetHasSync(packet)) {
            if (error) {
                *error = QStringLiteral("Invalid TS sync byte in source packet.");
            }
            return false;
        }
        // Transparent mode emits every synced packet verbatim (no PID filtering).
        if (!m_transparent) {
            const int pid = pidOf(tsPacketView(packet));
            const bool selected = m_selectedPids.find(pid) != m_selectedPids.end();
            const bool keepNull = m_retainNullPackets && pid == 0x1FFF;
            if (!selected && !keepNull) {
                --index;
                continue;
            }
        }
        payload += packet;
    }

    if (payload.isEmpty()) {
        return false;
    }

    // One pass over the Object, without copying packets, finds two things:
    // - MSFTS Group numbering: a new group starts at a random access point. The
    //   video (PCR) PID is identified from PAT/PMT at open() time; if that scan
    //   failed (e.g. sequential source), the scan latches onto the first PID
    //   where the random_access_indicator is observed.
    // - MSF media timeline (draft-ietf-moq-msf-01 Section 7.1.1): the media time
    //   of an Object is the PTS of its first media sample, taken from the first
    //   video PES that starts in it, on the PMT's video PID or the PCR PID.
    bool rapDetected = false;
    object->ptsUs.reset();
    const int ps = m_packetSize;
    const int tsOffset = ps == 192 ? 4 : 0;
    const int ptsPid = m_videoPid >= 0 ? m_videoPid : m_pcrPid;
    for (qsizetype offset = 0; offset + ps <= payload.size(); offset += ps) {
        if (rapDetected && (object->ptsUs.has_value() || ptsPid < 0)) {
            break;
        }
        const QByteArray tsView = QByteArray::fromRawData(payload.constData() + offset + tsOffset, 188);
        const int pid = pidOf(tsView);
        if (!object->ptsUs.has_value() && pid == ptsPid) {
            const std::int64_t pts = pesPts(tsView);
            if (pts >= 0) {
                object->ptsUs = m_ptsUnwrapper.unwrap(pts) * 100 / 9;   // 90 kHz to microseconds, floored
            }
        }
        // Skip PSI (PAT=0x0000, CAT=0x0001) and null (0x1FFF)
        if (rapDetected || pid <= 0x001F || pid == 0x1FFF || !hasRandomAccessIndicator(tsView)) {
            continue;
        }
        // Use the known RAP PID if already identified from PAT/PMT or prior latch.
        // In filtered mode without a known PCR PID, skip non-PCR PIDs.
        if (!m_transparent && m_pcrPid >= 0 && pid != m_pcrPid) {
            continue;
        }
        if (m_rapPid < 0) {
            m_rapPid = pid; // fallback: latch on first RAI PID seen
        }
        rapDetected = pid == m_rapPid;
    }

    if (rapDetected && m_sawFirstRap) {
        // New group at this RAP boundary
        ++m_currentGroupId;
        m_nextObjectIdInGroup = 0;
    }
    if (rapDetected) {
        m_sawFirstRap = true;
    }

    object->payload = std::move(payload);
    object->groupId = m_currentGroupId;
    object->objectId = m_nextObjectIdInGroup++;
    object->startsGroup = rapDetected;
    ++m_nextObjectId;
    return true;
}

bool M2tsPacketizer::hasRandomAccessIndicator(const QByteArray& tsPacket) const {
    // TS packet adaptation field: byte 3 bits 5-4 = adaptation_field_control.
    // Values 2 (AF only) or 3 (AF + payload) indicate an adaptation field is present.
    // The adaptation field flags byte (byte 5) bit 6 = random_access_indicator.
    if (tsPacket.size() < 6) {
        return false;
    }
    const int adaptationControl = (static_cast<unsigned char>(tsPacket[3]) >> 4) & 0x03;
    if (adaptationControl < 2) {
        return false; // no adaptation field
    }
    const int afLength = static_cast<unsigned char>(tsPacket[4]);
    if (afLength < 1) {
        return false; // no flags byte
    }
    // Bit 6 of the flags byte is random_access_indicator
    return (static_cast<unsigned char>(tsPacket[5]) & 0x40) != 0;
}

int M2tsPacketizer::packetSize() const {
    return m_packetSize;
}

int M2tsPacketizer::programNumber() const {
    return m_programNumber;
}

int M2tsPacketizer::pmtPid() const {
    return m_pmtPid;
}

int M2tsPacketizer::pcrPid() const {
    return m_pcrPid;
}

int M2tsPacketizer::patProgramCount() const {
    return m_patProgramCount;
}

QByteArray M2tsPacketizer::initData() const {
    return m_initData;
}

std::uint64_t M2tsPacketizer::objectsRead() const {
    return m_nextObjectId;
}

bool M2tsPacketizer::sequential() const {
    return m_sequential;
}

qint64 M2tsPacketizer::probeDurationMs(const QString& sourcePath) {
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    if (sourcePath.isEmpty()) {
        return 0;
    }
    AVFormatContext* ctx = nullptr;
    const QByteArray path = sourcePath.toUtf8();
    if (avformat_open_input(&ctx, path.constData(), nullptr, nullptr) != 0) {
        return 0;
    }
    qint64 durationMs = 0;
    if (avformat_find_stream_info(ctx, nullptr) >= 0 && ctx->duration > 0) {
        // AVFormatContext::duration is in AV_TIME_BASE units (microseconds).
        durationMs = static_cast<qint64>((ctx->duration + (AV_TIME_BASE / 2000)) / (AV_TIME_BASE / 1000));
    }
    avformat_close_input(&ctx);
    return durationMs;
#else
    Q_UNUSED(sourcePath);
    return 0;
#endif
}

} // namespace moq2ts
