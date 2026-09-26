# MSFTS Transparent TS over MOQ: Publisher Technical Note

## Overview

This document follows one continuous MPEG-TS stream from a camera/encoder onto the
**MOQ** transport. The two primer sections below explain just enough of each to
make the rest readable, and every stage is drawn as a diagram before it is
described in words.

It covers the publisher only. The pipeline has two roles here:

1. **Publisher (`moq2ts-cli`)** - takes MPEG-TS from a file, stdin, or a live SRT
   feed, cuts it into MOQ *objects* aligned to keyframe boundaries, and publishes
   them to a relay.
2. **Relay (`OpenMOQ moqx`)** - forwards objects to whoever is subscribed. It never
   opens a payload; it only moves bytes.

Any MSFTS-conformant subscriber can consume the result. Subscriber implementations
are out of scope here.

The defining idea of **MSFTS** ("MPEG-2 TS Streaming Format for MOQ") is that the
original TS multiplex is preserved **byte-for-byte on the wire**. The publisher
does *no* transcoding and *no* fMP4 synthesis - it just chops and labels. This is
the opposite of formats that demux TS into elementary streams and republish them
as fMP4 fragments.

---

## Primer 1 - MPEG-TS in five minutes

MPEG-TS ("Transport Stream") is a way to carry audio, video, and metadata as one
flat river of small, fixed-size **packets**. Each packet is **188 bytes** and
begins with the sync byte **`0x47`**. (A Blu-ray variant called M2TS uses **192**
bytes: a 4-byte timestamp prefix followed by the same 188.)

```
TS packet (188 bytes)
+-- byte 0        sync byte = 0x47
+-- bytes 1-2     payload_unit_start_indicator + PID (13 bits)
+-- byte 3        adaptation_field_control + continuity_counter
+-- [adaptation field]   (present only when the header says so)
|   +-- length
|   +-- flags byte
|   |   +-- discontinuity_indicator
|   |   +-- random_access_indicator   <- bit 6 = "a keyframe starts in this packet"
|   |   +-- PCR_flag
|   +-- PCR       (program clock, 33-bit @ 90 kHz)   (optional)
+-- [payload]     a slice of a PSI table, or a slice of a PES packet
```

Every packet carries a **PID** (packet identifier) that says which sub-stream it
belongs to. You learn what each PID means by reading two small tables that are
themselves carried in packets:

```
MPEG-TS multiplex  (one continuous stream of 188-byte packets)
+-- PID 0x0000   PAT   --> "the program's description (PMT) is on PID 0x1000"
+-- PID 0x1000   PMT   --> "video = HEVC on PID 0x0100 ; PCR clock rides on PID 0x0100"
+-- PID 0x0100   Video PES   (HEVC frames; carries PTS/DTS; RAI flag at keyframes)
+-- PID 0x0101   Audio PES   (optional; AAC/AC-3; carries PTS)
+-- PID 0x1FFF   Null packets (padding to hold a constant bitrate)
```

Four terms are used constantly below:

| Term | Plain meaning |
|------|---------------|
| **PAT / PMT** | The two "table of contents" packets. PAT points to the PMT; the PMT lists the video/audio PIDs and which PID carries the clock. |
| **PES** | The container inside TS payloads that holds one coded frame (or audio chunk) plus its timestamps. |
| **PTS / DTS** | *Presentation* and *Decode* timestamps (90 kHz ticks): when to show and when to decode a frame. |
| **PCR** | *Program Clock Reference* - the encoder's wall-clock, sampled into the stream so a decoder can pace itself. |
| **RAI** | *Random Access Indicator* - a single bit that marks the packet where a **keyframe (IDR)** begins. This is the only thing a receiver needs to find a safe place to start decoding. |

The one bit that matters most for this document is **RAI**: it is how the
publisher knows where a new group of pictures (GOP) starts, which is where it is
safe to begin a new MOQ group.

---

## Primer 2 - MOQ in five minutes

MOQ ("Media over QUIC") is a publish/subscribe transport. Instead of one long
socket, media is named and delivered as small addressable pieces over **QUIC**
(the same transport under HTTP/3). The naming hierarchy:

