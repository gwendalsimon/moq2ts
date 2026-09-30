#include "M2tsPacketizer.h"

#include <algorithm>

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

bool extractPsiSection(const QByteArray& tsPacket, QByteArray* section) {
    if (!payloadUnitStart(tsPacket)) {
        return false;
    }
    const int offset = payloadOffset(tsPacket);
    if (offset < 0 || offset >= tsPacket.size()) {
        return false;
    }

    const int pointer = static_cast<unsigned char>(tsPacket[offset]);
    const int sectionStart = offset + 1 + pointer;
    if (sectionStart + 3 > tsPacket.size()) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(tsPacket[sectionStart + 1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(tsPacket[sectionStart + 2]);
    if (sectionLength < 5 || sectionStart + 3 + sectionLength > tsPacket.size()) {
        return false;
    }
    *section = tsPacket.mid(sectionStart, 3 + sectionLength);
    return true;
}

// Number of programs a PAT section lists, not counting program_number 0 (the
// network PID). Returns -1 when the section is not a PAT.
int countPatPrograms(const QByteArray& section) {
    if (section.size() < 12 || static_cast<unsigned char>(section[0]) != 0x00) {
        return -1;
    }
    const int sectionLength = ((static_cast<unsigned char>(section[1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(section[2]);
    const int entriesEnd = 3 + sectionLength - 4;
    int count = 0;
    for (int offset = 8; offset + 4 <= entriesEnd; offset += 4) {
        const int program = (static_cast<unsigned char>(section[offset]) << 8) |
                            static_cast<unsigned char>(section[offset + 1]);
        if (program != 0) {
            ++count;
        }
    }
    return count;
}

bool findPatProgram(const QByteArray& section, int requestedProgram, int* programNumber, int* pmtPid) {
    if (section.size() < 12 || static_cast<unsigned char>(section[0]) != 0x00) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(section[1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(section[2]);
    const int entriesEnd = 3 + sectionLength - 4;
    for (int offset = 8; offset + 4 <= entriesEnd; offset += 4) {
        const int program = (static_cast<unsigned char>(section[offset]) << 8) |
                            static_cast<unsigned char>(section[offset + 1]);
        const int pid = ((static_cast<unsigned char>(section[offset + 2]) & 0x1f) << 8) |
                        static_cast<unsigned char>(section[offset + 3]);
        if (program == 0) {
            continue;
        }
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

// PTS of a PES packet that starts in tsPacket, in 90 kHz units. Returns -1 when
// the packet starts no PES packet or the header carries no PTS.
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
    // Transparent mode carries the whole multiplex verbatim: no PID filtering,
    // and initData stays empty (full PSI rides in-band). However, we still scan
    // PAT/PMT to identify the PCR PID (video) for MSFTS Section 6.3 RAI-based group
    // boundary detection.
    if (!m_transparent) {
        if (!collectInitData(error)) {
            return false;
        }
    } else {
        if (!identifyVideoPid(error)) {
            // Non-fatal for transparent mode: if we can't find the video PID,
            // fall back to latching on the first PID with RAI at runtime.
            if (error) {
                *error = QString(); // clear - not fatal
            }
        }
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

bool M2tsPacketizer::collectInitData(QString* error) {
    const qint64 originalPos = m_file.pos();
    if (!m_sequential && !m_file.seek(0)) {
        if (error) {
            *error = QStringLiteral("Failed to seek M2TS source while collecting initData.");
        }
        return false;
    }

    QByteArray patPacket;
    QByteArray pmtPacket;
    std::set<int> elementaryPids;
    constexpr int maxPacketsToScan = 4096;
    for (int index = 0; index < maxPacketsToScan; ++index) {
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
            if (error) {
                *error = QStringLiteral("Invalid TS sync byte while collecting initData.");
            }
            return false;
        }

        const QByteArray tsPacket = tsPacketView(sourcePacket);
        const int pid = pidOf(tsPacket);
        if (pid == 0 && patPacket.isEmpty()) {
            QByteArray patSection;
            if (extractPsiSection(tsPacket, &patSection) &&
                findPatProgram(patSection, m_requestedProgramNumber, &m_programNumber, &m_pmtPid)) {
                patPacket = sourcePacket;
                m_patProgramCount = countPatPrograms(patSection);
            }
        } else if (m_pmtPid >= 0 && pid == m_pmtPid && pmtPacket.isEmpty()) {
            QByteArray pmtSection;
            if (extractPsiSection(tsPacket, &pmtSection) && !pmtSection.isEmpty() &&
                parsePmt(pmtSection, &m_pcrPid, &elementaryPids, &m_videoPid)) {
                pmtPacket = sourcePacket;
            }
        }

        if (!patPacket.isEmpty() && !pmtPacket.isEmpty()) {
            break;
        }
    }

    if (!patPacket.isEmpty()) {
        m_initData += patPacket;
    }
    if (!pmtPacket.isEmpty()) {
        m_initData += pmtPacket;
    }

    if (m_initData.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Failed to collect PAT/PMT packets for catalog initData.");
        }
        return false;
    }

    m_selectedPids.clear();
    m_selectedPids.insert(0x0000);
    if (m_pmtPid >= 0) {
        m_selectedPids.insert(m_pmtPid);
    }
    if (m_pcrPid >= 0) {
        m_selectedPids.insert(m_pcrPid);
    }
    m_selectedPids.insert(elementaryPids.begin(), elementaryPids.end());

    if (m_requestedProgramNumber != 0 && m_programNumber != m_requestedProgramNumber) {
        if (error) {
            *error = QStringLiteral("Requested program %1 was not found in PAT.").arg(m_requestedProgramNumber);
        }
        return false;
    }
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

    // Non-seekable sources keep their forward-only position; readObject drains the
    // prebuffer captured above.
    return m_sequential ? true : m_file.seek(originalPos);
}

bool M2tsPacketizer::identifyVideoPid(QString* error) {
    // Lightweight PAT/PMT scan for transparent mode: identifies the PCR PID
    // (typically the video PID) for RAI-based group boundary detection without
    // building initData or setting up PID filtering.
    const qint64 originalPos = m_file.pos();
    if (!m_sequential && !m_file.seek(0)) {
        return false;
    }

    constexpr int maxPacketsToScan = 4096;
    int pmtPid = -1;
    int programNumber = 0;
    int pcrPid = -1;
    std::set<int> elementaryPids;

    for (int index = 0; index < maxPacketsToScan; ++index) {
        const QByteArray sourcePacket = m_file.read(m_packetSize);
        if (sourcePacket.size() != m_packetSize) {
            break;
        }
        if (m_sequential) {
            m_prebuffer += sourcePacket;
        }
        if (!packetHasSync(sourcePacket)) {
            break;
        }

        const QByteArray tsPacket = tsPacketView(sourcePacket);
        const int pid = pidOf(tsPacket);
        if (pid == 0 && pmtPid < 0) {
            QByteArray patSection;
            if (extractPsiSection(tsPacket, &patSection)) {
                m_patProgramCount = std::max(0, countPatPrograms(patSection));
                // A single-program source has only one program to describe, so
                // the requested number does not apply to it.
                findPatProgram(patSection, m_patProgramCount == 1 ? 0 : m_requestedProgramNumber,
                               &programNumber, &pmtPid);
            }
        } else if (pmtPid >= 0 && pid == pmtPid && pcrPid < 0) {
            QByteArray pmtSection;
            if (extractPsiSection(tsPacket, &pmtSection) && !pmtSection.isEmpty()) {
                parsePmt(pmtSection, &pcrPid, &elementaryPids, &m_videoPid);
            }
        }

        if (pmtPid >= 0 && pcrPid >= 0) {
            break;
        }
    }

    if (pcrPid >= 0) {
        m_pcrPid = pcrPid;
        m_rapPid = pcrPid; // Use PCR PID (video) for RAI group detection
        m_pmtPid = pmtPid;
        m_programNumber = programNumber;
    }

    if (!m_sequential) {
        m_file.seek(originalPos);
    }
    return pcrPid >= 0;
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

    // MSFTS Section 6.3 Group Numbering: start a new group at random access points
    // (IDR boundaries). Scan packets in this object for the random_access_indicator
    // in the adaptation field. The video (PCR) PID is identified from PAT/PMT at
    // open() time; if that scan failed (e.g. sequential source), fall back to
    // latching onto the first PID where RAI is observed at runtime.
    bool rapDetected = false;
    const int ps = m_packetSize;
    for (int offset = 0; offset < payload.size(); offset += ps) {
        const QByteArray sourcePacket = payload.mid(offset, ps);
        const QByteArray tsView = tsPacketView(sourcePacket);
        const int pid = pidOf(tsView);
        // Skip PSI (PAT=0x0000, CAT=0x0001) and null (0x1FFF)
        if (pid <= 0x001F || pid == 0x1FFF) {
            continue;
        }
        if (!hasRandomAccessIndicator(tsView)) {
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
        if (pid == m_rapPid) {
            rapDetected = true;
            break;
        }
    }

    if (rapDetected && m_sawFirstRap) {
        // New group at this RAP boundary
        ++m_currentGroupId;
        m_nextObjectIdInGroup = 0;
    }
    if (rapDetected) {
        m_sawFirstRap = true;
    }

    // MSF media timeline (draft-ietf-moq-msf-01 Section 7.1.1): the media time
    // of an Object is the PTS of its first media sample. Take the first video
    // PES that starts in this Object, on the PMT's video PID or, failing that,
    // the PCR PID.
    object->ptsUs.reset();
    const int ptsPid = m_videoPid >= 0 ? m_videoPid : m_pcrPid;
    for (int offset = 0; ptsPid >= 0 && offset < payload.size(); offset += ps) {
        const QByteArray tsView = tsPacketView(payload.mid(offset, ps));
        if (pidOf(tsView) != ptsPid) {
            continue;
        }
        const std::int64_t pts = pesPts(tsView);
        if (pts >= 0) {
            object->ptsUs = unwrapPts(pts) * 100 / 9;   // 90 kHz to microseconds, floored
            break;
        }
    }

    object->payload = std::move(payload);
    object->groupId = m_currentGroupId;
    object->objectId = m_nextObjectIdInGroup++;
    object->startsGroup = rapDetected;
    ++m_nextObjectId;
    return true;
}

std::uint64_t M2tsPacketizer::unwrapPts(std::int64_t pts) {
    // The PTS wraps at 2^33. A step back of more than half that range is a wrap.
    constexpr std::int64_t kPtsRange = std::int64_t{1} << 33;
    if (m_lastPts >= 0 && pts + kPtsRange / 2 < m_lastPts) {
        m_ptsWrapOffset += static_cast<std::uint64_t>(kPtsRange);
    }
    m_lastPts = pts;
    return m_ptsWrapOffset + static_cast<std::uint64_t>(pts);
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
