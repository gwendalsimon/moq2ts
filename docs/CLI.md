# moq2ts headless CLI (`moq2ts-cli`)

`moq2ts-cli` is a headless MSFTS (MPEG-TS over MOQ) publisher. It reuses the same
streaming core as the GUI but runs under `QCoreApplication`, so it needs **no
display server** and is intended for Linux/server deployment near an encoder.

It reads a **seekable `.ts` file**, a **live stream** (named FIFO or
`/dev/stdin`), or a **live SRT feed**, and publishes `draft-gregoire-moq-msfts`
objects to a MOQ relay.

## Build

Requirements: CMake 3.24+, a C++20 compiler, Qt 6 (or Qt 5) Core/Gui/Widgets, and
pkg-config. `libsrt` is needed for SRT ingest and FFmpeg's libav* for capture and
duration probing; both are found through pkg-config and the build degrades
gracefully if they are absent. The real-relay build additionally needs OpenSSL.

FFmpeg's libavformat loads the GnuTLS build of libsrt (`libsrt-gnutls`), so the
build links that same library when it is installed, and configures moqxr without
its own SRT ingest, which moq2ts does not use. Exactly one libsrt is then loaded:
two copies export the same symbols and crash the process at exit. `ldd
build/moq2ts-cli | grep srt` should list only `libsrt-gnutls`.

On Debian/Ubuntu:

```bash
sudo apt install cmake g++ pkg-config qt6-base-dev libsrt-openssl-dev \
                 libavcodec-dev libavformat-dev libavutil-dev libavdevice-dev \
                 libswresample-dev libswscale-dev libssl-dev
```

The CLI is built alongside the GUI. Two build flavors:

```bash
# Mock build: no relay and no moqxr SDK, accepts only mock:// endpoints.
# Self-contained, so this is the quickest way to check the tree compiles.
cmake -S . -B build-mock -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=ON
cmake --build build-mock -j

# Real relay build: needs the moqxr publisher SDK. Point at either a prebuilt
# SDK directory (containing include/ and lib/*.a) ...
cmake -S . -B build -DMOQXR_SDK_DIR=/path/to/openmoq-publisher-sdk
cmake --build build -j

# ... or a moqxr source checkout, which is built as a subproject.
cmake -S . -B build -DMOQXR_SOURCE_DIR=/path/to/moqxr
cmake --build build -j
```

One of `MOQXR_SDK_DIR`, `MOQXR_SOURCE_DIR` or `MOQ2TS_BUILD_WITH_MOCK_MOQXR=ON`
must be given. Configuring with none of them fails with "moqxr publish headers not
found", since there is no default location to fall back to.

Binaries: `build/moq2ts-cli` (real) or `build-mock/moq2ts-cli` (mock).

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--endpoint <url>` | `mock://local` | MOQ relay endpoint (must be `mock://...` in mock builds) |
| `--namespace <ns>` | `live/ch1` | MOQ track namespace |
| `--video <path>` | - | TS/M2TS source: seekable file, FIFO, or `/dev/stdin` |
| `--audio <path>` | - | Alternate single-stream TS source path |
| `--srt-config <path>` | - | SRT ingest: JSON caller config (see below). Takes the place of `--video`, and implies `--unmodified` |
| `--camera <id>` / `--mic <id>` | - | Capture device ids (instead of a TS source) |
| `--program <n>` | `0` | Per-program: the MPEG program to carry (0 = first). Unmodified multiplex: the reference program that the catalog names (0 = none) |
| `--unmodified` | off | Unmodified carriage: forward every source packet unchanged (no PID filter or rewrite). The catalog carries the source PAT and PMT as initialization data. The track is `unmodified-program` for a single-program source and `unmodified-multiplex` otherwise; without a PAT the track is refused |
| `--transparent`, `--publish-multiplex` | off | Aliases of `--unmodified`; `--transparent` is kept for existing scripts |
| `--retain-si` | off | Per-program mode: also keep DVB SI PIDs (NIT/SDT/EIT/TDT-TOT). The SDT and the EIT keep the carried service only; the NIT, BAT, TDT, and TOT pass unchanged |
| `--retain-null` | off | Per-program mode: also keep null (0x1FFF) packets |
| `--mux-rate <bps>` | `0` | Advisory source mux rate in bits/s. With 0, a per-program track of a single-program source without `--retain-null` measures the rate from the PCR when the source carries null packets, and a warning says when no rate is declared. The draft forbids `mpeg2tsMuxRate` in `unmodified-multiplex`, so `--unmodified` and SRT ingest drop it when the source PAT lists several programs, and warn |
| `--fragment-ms <ms>` | `250` | Group cadence |
| `--segment-bytes <n>` | `65536` | Target object size |
| `--draft <n>` | `16` | MOQ draft version: 14, 16, 17 or 18. The relay must list it in `moqt_versions`; any other value is refused |
| `--paced` | off | File sources only: pace publishing at media rate instead of as fast as possible |
| `--width/--height/--fps/--video-bitrate` | 1920/1080/30/2500 | Capture params |
| `--sample-rate/--channels/--audio-bitrate/--audio-codec` | 48000/2/160/aac | Audio params |