```
Namespace  "live/video_track1"          (the channel)
+-- Track  "program-1"                   (one media stream in that channel)
    +-- Group 0   (must begin at a keyframe / RAP)      -- QUIC stream A
    |   +-- Object 0   <- contains the IDR keyframe
    |   +-- Object 1
    |   +-- Object k   <- last object -> stream A is FIN'd (closed)
    +-- Group 1   (next keyframe)                       -- QUIC stream B
    |   +-- Object 0 ...
    +-- ...
    Track "program-1.timeline"   (a side track describing media time)
    Track "catalog"              (a JSON document describing all the tracks)
```

Key MOQ terms:

| Term | Plain meaning |
|------|---------------|
| **Object** | The smallest addressable payload. Here, one object = a fixed run of TS packets. |
| **Group** | An independently decodable unit. Here, one group = one GOP; its first object holds the keyframe. |
| **Subgroup / stream** | Each group is sent on its **own QUIC unidirectional stream**. This matters a lot (see below). |
| **Catalog** | A small JSON document (the MSF catalog) that tells a subscriber how to interpret the track - packet size, packets per object, "does every group start at a keyframe", etc. |
| **Relay** | A forwarder. Subscribers connect to it, not to the publisher. |
| **SUBSCRIBE filter** | Tells the relay *where in the stream* to start you (e.g. "at the next group boundary"). |

The single most important MOQ fact for this design:

> **QUIC guarantees byte order *within* one stream, but gives *no* ordering
> *across* different streams.**

Because each group travels on its own stream, **objects from a later group can
arrive before objects from an earlier group**. Any subscriber therefore needs a
small reorder buffer keyed by `(groupId, objectId)` - this is not optional polish,
it is required for correct playback. The publisher's side of the contract is that
every object carries the group and object IDs that make the reordering possible.

---

## High-Level Pipeline

```
Source
+-- SRT live feed (caller -> an ffmpeg/encoder listener)
+-- File / stdin
    |
    v
moq2ts-cli   (PUBLISHER)
+-- Detect packet size (188 vs 192)
+-- Parse PAT/PMT -> discover video PID (= "RAP PID") and PCR PID
+-- readObject(): accumulate 348 TS packets -> one 65 424-byte MOQ object
+-- Scan the object for random_access_indicator (RAI)
    +-- RAI on the RAP PID -> start a NEW group (object 0) on a NEW QUIC stream
    +-- otherwise          -> append as the next object on the current stream
        |
        v
   MOQ RELAY (OpenMOQ moqx) - forwards objects; never inspects payloads
        +-----------------> any MSFTS-conformant subscriber
```

---

# PART A - Publisher (`moq2ts-cli`)

The publisher's whole job is: **read TS, cut it into objects, put a group boundary
at every keyframe, and hand the objects to the relay.** It changes zero media
bytes.

## A1. Where the TS comes from - SRT and file

```
TWO INPUT PATHS

  Live:                                  Offline:
  +------------------------+             +------------------------+
  | ffmpeg / encoder       |             | .ts file  or  stdin    |
  | (SRT listener, TS out) |             |                        |
  +-----------+------------+             +-----------+------------+
              | SRT (caller connects)                | direct read
              v                                       v
  +------------------------+                          |
  | SrtSource (caller)     |                          |
  |  - 120 ms latency buf  |                          |
  |  - srt_recv() max 5264B|                          |
  |    = 4x1316 = 28 pkts  |                          |
  +-----------+------------+                          |
              | internal pipe (byte stream)           |
              +---------------+-----------------------+
                              v
                    +--------------------+
                    |  M2tsPacketizer    |  reads a flat byte stream,
                    |  (byte stream in)  |  re-frames it into TS packets
                    +--------------------+
```

**SRT path.** SRT ("Secure Reliable Transport") is a UDP-based protocol that adds
a retransmit/jitter buffer, popular for contribution feeds. Here `SrtSource` runs
in **caller** mode and connects out to an SRT **listener** (typically an ffmpeg
instance producing the TS). It sets a **120 ms** latency buffer (which absorbs
network jitter) and a receive timeout so it can poll a stop flag. Each
`srt_recv()` returns up to **5264 bytes** - four 1316-byte SRT payloads, and since
1316 = 7 x 188, that is **28 TS packets**. Those bytes are written into an internal
pipe that the packetizer reads as if it were a file. The path is selected with
`--srt-config <path>`, a JSON file in moqxr's caller format; the first entry in
`srt_callers` is used, and `--video` is then unnecessary.

