#!/usr/bin/env bash
# Two-program SRT -> moq2ts-cli --config -> relay -> one moq2ts-receiver per program.
# ffmpeg pushes a CBR MPTS (programs 101 and 102) to the SRT listener of moq2ts-cli.
# Each program is published as the per-program track program-<n> (MSFTS 5.3); the
# script checks that each capture holds only its program's PIDs plus the PAT and PMT,
# every PID of the program, and no continuity error. Exits non-zero otherwise.
# Requires a draft-22 relay (a local moq5-relay by default, see e2e-relay.sh),
# ffmpeg, ffprobe, and python3.
#
# Environment:
#   RELAY_URL         an existing relay, e.g. moqt://127.0.0.1:4433; without it
#                     a local relay starts from MOQ5_RELAY (see e2e-relay.sh)
#   MOQ2TS_CLI        the moq2ts-cli binary (required)
#   MOQ2TS_RECEIVER   the moq2ts-receiver binary (required)
#   SRT_PORT          SRT listener port of moq2ts-cli (default 9124)
#   SRT_PAYLOAD       SRT payload size in bytes, a multiple of 188 (default 1316);
#                     lower it, e.g. to 940, where large loopback datagrams are lost
#   SECONDS_TO_RUN    receive duration in seconds (default 25)
#   DRAFT             MOQT draft of the publisher and receiver, 22 or 18 (default 22)
#   NAMESPACE         MOQT namespace (default e2e/mpts/<time>)
set -euo pipefail

: "${MOQ2TS_CLI:?set MOQ2TS_CLI to the moq2ts-cli binary}"
: "${MOQ2TS_RECEIVER:?set MOQ2TS_RECEIVER to the moq2ts-receiver binary}"
SRT_PORT="${SRT_PORT:-9124}"
SRT_PAYLOAD="${SRT_PAYLOAD:-1316}"
SECONDS_TO_RUN="${SECONDS_TO_RUN:-25}"
NAMESPACE="${NAMESPACE:-e2e/mpts/$(date +%s)}"
DRAFT="${DRAFT:-22}"
PROGRAMS=(101 102)
MUX_RATE=3000000
for tool in ffmpeg ffprobe python3 timeout; do
  command -v "$tool" > /dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
if ! [[ "$SRT_PAYLOAD" =~ ^[1-9][0-9]*$ ]] || [ $((SRT_PAYLOAD % 188)) -ne 0 ]; then
  echo "SRT_PAYLOAD must be a positive multiple of 188, got '$SRT_PAYLOAD'" >&2
  exit 1
fi

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
# Print the tail of the publisher and sender logs, then fail.
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
  -muxrate "$MUX_RATE" -t "$((SECONDS_TO_RUN + 10))" -f mpegts "$work/source.ts"

cat > "$work/mpts.json" <<JSON
{
  "relay": { "endpoint": "$RELAY_URL", "namespace": "$NAMESPACE", "draft": $DRAFT, "insecure": $E2E_INSECURE },
  "ingest": { "srt": { "mode": "listener", "host": "127.0.0.1", "port": $SRT_PORT, "latency_ms": 200 } },
  "programs": [ { "number": ${PROGRAMS[0]} }, { "number": ${PROGRAMS[1]} } ]
}
JSON

"$MOQ2TS_CLI" --config "$work/mpts.json" > "$work/publisher.log" 2>&1 &
pids+=($!)
publisher=$!
sleep 2
kill -0 "$publisher" 2>/dev/null || fail "the publisher exited at start-up"
ffmpeg -hide_banner -loglevel error -re -i "$work/source.ts" -map 0 -c copy \
  -program program_num=101:title=A:st=0:st=1 -program program_num=102:title=B:st=2:st=3 \
  -muxrate "$MUX_RATE" -f mpegts \
  "srt://127.0.0.1:$SRT_PORT?mode=caller&pkt_size=$SRT_PAYLOAD" > "$work/sender.log" 2>&1 &
pids+=($!)
sender=$!
sleep 3
kill -0 "$publisher" 2>/dev/null || fail "the publisher exited before the receivers started"
kill -0 "$sender" 2>/dev/null || fail "the sender exited before the receivers started"

rx=()
for n in "${PROGRAMS[@]}"; do
  timeout "$((SECONDS_TO_RUN + 30))" "$MOQ2TS_RECEIVER" --url "$RELAY_URL" --namespace "$NAMESPACE" --draft "$DRAFT" --track "program-$n" \
    --duration "$SECONDS_TO_RUN" --insecure --out "$work/program-$n.ts" \
    --stats-json "$work/program-$n.json" > "$work/receiver-$n.out" 2> "$work/receiver-$n.log" &
  rx+=($!)
  pids+=($!)
done
status=0
for pid in "${rx[@]}"; do wait "$pid" || status=1; done
[ "$status" -eq 0 ] || { tail -n 5 "$work"/receiver-*.log >&2; fail "a receiver failed or timed out"; }
kill -0 "$publisher" 2>/dev/null || fail "the publisher exited during the run"

rc=0
timeout 120 python3 - "$work" "${PROGRAMS[@]}" <<'PY' || rc=$?
import json
import pathlib
import subprocess
import sys

