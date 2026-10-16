#include "TsStats.h"

#include <stddef.h>
#include <string.h>

static uint64_t packet_hash(const uint8_t *packet) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < 188; i++) {
        hash ^= packet[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void receiver_ts_stats_init(receiver_ts_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
}

bool receiver_ts_stats_add(receiver_ts_stats_t *stats, const uint8_t *source,
                           uint32_t packet_size) {
    if (!stats || !source || (packet_size != 188 && packet_size != 192)) return false;
    const uint8_t *p = source + (packet_size == 192 ? 4 : 0);
    stats->packets++;
    if (p[0] != 0x47) {
        stats->sync_errors++;
        return false;
    }
    const uint8_t afc = (p[3] >> 4) & 3;
    if (!afc || (afc & 2 && (size_t)p[4] + 5 > 188)) {
        stats->malformed_packets++;
        return false;
    }
    const uint16_t pid = (uint16_t)(((p[1] & 0x1f) << 8) | p[2]);
    const uint8_t cc = p[3] & 0x0f;
    if (p[1] & 0x80) stats->transport_errors++;
    if (pid == 0x1fff) return true;
    receiver_ts_pid_state_t *state = &stats->pids[pid];
    const bool discontinuity = (afc & 2) && p[4] > 0 && (p[5] & 0x80);
    if (discontinuity) state->seen = false;
    const uint64_t hash = packet_hash(p);
    if (state->seen) {
        const uint8_t expected = (uint8_t)((state->last_cc + ((afc & 1) ? 1 : 0)) & 15);
        if (cc != expected) {
            if ((afc & 1) && state->last_had_payload &&
                cc == state->last_cc && hash == state->last_packet_hash &&
                state->repeats < 1) {
                state->repeats++;
            } else {
                stats->continuity_errors++;
                state->repeats = 0;
            }
        } else {
            state->repeats = 0;
        }
    }
    state->last_cc = cc;
    state->last_packet_hash = hash;
    state->last_had_payload = (afc & 1) != 0;
    state->seen = true;
    return true;
}