**File / stdin path.** No SRT layer at all - the packetizer reads the device
directly. The only difference downstream is that a file is *seekable* while a pipe
or SRT feed is not (which matters for the init scan in A3).

Either way, the packetizer sees the same thing: **a flat stream of bytes that it
must re-frame into TS packets.**

## A2. Detecting the packet size (188 vs 192)

Before anything else, the packetizer peeks at the first **768 bytes** (four
maximum-size packets) and looks for the `0x47` sync byte at a regular interval:

```
peek 768 bytes, then test:
   sync at offset 0  AND  sync at offset 188  -> 188-byte TS      (packetSize = 188)
   sync at offset 4  AND  sync at offset 196  -> 192-byte M2TS    (packetSize = 192)
   neither                                     -> error: not packet-aligned
```

For 192-byte M2TS, the real 188-byte packet starts 4 bytes in (after the timestamp
prefix). The detected size is remembered and used for every read afterwards.

## A3. Reading the tables (PAT/PMT) and finding the video PID

The packetizer reads packets until it has seen both the **PAT** (PID `0x0000`) and
the **PMT** it points to. From the PMT it learns:

```
PMT tells us:
+-- PCR PID            (which PID carries the clock - usually the video PID)
+-- Video PID(s)       + stream_type   (H.264 = 0x1B, HEVC = 0x24)
+-- Audio PID(s)       + stream_type
```

The **video PID becomes the "RAP PID"** - the one PID whose packets are watched
for the keyframe flag. For **non-seekable** sources (stdin, pipe, SRT), every
packet consumed during this scan is saved into a *prebuffer* and replayed first, so
that not a single byte is lost from the front of the stream.

## A4. Building objects - how many TS packets per object

An **object** is just a fixed run of TS packets. The count is derived from a target
byte size:

```
packetsPerObject = targetSegmentBytes / packetSize
                 = 65 536 / 188
                 = 348 packets

one object = 348 x 188 = 65 424 bytes
```

Object size and timing at a typical bitrate:

| Property | Value |
|----------|-------|
| TS packets per object | **348** |
| Bytes per object | **65 424** |
| Media per object @ 2.5 Mbps | **~ 209 ms** |
| Objects per group @ 30 fps, GOP 60 (2 s keyframes) | **8-15** |

`readObject()` simply accumulates 348 synced packets into one payload. (In
transparent mode every packet is copied verbatim; a "filtered" mode also exists
that keeps only selected PIDs, but MSFTS runs transparent.)

## A5. Where groups begin - guaranteeing a keyframe at the start of every group

This is the heart of MSFTS. A MOQ **group must be independently decodable**, so a
group may only start where a **keyframe (IDR)** starts - and the encoder marks that
spot with the **random_access_indicator** bit.

As each object is assembled, the publisher scans its packets:

```
for each packet in the object:
    skip PSI (PID <= 0x1F) and null (0x1FFF) packets
    if packet is on the RAP PID  AND  random_access_indicator == 1:
        -> this object contains a keyframe (rapDetected = true)
```

The group counter is then advanced with a one-time gate so that the *first* group
is always ID 0:

```
GROUP BOUNDARY LOGIC

  rapDetected?                       sawFirstRap?      action
  -------------                      ------------      --------------------------
  no - append object to current group
  yes  (first RAP ever)              false -> true      keep group 0 (do NOT ++), obj 0
  yes  (any later RAP)               true              ++groupId, reset objectId = 0
```

