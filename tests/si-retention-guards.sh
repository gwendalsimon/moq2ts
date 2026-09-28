#!/usr/bin/env bash
set -euo pipefail

# Guards for the opt-in filtered-path enhancements (msfts#7 suggestions 1, 2, 4):
# SI-table retention, null-packet retention, and the advisory mux-rate hint.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CFG="$REPO_ROOT/src/app/PublishConfig.h"
MUXER="$REPO_ROOT/src/media/MsftsMuxer.cpp"
HDR="$REPO_ROOT/src/media/MsftsMuxer.h"
PKT="$REPO_ROOT/src/media/M2tsPacketizer.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# Opt-in toggles default OFF so historical filtered behavior is unchanged.
grep -q 'bool retainSiTables = false;' "$CFG" \
  || fail "PublishConfig must carry retainSiTables defaulting to false"
grep -q 'bool retainNullPackets = false;' "$CFG" \
  || fail "PublishConfig must carry retainNullPackets defaulting to false"
grep -q 'int mpeg2tsMuxRateBps = 0;' "$CFG" \
  || fail "PublishConfig must carry mpeg2tsMuxRateBps defaulting to 0"

# SI retention keeps the well-known DVB SI PIDs and is gated on the flag.
grep -q 'm_retainSiTables' "$PKT" \
  || fail "packetizer must honor m_retainSiTables"
grep -q '0x0011' "$PKT" \
  || fail "SI retention must include the SDT/BAT PID (0x0011)"

# Null retention is gated on the flag and targets the null PID.
grep -q 'm_retainNullPackets' "$PKT" \
  || fail "packetizer must honor m_retainNullPackets"
grep -q '0x1FFF' "$PKT" \
  || fail "null retention must target PID 0x1FFF"

# Mux-rate hint is threshold-gated (never emitted when 0).
grep -q 'catalog.mpeg2tsMuxRateBps > 0' "$MUXER" \
  || fail "mpeg2tsMuxRate must be emitted only when > 0"
grep -q 'mpeg2tsMuxRate' "$MUXER" \
  || fail "catalog must be able to emit mpeg2tsMuxRate"
grep -q 'qint64 mpeg2tsMuxRateBps' "$HDR" \
  || fail "MsftsCatalog must carry mpeg2tsMuxRateBps"

printf 'si-retention guards passed\n'
