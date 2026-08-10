#pragma once

#include <QString>

namespace moq2ts {

enum class AudioCodecPreset {
    AAC,
    Opus,
};

struct PublishConfig {
    QString moqEndpoint = "mock://local";
    QString namespaceName = "live/paul1";

    QString videoSource = QString(); // M2TS file/pipe/URL for H.264 source
    QString audioSource = QString(); // M2TS file/pipe/URL for audio source (optional)
    QString cameraDeviceId = QString(); // UI-selected capture device id
    QString microphoneDeviceId = QString(); // UI-selected capture device id

    int videoWidth = 1920;
    int videoHeight = 1080;
    int videoFramerate = 30;
    int videoTargetBitrateKbps = 2500;
    int keyframeIntervalMs = 1000;  // live H.264 IDR cadence; MOQT group cadence follows it
    bool paceEgress = true; // pace capture object release on real media time

    int audioSampleRate = 48000;
    int audioChannels = 2;
    int audioTargetBitrateKbps = 160;
    AudioCodecPreset audioCodec = AudioCodecPreset::AAC;

    int fragmentDurationMs = 250;
    int targetSegmentBytes = 64 * 1024;
    int programNumber = 0; // 0 selects the first nonzero PAT program

    // MSFTS carriage-profile options (draft-gregoire-moq-msfts feedback, msfts#7).
    // Defaults preserve the historical filtered single-program behavior.
    bool transparentMode = false;   // carry the whole multiplex verbatim (no filter/rewrite)
    bool retainSiTables = false;    // filtered path: also keep SDT/EIT/TDT-TOT/NIT PIDs
    bool retainNullPackets = false; // filtered path: also keep null (0x1FFF) packets
    int m2tsMuxRateBps = 0;         // advisory source mux rate; 0 omits the catalog hint
    bool pacedFileSource = false;    // pace file-source publishing at media-time rate
    int draftVersion = 16;              // MOQ draft version (14 or 16)

    bool forceRealtime = true;
    bool useOpenh264 = true;
    bool useLibAvTranscode = true;
    bool useLibOpusFallback = false;
};

} // namespace moq2ts