`Ctrl-C` (SIGINT) or SIGTERM stops gracefully. Exit code 0 on clean completion, 1
on error, 2 on invalid arguments.

## Run: seekable file

```bash
# Per-program carriage (default behavior) with a mux-rate hint.
./build-mock/moq2ts-cli --endpoint mock://local --namespace live/ch1 \
    --video sample.ts --program 1 --mux-rate 38000000

# Unmodified carriage of every source packet.
./build-mock/moq2ts-cli --endpoint mock://local --namespace live/ch1 \
    --unmodified --video sample.ts
```

## Run: live feed over SRT

`moq2ts-cli` connects out as an SRT **caller**, so the encoder side runs an SRT
**listener**. Point `--srt-config` at a JSON file in moqxr's format; the first
entry in `srt_callers` is used. `--video` is not needed, the SRT feed is the
source.

```json
{
  "srt_callers": [
    {
      "id": "encoder1",
      "srt": {
        "mode": "caller",
        "host": "127.0.0.1",
        "port": 9000,
        "latency_ms": 200,
        "rcvbuf_bytes": 8388608,
        "udp_rcvbuf_bytes": 8388608
      }
    }
  ]
}
```

```bash
# Encoder side: ffmpeg listens for the caller and emits MPEG-TS.
ffmpeg -re -i input.ts -c copy -f mpegts \
  "srt://0.0.0.0:9000?mode=listener&pkt_size=1316" &

# Publisher side: connect, ingest, publish.
./build/moq2ts-cli --endpoint <relay-url> --namespace live/ch1 \
    --srt-config ./srt_callers.json --unmodified
```

`pkt_size=1316` is worth keeping: 1316 = 7 x 188, so each SRT payload holds a
whole number of TS packets and no packet straddles a datagram boundary. The
publisher reads the SRT payloads as one byte stream and passes on whole packets
only, so a sender whose payloads do not end on a packet boundary still works.
`latency_ms` sets SRT's retransmit buffer, which absorbs network jitter on the
ingest side before the publisher ever sees the bytes.

**SRT ingest always uses unmodified carriage** (`--publish-multiplex` says so
explicitly). A contribution feed is forwarded as received, so the per-program
options (`--retain-si`, `--retain-null`) do not apply and are ignored; passing
them prints a warning. The catalog drops `mpeg2tsMuxRate` when the source PAT
lists several programs, but `--mux-rate` still bounds the catalog `bitrate`
(see [DEVELOPER.md](DEVELOPER.md)). Use a file or FIFO source if you need
per-program publishing.

A two-program test feed, as used to test this path. ffmpeg encodes both programs
in real time and listens for the publisher on SRT; `-muxrate` pads the multiplex
to a constant rate with null packets, as a contribution encoder does:

