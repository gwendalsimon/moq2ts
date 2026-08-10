#!/usr/bin/env bash
set -euo pipefail

# Guards for the transparent/whole-multiplex carriage profile (msfts#7 suggestion 3).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CFG="$REPO_ROOT/src/app/PublishConfig.h"
MUXER="$REPO_ROOT/src/media/MsftsMuxer.cpp"
HDR="$REPO_ROOT/src/media/MsftsMuxer.h"
PKT="$REPO_ROOT/src/media/M2tsPacketizer.cpp"
PIPE="$REPO_ROOT/src/media/LivePipeline.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# Config toggle exists and defaults to the historical filtered behavior.
grep -q 'bool transparentMode = false;' "$CFG" \
  || fail "PublishConfig must carry transparentMode defaulting to false"

# Catalog carries and gates the whole-multiplex flag (draft name: m2tsMpts).
grep -q 'bool m2tsMpts' "$HDR" \
  || fail "MsftsCatalog must carry m2tsMpts"
grep -q 'catalog.m2tsMpts' "$MUXER" \
  || fail "m2tsMpts emission must be gated on catalog.m2tsMpts"
grep -q '"m2tsMpts"' "$MUXER" \
  || fail "catalog must emit the draft-conformant m2tsMpts key"
# The legacy m2tsTransparent spelling stays accepted on parse for interop.
grep -q '"m2tsTransparent"' "$MUXER" \
  || fail "catalogFromJson must still accept the legacy m2tsTransparent key"
# Per-program identifiers remain available for the non-transparent branch.
grep -q 'm2tsProgramNumber' "$MUXER" \
  || fail "filtered path must still emit m2tsProgramNumber"

# Packetizer bypasses PAT/PMT parse and PID filtering when transparent.
grep -q 'if (!m_transparent)' "$PKT" \
  || fail "packetizer must guard collectInitData/filtering on !m_transparent"
grep -q 'setTransparent' "$PKT" \
  || fail "packetizer must expose setTransparent"

# Pipeline wires the packetizer and catalog for transparent mode.
grep -q 'setTransparent(m_config.transparentMode)' "$PIPE" \
  || fail "pipeline must pass transparentMode into the packetizer"
grep -q '.m2tsMpts = m_config.transparentMode' "$PIPE" \
  || fail "pipeline must set catalog m2tsMpts from config"

printf 'transparent-mode guards passed\n'
