#include "../src/receive/TsStats.h"

#include <assert.h>
#include <string.h>

static void packet(unsigned char *p, unsigned pid, unsigned cc, unsigned afc) {
    memset(p, 0xff, 188);
    p[0] = 0x47;
    p[1] = (unsigned char)((pid >> 8) & 0x1f);
    p[2] = (unsigned char)pid;
    p[3] = (unsigned char)((afc << 4) | cc);
    if (afc == 2 || afc == 3) p[4] = 1;
    if (afc == 2 || afc == 3) p[5] = 0;
}

int main(void) {
    receiver_ts_stats_t s;
    receiver_ts_stats_init(&s);
    unsigned char p[188];
    packet(p, 256, 0, 1);
    assert(receiver_ts_stats_add(&s, p, 188));
    packet(p, 256, 1, 1);
    assert(receiver_ts_stats_add(&s, p, 188));
    assert(receiver_ts_stats_add(&s, p, 188));
    assert(s.continuity_errors == 0);
    p[6] = 0x00;
    assert(receiver_ts_stats_add(&s, p, 188));
    assert(s.continuity_errors == 1);
    packet(p, 256, 3, 1);
    assert(receiver_ts_stats_add(&s, p, 188));
    assert(s.continuity_errors == 2);
    p[1] |= 0x80;
    assert(receiver_ts_stats_add(&s, p, 188));
    assert(s.transport_errors == 1);
    packet(p, 256, 3, 2);
    assert(receiver_ts_stats_add(&s, p, 188));
    assert(s.continuity_errors == 3);
    packet(p, 256, 9, 3);
    p[5] = 0x80;
    assert(receiver_ts_stats_add(&s, p, 188));
    assert(s.continuity_errors == 3);
    packet(p, 0x1fff, 12, 1);
    assert(receiver_ts_stats_add(&s, p, 188));
    packet(p, 0x1fff, 8, 1);
    assert(receiver_ts_stats_add(&s, p, 188));
    assert(s.continuity_errors == 3);
    p[0] = 0;
    assert(!receiver_ts_stats_add(&s, p, 188));
    assert(s.sync_errors == 1);
    assert(s.packets == 11);

    receiver_ts_stats_init(&s);
    unsigned char m2ts[192];
    memset(m2ts, 0, 4);
    packet(m2ts + 4, 100, 0, 1);
    assert(receiver_ts_stats_add(&s, m2ts, 192));
    assert(s.packets == 1 && s.sync_errors == 0);
    return 0;
}
