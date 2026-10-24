#!/usr/bin/env bash
# Two-program SRT -> moq2ts publisher -> draft-22 relay -> moq5 receiver.
# Requires a draft-22 relay (a local moq5-relay by default, see e2e-relay.sh)
# and installed ffmpeg/ffprobe. RELAY_URL selects an existing relay instead.
set -euo pipefail

: "${MOQ2TS_CLI:?set MOQ2TS_CLI to the moq2ts-cli binary}"
: "${MOQ2TS_RECEIVER:?set MOQ2TS_RECEIVER to the moq2ts-receiver binary}"
SRT_PORT="${SRT_PORT:-9123}"
SECONDS_TO_RUN="${SECONDS_TO_RUN:-12}"
NAMESPACE="${NAMESPACE:-e2e/mux/$(date +%s)}"
DRAFT="${DRAFT:-22}"
work="$(mktemp -d)"
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
}
trap cleanup EXIT
# shellcheck source=tests/e2e-relay.sh
. "$(dirname "${BASH_SOURCE[0]}")/e2e-relay.sh"
e2e_relay_start "$work" || exit 1

ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=1000 \
  -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=500 \
  -map 0 -map 1 -map 2 -map 3 -c:v libx264 -preset ultrafast -g 25 -bf 0 -c:a aac \
  -program title=A:st=0:st=1 -program title=B:st=2:st=3 \
  -t "$((SECONDS_TO_RUN + 8))" -f mpegts "$work/source.ts"
remux=(-map 0 -c copy -program title=A:st=0:st=1 -program title=B:st=2:st=3 -f mpegts)
ffmpeg -hide_banner -loglevel error -y -i "$work/source.ts" "${remux[@]}" "$work/sent.ts"

cat > "$work/srt.json" <<EOF
{ "srt_callers": [ { "id": "e2e", "srt": { "mode": "caller", "host": "127.0.0.1", "port": $SRT_PORT, "latency_ms": 120 } } ] }
EOF

ffmpeg -hide_banner -loglevel error -re -i "$work/source.ts" "${remux[@]}" \
  "srt://127.0.0.1:$SRT_PORT?mode=listener&pkt_size=1316" > "$work/sender.log" 2>&1 &
pids+=($!)
sleep 1
"$MOQ2TS_CLI" --endpoint "$RELAY_URL" --namespace "$NAMESPACE" --srt-config "$work/srt.json" \
  --publish-multiplex --draft "$DRAFT" ${E2E_INSECURE_ARGS[@]+"${E2E_INSECURE_ARGS[@]}"} "$@" > "$work/publisher.log" 2>&1 &
pids+=($!)
sleep 2
"$MOQ2TS_RECEIVER" --url "$RELAY_URL" --namespace "$NAMESPACE" --draft "$DRAFT" \
  --duration "$SECONDS_TO_RUN" --insecure --out "$work/capture.ts" \
  --stats-json "$work/stats.json" > "$work/receiver.out" 2> "$work/receiver.log"

python3 - "$work/sent.ts" "$work/capture.ts" "$work/stats.json" <<'PY'
import json
import pathlib
import subprocess
import sys

sent = pathlib.Path(sys.argv[1]).read_bytes()
capture = pathlib.Path(sys.argv[2]).read_bytes()
stats = json.loads(pathlib.Path(sys.argv[3]).read_text())
assert len(capture) >= 188 * 100, f"too little media: {len(capture)} bytes"
assert len(capture) % 188 == 0, "capture is not packet aligned"
assert stats["bytes"] == len(capture) and stats["objects"] > 0, stats
assert stats["sync_errors"] == 0 and stats["malformed_packets"] == 0, stats
assert stats["transport_errors"] == 0 and stats["continuity_errors"] == 0, stats
offset = 0
for pos in range(0, len(capture), 188):
    packet = capture[pos:pos + 188]
    assert packet[0] == 0x47, f"bad sync at packet {pos // 188}"
    found = sent.find(packet, offset)
    assert found >= 0 and found % 188 == 0, f"packet {pos // 188} absent from source"
    offset = found + 188
print(f"captured {stats['objects']} objects, {len(capture) // 188} source-identical TS packets; "
      f"MOQT gaps reported: {stats['moqt_gaps']}")
probe = json.loads(subprocess.check_output([
    "ffprobe", "-v", "error", "-show_entries", "program=program_id",
    "-of", "json", sys.argv[2]
]))
assert len(probe.get("programs", [])) == 2, probe
PY

echo "artifacts in $work"
