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
PIPE="$REPO_ROOT/src/media/LivePipeline.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# Opt-in toggles default OFF so historical filtered behavior is unchanged.
grep -q 'QSet<SiTable> siTables;' "$CFG" \
  || fail "PublishConfig must carry the SI tables to keep, none by default"
grep -q 'retainSiTables' "$CFG" \
  && fail "PublishConfig must not carry retainSiTables: --retain-si selects every table" || true
grep -q 'bool retainNullPackets = false;' "$CFG" \
  || fail "PublishConfig must carry retainNullPackets defaulting to false"
grep -q 'int mpeg2tsMuxRateBps = 0;' "$CFG" \
  || fail "PublishConfig must carry mpeg2tsMuxRateBps defaulting to 0"

# SI retention keeps the well-known DVB SI PIDs and is gated on the flag:
# --retain-si selects every SI table and its absence none, the pipeline passes
# the selection on, and each SI PID is kept only when one of its tables is
# selected.
grep -q 'setSiTables(retain ? allSiTables() : QSet<SiTable>());' "$PKT" \
  || fail "setRetainSiTables must select every SI table when on, and none when off"
grep -q 'cfg.siTables = moq2ts::allSiTables();' "$REPO_ROOT/src/cli/main_cli.cpp" \
  || fail "--retain-si must select every SI table at parse time"
grep -q 'packetizer.setSiTables(m_config.siTables);' "$PIPE" \
  || fail "LivePipeline must pass the SI tables to the packetizer"
grep -q 'keepPid(0x0011, m_siTables.contains(SiTable::Sdt) || m_siTables.contains(SiTable::Bat));' "$PKT" \
  || fail "SI retention must keep the SDT/BAT PID (0x0011) only when the SDT or the BAT is selected"

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