work = pathlib.Path(sys.argv[1])
programs = [int(p) for p in sys.argv[2:]]

def packets(path):
    data = path.read_bytes()
    assert len(data) % 188 == 0, f"{path.name}: not packet aligned"
    for pos in range(0, len(data), 188):
        pkt = data[pos:pos + 188]
        assert pkt[0] == 0x47, f"{path.name}: bad sync at packet {pos // 188}"
        yield pkt

def pid_of(pkt):
    return ((pkt[1] & 0x1F) << 8) | pkt[2]

def section(path, pid):
    """First PSI section on pid. Handles single-packet PAT/PMT sections only."""
    for pkt in packets(path):
        if pid_of(pkt) == pid and pkt[1] & 0x40:
            off = 4
            if pkt[3] & 0x20:
                off += 1 + pkt[4]
            payload = pkt[off:]
            return payload[1 + payload[0]:]
    raise AssertionError(f"{path.name}: no section on PID {pid:#x}")

def expected_pids(path, number):
    pat = section(path, 0)
    length = ((pat[1] & 0x0F) << 8) | pat[2]
    entries = pat[8:3 + length - 4]
    pmt_pid = None
    for i in range(0, len(entries), 4):
        if (entries[i] << 8 | entries[i + 1]) == number:
            pmt_pid = ((entries[i + 2] & 0x1F) << 8) | entries[i + 3]
    assert pmt_pid is not None, f"program {number} not in the source PAT"
    pmt = section(path, pmt_pid)
    length = ((pmt[1] & 0x0F) << 8) | pmt[2]
    end = 3 + length - 4
    pcr = ((pmt[8] & 0x1F) << 8) | pmt[9]
    pos = 12 + (((pmt[10] & 0x0F) << 8) | pmt[11])
    es = set()
    while pos < end:
        es.add(((pmt[pos + 1] & 0x1F) << 8) | pmt[pos + 2])
        pos += 5 + (((pmt[pos + 3] & 0x0F) << 8) | pmt[pos + 4])
    return pmt_pid, es | {pcr}

def continuity_errors(path):
    last, errors = {}, 0
    for pkt in packets(path):
        pid = pid_of(pkt)
        afc = (pkt[3] >> 4) & 3
        if pid == 0x1FFF or not afc & 1:
            continue
        cc = pkt[3] & 0x0F
        discont = afc & 2 and pkt[4] > 0 and pkt[5] & 0x80
        if pid in last and not discont and cc != (last[pid] + 1) % 16 and cc != last[pid]:
            errors += 1
        last[pid] = cc
    return errors

source = work / "source.ts"
failed = False
for n in programs:
    capture = work / f"program-{n}.ts"
    problems = []
    summary = ""
    try:
        try:
            stats = json.loads((work / f"program-{n}.json").read_text())
            for key in ("objects", "bytes", "continuity_errors", "sync_errors",
                        "malformed_packets", "transport_errors"):
                assert key in stats, f"stats key {key} missing"
        except (OSError, ValueError, AssertionError) as err:
            raise AssertionError(f"no usable stats JSON: {err}")
        pmt_pid, es = expected_pids(source, n)
        allowed = {0, pmt_pid} | es
        seen = {pid_of(p) for p in packets(capture)}
        cc = continuity_errors(capture)
        try:
            probe = json.loads(subprocess.check_output([
                "ffprobe", "-v", "error", "-show_entries", "program=program_id",
                "-of", "json", str(capture)], stderr=subprocess.DEVNULL, timeout=30))
            probed = [p["program_id"] for p in probe.get("programs", [])]
        except (subprocess.SubprocessError, ValueError) as err:
            probed = f"ffprobe failed ({err})"
        if stats["objects"] == 0 or stats["bytes"] != capture.stat().st_size:
            problems.append(f"stats do not match the capture: {stats}")
        if capture.stat().st_size < 188 * 1000:
            problems.append("too little media")
        if seen - allowed:
            problems.append(f"foreign PIDs {sorted(hex(p) for p in seen - allowed)}")
        if allowed - seen:
            problems.append(f"missing PIDs {sorted(hex(p) for p in allowed - seen)}")
        if cc or stats["continuity_errors"]:
            problems.append(f"continuity errors: {cc} counted, {stats['continuity_errors']} reported")
        if stats["sync_errors"] or stats["malformed_packets"] or stats["transport_errors"]:
            problems.append(f"packet errors: {stats}")
        if probed != [n]:
            problems.append(f"ffprobe programs {probed}, expected [{n}]")
        pidlist = " ".join(f"{p:#06x}" for p in sorted(seen))
        summary = (f": {stats['objects']} objects, {capture.stat().st_size // 188} "
                   f"packets, PIDs {pidlist}, continuity errors {cc}")
    except (AssertionError, OSError) as err:
        problems.append(str(err))
    verdict = "FAIL " + "; ".join(problems) if problems else "PASS"
    print(f"program {n}: {verdict}{summary}")
    failed |= bool(problems)
sys.exit(1 if failed else 0)
PY
[ "$rc" -eq 0 ] && ok=1
exit "$rc"
