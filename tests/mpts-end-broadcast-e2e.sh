#!/usr/bin/env bash
# A two-program FIFO source that ends -> moq2ts-cli --config -> relay -> one
# moq2ts-receiver per program. When the source ends cleanly the publisher ends
# the broadcast with the closing catalog (MSF -01 Section 11.3), and moqxr
# ends every track before it. Each receiver must report "[rx] track ended" long
# before its own --duration runs out, with every program's packets intact.
# Exits non-zero otherwise.
# Requires a draft-22 relay (a local moq5-relay by default, see e2e-relay.sh),
# ffmpeg, and python3.
#
# Environment:
#   RELAY_URL         an existing relay, e.g. moqt://127.0.0.1:4433; without it
#                     a local relay starts from MOQ5_RELAY (see e2e-relay.sh)
#   MOQ2TS_CLI        the moq2ts-cli binary (required)
#   MOQ2TS_RECEIVER   the moq2ts-receiver binary (required)
#   SOURCE_SECONDS    length of the source in seconds (default 15)
#   RX_DURATION       upper bound of each receiver, in seconds (default 90)
#   DRAFT             MOQT draft of the publisher and receiver, 22 or 18 (default 22)
#   NAMESPACE         MOQT namespace (default e2e/mpts-end/<time>)
set -euo pipefail

: "${MOQ2TS_CLI:?set MOQ2TS_CLI to the moq2ts-cli binary}"
: "${MOQ2TS_RECEIVER:?set MOQ2TS_RECEIVER to the moq2ts-receiver binary}"
SOURCE_SECONDS="${SOURCE_SECONDS:-15}"
RX_DURATION="${RX_DURATION:-90}"
NAMESPACE="${NAMESPACE:-e2e/mpts-end/$(date +%s)}"
DRAFT="${DRAFT:-22}"
PROGRAMS=(101 102)
MUX_RATE=3000000
# The receivers start about 4 s after the source and must end within this many
# seconds of it: the source length plus a margin, far below RX_DURATION.
END_WITHIN=$((SOURCE_SECONDS + 25))
for tool in ffmpeg python3 timeout mkfifo; do
  command -v "$tool" > /dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done

work="$(mktemp -d)"
pids=()
ok=0
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  if [ "$ok" -eq 1 ]; then rm -rf "$work"; else echo "artifacts kept in $work" >&2; fi
}
trap cleanup EXIT
# shellcheck source=tests/e2e-relay.sh
. "$(dirname "${BASH_SOURCE[0]}")/e2e-relay.sh"
e2e_relay_start "$work" || exit 1
fail() {
  echo "FAIL: $*" >&2
  for log in publisher sender; do
    echo "--- last lines of $log.log" >&2
    tail -n 8 "$work/$log.log" >&2 || true
  done
  exit 1
}

# Two programs, each with its own video and audio, in one CBR multiplex.
timeout 120 ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=1000 \
  -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=500 \
  -map 0 -map 1 -map 2 -map 3 -c:v libx264 -preset ultrafast -g 25 -bf 0 -b:v 600k \
  -c:a aac -b:a 64k \
  -program program_num=101:title=A:st=0:st=1 -program program_num=102:title=B:st=2:st=3 \
  -muxrate "$MUX_RATE" -t "$((SOURCE_SECONDS + 10))" -f mpegts "$work/source.ts"

mkfifo "$work/live.fifo"
cat > "$work/mpts.json" <<JSON
{
  "relay": { "endpoint": "$RELAY_URL", "namespace": "$NAMESPACE", "draft": $DRAFT, "insecure": $E2E_INSECURE },
  "ingest": { "path": "$work/live.fifo" },
  "programs": [ { "number": ${PROGRAMS[0]} }, { "number": ${PROGRAMS[1]} } ]
}
JSON

"$MOQ2TS_CLI" --config "$work/mpts.json" > "$work/publisher.log" 2>&1 &
pids+=($!)
publisher=$!
# The source ends after SOURCE_SECONDS: ffmpeg exits and closes the FIFO, which
# is a clean end of the source for the publisher.
ffmpeg -hide_banner -loglevel error -y -re -i "$work/source.ts" -t "$SOURCE_SECONDS" -map 0 -c copy \
  -program program_num=101:title=A:st=0:st=1 -program program_num=102:title=B:st=2:st=3 \
  -muxrate "$MUX_RATE" -f mpegts "$work/live.fifo" > "$work/sender.log" 2>&1 &
pids+=($!)
sender=$!
sleep 4
kill -0 "$publisher" 2>/dev/null || fail "the publisher exited before the receivers started"
kill -0 "$sender" 2>/dev/null || fail "the sender exited before the receivers started"

started=$SECONDS
rx=()
for n in "${PROGRAMS[@]}"; do
  timeout "$((RX_DURATION + 30))" "$MOQ2TS_RECEIVER" --url "$RELAY_URL" --namespace "$NAMESPACE" --draft "$DRAFT" \
    --track "program-$n" --duration "$RX_DURATION" --insecure --out "$work/program-$n.ts" \
    --stats-json "$work/program-$n.json" > "$work/receiver-$n.out" 2> "$work/receiver-$n.log" &
  rx+=($!)
  pids+=($!)
done
status=0
for pid in "${rx[@]}"; do wait "$pid" || status=1; done
elapsed=$((SECONDS - started))
[ "$status" -eq 0 ] || { tail -n 5 "$work"/receiver-*.log >&2; fail "a receiver failed or timed out"; }

for n in "${PROGRAMS[@]}"; do
  grep -q '^\[rx\] track ended' "$work/receiver-$n.log" ||
    fail "program $n: the receiver did not see the broadcast end (ran $elapsed s of $RX_DURATION s)"
done
[ "$elapsed" -le "$END_WITHIN" ] ||
  fail "the receivers ended after $elapsed s, want at most $END_WITHIN s for a $SOURCE_SECONDS s source"

timeout 60 python3 - "$work" "${PROGRAMS[@]}" <<'PY' || fail "a capture is not a clean program"
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
for program in (int(p) for p in sys.argv[2:]):
    data = (work / f"program-{program}.ts").read_bytes()
    stats = json.loads((work / f"program-{program}.json").read_text())
    assert len(data) >= 188 * 100 and len(data) % 188 == 0, f"program {program}: {len(data)} bytes"
    assert stats["bytes"] == len(data) and stats["objects"] > 0, stats
    assert stats["sync_errors"] == 0 and stats["malformed_packets"] == 0, stats
    assert stats["transport_errors"] == 0 and stats["continuity_errors"] == 0, stats
    print(f"program {program}: ended the broadcast cleanly after {stats['objects']} objects, "
          f"{len(data) // 188} packets, continuity errors 0")
PY

echo "both receivers saw the broadcast end after $elapsed s (source $SOURCE_SECONDS s, receiver limit $RX_DURATION s)"
ok=1