Result: **the object that contains a keyframe always becomes object 0 of a new
group.** Because grouping is object-granular (not packet-granular), object 0 holds
the keyframe packet plus its neighbours; TS/PES are self-framing, so a subscriber
that starts at object 0 always has a complete IDR to initialize its decoder. This
is exactly what the catalog advertises as `mpeg2tsRandomAccess = true`. (For the exact
byte-vs-IDR interaction - objects are *not* cut short at the keyframe on this path -
and a measured worked example, see [A8](#a8-objects-vs-groups--how-byte-sizing-and-idr-grouping-interact).)

```
Group N   (starts at IDR keyframe)  -- QUIC stream N
+-- Object 0   [65 424 B]  <- keyframe (RAI set) lives here
+-- Object 1   [65 424 B]
+-- ...
+-- Object K   [65 424 B]  <- last -> stream N FIN
Group N+1 (next IDR)                -- QUIC stream N+1 (fresh stream)
+-- Object 0 ...
```

## A6. The catalog

Before media flows, the publisher emits an **MSF catalog** - a JSON document on the
`catalog` track (group 0, object 0) - so subscribers know how to read the payloads:

```
catalog (JSON)
+-- version = "draft-01"
+-- tracks[]
    +-- program-1              (the media track)
    |   +-- packaging          = "mpeg2ts"  -> payload is raw TS packets
    |   +-- mpeg2tsMode        = "unmodified-multiplex" or "per-program"
    |   +-- mpeg2tsPacketSize  = 188 or 192 -> bytes per TS packet
    |   +-- mpeg2tsRandomAccess = true      -> every group starts at a keyframe
    |   +-- role/mimeType/namespace/bitrate
    |   +-- targetLatency      = 1000       -> advisory latency target (ms)
    +-- program-1.timeline     (media-timeline side track)
```

## A7. Publishing to the relay

```
PUBLISH MODEL

  Group 0  -------------> QUIC unidirectional stream 0 : Obj0, Obj1, ... ObjK, FIN
  Group 1  -------------> QUIC unidirectional stream 1 : Obj0, Obj1, ...      FIN
  Group 2  -------------> QUIC unidirectional stream 2 : Obj0, ...
                                 |
                                 v
                          MOQ RELAY (OpenMOQ moqx)
                          forwards every object unchanged
```

Each group uses **one** QUIC unidirectional stream; objects are written in order on
it; the stream is FIN'd when the next group opens a new one. The last object of a
group is flagged as final-in-subgroup. The relay forwards objects to subscribers
without ever looking inside a payload.

## A8. Objects vs groups - how byte-sizing and IDR-grouping interact

Object size (Section A4) and group boundaries (Section A5) are **two independent axes**. Objects
are cut by a **fixed byte/packet count**; groups are marked by **IDRs**. A common
misreading is "if a keyframe appears before the byte mark, the object is cut short
there." That is only true on the *capture* path - **not** on the file/SRT path.

```
TWO CHUNKING BEHAVIOURS

  External TS  (file / stdin / SRT - this is the ffmpeg+SRT feed)
    - object = ALWAYS a fixed 348 packets (65 424 B); the IDR does NOT cut it early
    - the finished object is then scanned; whichever object CONTAINS the next IDR
      is relabeled Object 0 of a new group
    - so the IDR sits INSIDE Object 0, after a few tail packets of the previous GOP
    - group boundary is quantized to the fixed object grid

  In-process capture (camera/mic -> encoder)
    - object IS cut at the keyframe ("an object must not cross a group boundary")
    - Object 0 starts EXACTLY at the IDR; objects are variable-size (<= 348 packets)
```

| Source | Object size | Where the IDR sits | Group boundary |
|---|---|---|---|
| File / stdin / **SRT** (external TS) | **fixed** 348 pkts | **inside** Object 0 (prev-GOP tail precedes it) | snapped to the object that *contains* the IDR |
| In-process capture (encode) | **variable**, cut at the IDR | at the **first byte** of Object 0 | exactly at the IDR |

On the SRT path the byte grid never moves; only the *group label* snaps to it:

```
SRT / FILE PATH - the byte grid is fixed; the group boundary snaps to it

 fixed 65 424-byte objects:
 +-------+-------+-------+-------+-------+-------+-------+
 |  obj  |  obj  |  obj  |  obj  |  obj  |  obj  |  obj  | ...
 +-------+-------+-------+---^---+-------+-------+-------+
        ...group N objects...    | IDR starts here, mid-object
                             +- this object becomes Object 0 of group N+1
                                (it also carries group N's tail, before the IDR)

 A subscriber that joins at Object 0 still gets the IDR - with repeat-headers=1
 the VPS/SPS/PPS ride with it - and its demuxer simply skips the few pre-IDR bytes.
```

### Worked example - the exact feed (measured)

The feed `ffmpeg ... -c:v libx265 -r 30 -g 60 -keyint_min 60 -bf 0 -x265-params
"keyint=60:min-keyint=60:scenecut=0:open-gop=0:repeat-headers=1" -an -f mpegts ...`
was encoded to a 20 s clip and measured with `validate-ts.py`:

```
Keyframe / GOP
  -g 60 / keyint 60 @ 30 fps, scenecut=0, open-gop=0  ->  a FIXED IDR every 60 frames
  = 2.0 s GOP        (measured: 10 IDRs in the 20 s clip)
  -bf 0              ->  no B-frames (decode order = display order, DTS = PTS)

Object (SRT path - fixed)
  348 packets x 188 = 65 424 bytes  ~  203 ms at the measured ~2.58 Mbps (VBR)

Objects per group (MEASURED - tracks scene complexity, so it varies a lot)
  per group: [ 1, 1, 3, 8, 14, 15, 15, 13, 15, 14 ]   ->  avg ~ 10,  range 1-15
  99 objects across 10 groups
  low-motion intro GOPs -> 1-3 objects ;  high-motion GOPs -> 13-15 objects
```

So for this 2-second-GOP feed a group is **~10 objects on average**, but because the
object count is driven by *bytes* (VBR) and the group length by *time* (fixed GOP),
it swings from ~1 object (a nearly static GOP) to ~15 (a busy one). Two consequences
worth noting:

- **Group ~ GOP only when the GOP is larger than one object.** If a whole GOP is
  smaller than 65 424 bytes (~200 ms), it can share an object with its neighbour,
  and only the **first** IDR inside an object starts a group - extra IDRs in the same
  object do not create their own group. Group granularity is therefore floored at the
  object size.
- **Object 0 is not aligned to the IDR byte** on this path; it begins on the fixed
  grid and the IDR lands somewhere inside it. That is harmless for decoding (the
  keyframe + its headers are wholly contained), and it is why Section A5 says grouping is
  *object-granular, not packet-granular*.

---

# PART B - Relay (`OpenMOQ moqx`)

The relay is deliberately dumb: it accepts the publisher's objects and fans them
out to subscribers. It does not parse TS, does not reorder, does not re-time. Its
only relevant behaviour for this note is the consequence already stated in Primer 2
- because it forwards each group's stream independently, a subscriber can receive
group N+1's first object before group N's last object.

---

## Timing Model

- **Publisher.** Preserves all PTS/PCR/DTS byte-for-byte. No timestamp manipulation.
  A subscriber that needs a zero-based timeline rebases on its own side; nothing in
  the published stream depends on it.

---

## Error Scenarios

| Scenario | Publisher behaviour |
|----------|---------------------|
| SRT connection lost | Stops publishing; relay sends PUBLISH_DONE |
| Objects arrive out of order downstream | Not a publisher concern: group and object IDs carry the order |
| PTS wrap-around / `-stream_loop` | Transparent, sent as-is |
| Network congestion | SRT latency buffer absorbs ingest jitter; QUIC handles loss on egress |
| Publisher killed (SIGINT) | Clean stop, FIN on the last stream |

---

## Glossary (quick reference)

| Term | One-line meaning |
|------|------------------|
| **TS packet** | 188-byte unit starting with `0x47`; the atom of MPEG-TS. |
| **PID** | Number tagging which sub-stream a packet belongs to. |
| **PAT / PMT** | Tables that map PIDs to programs and elementary streams. |
| **PES** | Payload container that holds one coded frame + its PTS/DTS. |
| **PTS / DTS** | Presentation / decode timestamps (90 kHz). |
| **PCR** | Encoder clock sampled into the stream for pacing. |
| **RAI** | Bit marking the packet where a keyframe (IDR) starts. |
| **GOP** | Group of Pictures - from one keyframe to the next. |
| **MOQ** | Media over QUIC: named pub/sub media transport. |
| **Object** | Smallest MOQ payload; here a run of TS packets. |
| **Group** | Independently decodable MOQ unit; here one GOP, on its own QUIC stream. |
| **fMP4** | Fragmented MP4 (ftyp/moov once, then moof+mdat); the container MSFTS deliberately does not synthesise. |
| **Catalog** | JSON describing the tracks (MSF format). |
| **Relay** | Forwarder subscribers connect to instead of the publisher. |