```bash
ffmpeg -hide_banner -re \
  -f lavfi -i "testsrc2=s=640x360:r=25" -f lavfi -i "sine=f=440:r=48000" \
  -f lavfi -i "testsrc=s=640x360:r=25"  -f lavfi -i "sine=f=880:r=48000" \
  -map 0:v -map 1:a -map 2:v -map 3:a \
  -c:v libx264 -preset ultrafast -g 50 -keyint_min 50 -sc_threshold 0 \
  -b:v:0 2000k -maxrate:v:0 2000k -bufsize:v:0 2000k \
  -b:v:1 1000k -maxrate:v:1 1000k -bufsize:v:1 1000k \
  -c:a aac -b:a 96k \
  -program program_num=1:title=ProgOne:st=0:st=1 \
  -program program_num=2:title=ProgTwo:st=2:st=3 \
  -muxrate 6000000 -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

`-g 50` at 25 fps gives a random access point every 2 s, the longest Group that
MSFTS recommends. To keep a byte-for-byte copy of what was sent, see the
end-to-end test in [tests/srt-mpts-e2e.sh](../tests/srt-mpts-e2e.sh).

A multi-program feed is published as one `unmodified-multiplex` track:

| | Without `--program` | With `--program N` |
|---|---|---|
| Track name | `mux-1` | `mux-1-pN` |
| `mpeg2tsProgramNumber`, `mpeg2tsPcrPid` | absent | program N and its PCR PID |
| `mpeg2tsRandomAccess` | absent | true when Groups start at random access points of N |
| initialization data | the PAT and the PMT of every program | the PAT and the PMT of N |
| a program leaves the PAT | the track goes on | the track ends when N leaves |
| the PAT lists one program | the track ends | the track ends |

The track ends because the draft forbids `unmodified-multiplex` for a
one-program PAT and requires a new track for a new program number. The
initialization data is declared only when every PMT arrived at the start; it is
never added later.

The SRT receive buffer holds about 5 s. While nothing reads it, for example
while the publisher waits for its first subscriber, it keeps the newest media
and drops the oldest. The stream then resumes at a random access point of the
PID that starts Groups, and the gap falls on a Group boundary, never inside a
Group (draft "PCR and Timing"). A gap before the first Object is not published
at all: the track starts at the live edge, and the data read before the gap is
dropped with a warning.

## Run: live feed from ffmpeg (server / near-encoder)

### Via a named FIFO

`-y` is required on every ffmpeg command that writes to the FIFO: `mkfifo` has
already created the path, and without it ffmpeg prompts to overwrite and exits.

```bash
mkfifo /tmp/live.ts

# ffmpeg produces a continuous MPEG-TS into the FIFO (test source shown; swap in
# your real input with -i). Unmodified carriage is the byte-faithful path.
ffmpeg -re -y \
  -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
  -c:v libx264 -b:v 4M -c:a aac -b:a 128k \
  -f mpegts -muxrate 6M -pcr_period 20 /tmp/live.ts &

./build/moq2ts-cli --endpoint <relay-url> --namespace live/ch1 \
    --unmodified --video /tmp/live.ts
```

### Via a stdin pipe

```bash
ffmpeg -re -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
  -c:v libx264 -b:v 4M -c:a aac -f mpegts -muxrate 6M - \
  | ./build/moq2ts-cli --endpoint <relay-url> --namespace live/ch1 \
        --unmodified --video /dev/stdin
```

## Verify byte-faithful passthrough

For a file source with unmodified carriage the published payload must equal the input
byte-for-byte. Under the mock build, the mock publisher logs objects to stderr;
for a fidelity check, capture the emitted payloads and `cmp` against the source
`.ts`. The catalog JSON should contain
`"mpeg2tsMode":"unmodified-program"` when the source PAT lists one program,
and `"mpeg2tsMode":"unmodified-multiplex"` otherwise. Neither contains
`mpeg2tsSiPids`. Both carry the source PAT and PMT in a root `initDataList`:
the one PMT of an `unmodified-program`, and for an `unmodified-multiplex` the
PMT of its reference program (`--program`), or of every program without one.
An `unmodified-multiplex` catalog names its reference program in
`mpeg2tsProgramNumber` and `mpeg2tsPcrPid` only with `--program`, and never
contains `mpeg2tsMuxRate`.

In per-program mode the PAT/PMT bootstrap (with the rewritten PAT) is carried the way MSF-01 defines it: the
track gets an `initRef` string, and the bytes live in a root `initDataList` entry
of type `inline`. The older MSF-00 spelling put a base64 `initData` field on the
track itself; catalogs in that shape are still parsed on the receive side, but are
no longer produced.

## Notes

- **Start-up look-ahead.** Before a live track starts, moq2ts may read ahead
  in the source: up to 20,000 packets for the first random access point, and
  up to one second of PCR time to measure the mux rate. On a live source this
  delays the start by up to a few seconds. The packets read are published.

- **Live stream = live catalog.** A non-seekable source (FIFO/stdin) is detected
  automatically and advertised as `isLive: true`; VOD duration probing is skipped
  (it would otherwise open and consume the pipe a second time).
- **Per-program mode over a pipe** works too: the PAT/PMT init scan is buffered and
  replayed so no leading packets are lost. If the source never emits PAT/PMT within
  the first ~4096 packets, per-program init fails with a clear error - use
  `--unmodified` for such feeds.
