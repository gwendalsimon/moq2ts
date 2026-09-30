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
| `--srt-config <path>` | - | SRT ingest: JSON caller config (see below). Takes the place of `--video`, and implies `--transparent` |
| `--camera <id>` / `--mic <id>` | - | Capture device ids (instead of a TS source) |
| `--program <n>` | `0` | MPEG program to select (0 = first); ignored with `--transparent` |
| `--transparent` | off | Carry the **whole multiplex verbatim** (no PID filter/rewrite, no initialization data). Declared `unmodified-program` when the source PAT lists one program, `unmodified-multiplex` otherwise |
| `--retain-si` | off | Filtered mode: also keep DVB SI PIDs (NIT/SDT/EIT/TDT-TOT) |
| `--retain-null` | off | Filtered mode: also keep null (0x1FFF) packets |
| `--mux-rate <bps>` | `0` | Advisory source mux rate in bits/s (0 omits the catalog hint). The draft forbids `mpeg2tsMuxRate` in `unmodified-multiplex`, so `--transparent` and SRT ingest drop it when the source PAT lists several programs, and warn |
| `--fragment-ms <ms>` | `250` | Group cadence |
| `--segment-bytes <n>` | `65536` | Target object size |
| `--draft <n>` | `16` | MOQ draft version (14 or 16) |
| `--paced` | off | File sources only: pace publishing at media rate instead of as fast as possible |
| `--width/--height/--fps/--video-bitrate` | 1920/1080/30/2500 | Capture params |
| `--sample-rate/--channels/--audio-bitrate/--audio-codec` | 48000/2/160/aac | Audio params |

`Ctrl-C` (SIGINT) or SIGTERM stops gracefully. Exit code 0 on clean completion, 1
on error, 2 on invalid arguments.

## Run: seekable file

```bash
# Filtered single-program (default behavior) with a mux-rate hint.
./build-mock/moq2ts-cli --endpoint mock://local --namespace live/ch1 \
    --video sample.ts --program 1 --mux-rate 38000000

# Transparent whole-multiplex passthrough.
./build-mock/moq2ts-cli --endpoint mock://local --namespace live/ch1 \
    --transparent --video sample.ts
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
    --srt-config ./srt_callers.json --transparent
```

`pkt_size=1316` is worth keeping: 1316 = 7 x 188, so each SRT payload holds a
whole number of TS packets and no packet straddles a datagram boundary.
`latency_ms` sets SRT's retransmit buffer, which absorbs network jitter on the
ingest side before the publisher ever sees the bytes.

**SRT ingest always runs in transparent mode.** A contribution feed carries the
whole multiplex, so the filtered-mode options (`--retain-si`, `--retain-null`,
`--program`) do not apply and are ignored; passing them prints a warning. The
same goes for `--mux-rate`, which the catalog only carries in per-program mode.
Use a file or FIFO source if you need filtered single-program publishing.

## Run: live feed from ffmpeg (server / near-encoder)

### Via a named FIFO

`-y` is required on every ffmpeg command that writes to the FIFO: `mkfifo` has
already created the path, and without it ffmpeg prompts to overwrite and exits.

```bash
mkfifo /tmp/live.ts

# ffmpeg produces a continuous MPEG-TS into the FIFO (test source shown; swap in
# your real input with -i). Transparent mode is the byte-faithful passthrough path.
ffmpeg -re -y \
  -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
  -c:v libx264 -b:v 4M -c:a aac -b:a 128k \
  -f mpegts -muxrate 6M -pcr_period 20 /tmp/live.ts &

./build/moq2ts-cli --endpoint <relay-url> --namespace live/ch1 \
    --transparent --video /tmp/live.ts
```

### Via a stdin pipe

```bash
ffmpeg -re -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
  -c:v libx264 -b:v 4M -c:a aac -f mpegts -muxrate 6M - \
  | ./build/moq2ts-cli --endpoint <relay-url> --namespace live/ch1 \
        --transparent --video /dev/stdin
```

## Verify byte-faithful passthrough

For a file source in transparent mode the published payload must equal the input
byte-for-byte. Under the mock build, the mock publisher logs objects to stderr;
for a fidelity check, capture the emitted payloads and `cmp` against the source
`.ts`. The catalog JSON (also logged) should contain
`"mpeg2tsMode":"unmodified-program"` when the source PAT lists one program,
and `"mpeg2tsMode":"unmodified-multiplex"` otherwise. Neither contains
`mpeg2tsSiPids` or a root `initDataList`. An `unmodified-multiplex` catalog
must NOT contain `mpeg2tsProgramNumber`, `mpeg2tsPcrPid`, or
`mpeg2tsMuxRate`.

In filtered mode the PAT/PMT bootstrap is carried the way MSF-01 defines it: the
track gets an `initRef` string, and the bytes live in a root `initDataList` entry
of type `inline`. The older MSF-00 spelling put a base64 `initData` field on the
track itself; catalogs in that shape are still parsed on the receive side, but are
no longer produced.

## Notes

- **Live stream = live catalog.** A non-seekable source (FIFO/stdin) is detected
  automatically and advertised as `isLive: true`; VOD duration probing is skipped
  (it would otherwise open and consume the pipe a second time).
- **Filtered mode over a pipe** works too: the PAT/PMT init scan is buffered and
  replayed so no leading packets are lost. If the source never emits PAT/PMT within
  the first ~4096 packets, filtered init fails with a clear error - use
  `--transparent` for whole-multiplex feeds.
