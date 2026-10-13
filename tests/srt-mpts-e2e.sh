#!/usr/bin/env bash
# End-to-end check of SRT ingest and unmodified-multiplex carriage (draft
# "Unmodified Multiplex"): a two-program feed goes over SRT into moq2ts-cli,
# through a MOQ relay, out of moq2ts-gateway, and the capture is compared byte
# for byte with what was sent. Needs a running relay, so it is run by hand:
#
#   RELAY_URL=moqt://127.0.0.1:4433 \
#   MOQ2TS_CLI=build/moq2ts-cli \
#   GATEWAY=../moq2ts-receivers/moq2ts-gateway/build/moq2ts-gateway \
#   VALIDATOR=../moq2ts-receivers/validate-ts.py \
#   tests/srt-mpts-e2e.sh [--program N]
#
# With --program N the publisher names N as the reference program, and the
# track is mux-1-pN instead of mux-1.
set -euo pipefail

: "${RELAY_URL:?set RELAY_URL to the relay, e.g. moqt://127.0.0.1:4433}"
: "${MOQ2TS_CLI:?set MOQ2TS_CLI to the moq2ts-cli binary}"
: "${GATEWAY:?set GATEWAY to the moq2ts-gateway binary built with moxygen}"
: "${VALIDATOR:?set VALIDATOR to validate-ts.py}"
SRT_PORT="${SRT_PORT:-9123}"
SECONDS_TO_RUN="${SECONDS_TO_RUN:-20}"
NAMESPACE="${NAMESPACE:-e2e/mux}"
PROGRAM_ARGS=("$@")

work="$(mktemp -d)"
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
}
trap cleanup EXIT

# A two-program source. The SRT sender remuxes it; the same remux written to a
# file is byte-identical, because the mpegts muxer is deterministic, and is
# the reference the capture is compared with.
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=1000 \
  -f lavfi -i testsrc=size=640x360:rate=25 -f lavfi -i sine=frequency=500 \
  -map 0 -map 1 -map 2 -map 3 -c:v libx264 -g 25 -bf 0 -c:a aac \
  -program title=A:st=0:st=1 -program title=B:st=2:st=3 \
  -t "$((SECONDS_TO_RUN + 10))" -f mpegts "$work/source.ts"
remux=(-map 0 -c copy -program title=A:st=0:st=1 -program title=B:st=2:st=3 -f mpegts)
ffmpeg -hide_banner -loglevel error -y -i "$work/source.ts" "${remux[@]}" "$work/sent.ts"

cat > "$work/srt.json" <<EOF
{ "srt_callers": [ { "id": "e2e", "srt": { "mode": "caller", "host": "127.0.0.1", "port": $SRT_PORT, "latency_ms": 120 } } ] }
EOF

ffmpeg -hide_banner -loglevel error -re -i "$work/source.ts" "${remux[@]}" \
  "srt://127.0.0.1:$SRT_PORT?mode=listener&pkt_size=1316" &
pids+=($!)
sleep 1
"$MOQ2TS_CLI" --endpoint "$RELAY_URL" --namespace "$NAMESPACE" --srt-config "$work/srt.json" \
  --publish-multiplex "${PROGRAM_ARGS[@]}" > "$work/publisher.log" 2>&1 &
pids+=($!)
sleep 3
# No --track: the gateway takes the unmodified-multiplex track from the catalog.
timeout -s INT "$SECONDS_TO_RUN" "$GATEWAY" --url "$RELAY_URL" --namespace "$NAMESPACE" --insecure \
  --out "$work/capture.ts" --catalog-out "$work/catalog.json" --stats-json "$work/stats.json" \
  2> "$work/gateway.log" || true

grep -E '\[gw\] (track|reference|no reference|stream|catalog|gap|warning)' "$work/gateway.log" || true
python3 "$VALIDATOR" "$work/capture.ts" --no-tsduck --catalog "$work/catalog.json" \
  --compare "$work/sent.ts" | sed -n '/=== Catalog/,$p'
echo "artifacts in $work"
