#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct receiver_ts_pid_state {
    uint64_t last_packet_hash;
    uint8_t last_cc;
    uint8_t repeats;
    bool seen;
    bool last_had_payload;
} receiver_ts_pid_state_t;

typedef struct receiver_ts_stats {
    uint64_t packets;
    uint64_t sync_errors;
    uint64_t malformed_packets;
    uint64_t transport_errors;
    uint64_t continuity_errors;
    receiver_ts_pid_state_t pids[8192];
} receiver_ts_stats_t;

void receiver_ts_stats_init(receiver_ts_stats_t *stats);
bool receiver_ts_stats_add(receiver_ts_stats_t *stats, const uint8_t *packet,
                           uint32_t packet_size);
