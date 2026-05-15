# PLYR-Qt CD Rip Strategy

The plan for bringing CD ripping + AccurateRip verification into PLYR-Qt
("Concerto") as a first-class in-app feature, replacing the loose
`~/Downloads/rip_cd.sh` bash script.

This doc captures decisions already made — do not relitigate them
without reason. See "Posture" at the bottom.

---

## North star

One downloadable app. No user installs of `flac`, `cdparanoia`, Python,
Homebrew, etc. Everything self-contained inside the Qt binary / .app
bundle.

## Non-negotiables

### 1. License stance: non-GPL / permissive

PLYR-Qt stays non-GPL. Path: clean-room reimplementation of publicly
documented algorithms + permissively-licensed libraries only.

- **libFLAC (BSD)** — decode + encode. Linkable / vendorable, no copyleft.
- **libdiscid (LGPL)** — MusicBrainz disc id; linkable in a closed app.
- **NOT libcdio / libcdio-paranoia / cdparanoia** — all GPL. Linking them
  would force PLYR-Qt to GPL.
- AccurateRip checksum, CDDB id, MusicBrainz disc id, CTDB lookup —
  clean-room implementations from public specs / Hydrogenaudio reverse-
  engineering notes. No transcription of GPL source.

The GPL question ("can the app itself be GPL?") was raised twice in
conversation and never firmly answered, but the de facto and consistent
direction is permissive. Treat that as settled unless the user explicitly
reopens it.

### 2. CD reading: native OS APIs, not cdparanoia

Not bundling cdparanoia as a subprocess (the obvious GPL workaround) —
writing our own CD-DA reader on native OS APIs:

- **macOS**: IOKit + SCSI MMC pass-through.
- **Linux**: `CDROM_READ_AUDIO` ioctl.
- **Windows** (if ever): SPTI.

More work, but it's the only way to keep code licensing clean *and*
make the binary fully self-contained (no helper subprocesses to ship).

### 3. Skip cdparanoia's "paranoia" jitter algorithm for v1

The full jitter-correction stack (overlapping reads with sample-domain
correlation) is out of scope for v1. Plain sector reads with basic
re-read on read errors are fine for the modern-drive + clean-disc common
case. If/when re-read logic is added later, it must be our own clean-room
implementation of the *technique* (read multiple times, compare, re-read
on disagreement) — never cdparanoia's code or anything closely derived
from it.

### 4. CD reading happens *downstream of all error correction*

Important framing: the drive's silicon does Viterbi/PRML and CIRC
Reed-Solomon decoding *before* the host interface returns a byte. No
software path (ours, cdparanoia, EAC) does error correction. What we
work with is post-CIRC PCM samples. The only downstream levers are
re-read, C2 error pointers (if the drive exposes them), and AccurateRip
cross-verification.

---

## Pipeline

### A. Setup (once per drive)
1. SCSI INQUIRY → drive vendor/product string.
2. Look up drive in **AccurateRip's drive offset database**
   (https://www.accuraterip.com/driveoffsets.htm — static lookup,
   bundleable as a JSON/CSV resource).
3. Save the offset in the app config.

### B. Rip
1. Read disc TOC.
2. Compute MusicBrainz disc id; query MusicBrainz for release metadata.
3. Read each track's CDDA sectors via the native OS API, **applying the
   drive offset** at read time.
4. Encode tracks to FLAC with libFLAC.
5. Apply MusicBrainz tags.
6. Save a rip log + `.cue` sheet alongside the FLACs. (Current
   `rip_cd.sh` discards the TOC, which made retroactive verification
   needlessly hard.)

### C. Verify
1. From a fresh disc: use the just-read TOC. From existing FLACs:
   reconstruct from per-track sample counts (every count must be a
   multiple of 588 samples = 1 CD sector) or read from the `.cue`.
2. Compute disc fingerprints: AccurateRip id1/id2, CDDB id, MusicBrainz
   disc id.
3. Query AccurateRip (`http://www.accuraterip.com/accuraterip/…/dBAR-NNN-id1-id2-cddb.bin`).
4. Decode FLAC → PCM with libFLAC.
5. Compute AccurateRip v1/v2 per track; compare to the response.
6. **Fallback on no-match**: run a per-disc offset scan (the verify-time
   pressing-offset detection that EAC / dBpoweramp / whipper all do).
   Handles (a) rips made without drive-offset correction, (b) pressings
   AR's DB has at a different alignment than the user's disc.
7. Optionally query **CTDB** (db.cuetools.net/lookup2.php) for a second,
   independent consensus pool. CTDB also stores parity → can detect *and
   repair*, not just verify.
8. Report per-track: accurate / not accurate / not in DB, with
   confidence count.

---

## What's already built

In `src/`:
- **`ArVerify.h` / `ArVerify.cpp`** — pure core: TOC reconstruction,
  disc-ID math (AR id1/id2, CDDB, MusicBrainz), AccurateRip v1/v2
  checksum, CTDB CRC32, response parsers, AR + CTDB offset scans.
  Decoupled from I/O.
- **`FlacDecode.h` / `FlacDecode.cpp`** — libFLAC-based decode: fast
  STREAMINFO read, full PCM decode, and Vorbis comment reader for
  inspection / verification.
- **`FlacEncode.h` / `FlacEncode.cpp`** — libFLAC-based encode locked
  to Red Book CD audio (44.1k/16/stereo), compression -8 to match
  rip_cd.sh. Accepts a list of Vorbis comment tags written into the
  output file ahead of the audio. Round-trip verified bit-identical
  across all 202 Rachmaninoff FLACs via `flacrt_cli`.
- **`MusicBrainz.h` / `MusicBrainz.cpp`** — MB web-service lookup
  (`ws/2/discid/<id>?fmt=json&inc=artist-credits+recordings`), JSON
  parse via Qt, medium-matching with the same precedence rip_cd.sh
  uses (disc-ID first, then track count, last-resort medium[0]).
- **`arverify_main.cpp`** — CLI harness (`arverify_cli` target).
- **`flacrt_main.cpp`** — encoder round-trip test (`flacrt_cli`).
- **`mbquery_main.cpp`** — MB lookup + tagged-encode demo
  (`mbquery_cli` target); with `--tag-track-1 <path>` it re-encodes
  one track with derived Vorbis tags and reads them back to confirm
  the tag-writing path.

In `CMakeLists.txt`:
- `arverify` static lib (QtCore only — for SHA-1 in the MusicBrainz id).
- `arverify_cli` desktop CLI executable.
- libFLAC discovered via `find_path` / `find_library` (Homebrew paths).
- `Qt6::Network` added to `find_package`.

Runs end-to-end against the 10 Rachmaninoff discs.

### How to run what's built

```
cmake --build build --target arverify_cli
./build/arverify_cli /Users/thompson/Rachmaninoff_Rips/CD*
```

Per disc it now does:

1. AccurateRip lookup → entries + per-track v1/v2 CRCs.
2. CTDB (CueTools DB) lookup → entries + per-track CRC32s.
3. AR offset scan: fast v1 prefix-sum over +/- 3000 frames; if that
   finds no match, a v2-aware slow scan over the two shortest middle
   tracks (catches the case where AR's DB stores only v2 CRCs for the
   disc — CD01 and CD08 here).
4. CTDB offset scan: independent of AR's, since the two pools cluster
   submissions separately. Slow CRC32 scan over the same two probe
   tracks, scoring offsets by their probe-A entry's confidence so a
   high-confidence canonical pressing wins over low-confidence
   collision-mates.
5. Per-track verification: v1+v2 vs AR at AR's offset; CRC32 vs CTDB
   at CTDB's offset; reports each pool's verdict + confidence.

CRC32 is delegated to zlib (PCLMULQDQ-accelerated on x86, hardware on
ARM64), so the slow scan finishes the 10-disc set in ~4 minutes total
on an Intel iMac.

---

## Known findings about the existing rips

Source collection: `/Users/thompson/Rachmaninoff_Rips/CD01_*..CD10_*` —
the RCA Victor Gold Seal "Sergei Rachmaninoff: The Complete Recordings"
(1992 box, 10 CDs). Ripped via `~/Downloads/rip_cd.sh` (plain cdparanoia,
no drive offset correction).

**9 of 10 verify bit-accurate** against AccurateRip at per-disc offsets
(`arverify_cli` reproduces every row of this table end-to-end):

| Disc | Detected offset | DB pressings | Notes |
|---|---:|---:|---|
| CD01 | −254 | 2 | v2-only in DB |
| CD02 | −570 | 4 | |
| CD03 | −670 | 4 | |
| CD04 | −658 | 4 | |
| CD05 | −658 | 5 | |
| CD06 | −658 | 4 | |
| CD07 | −658 | 4 | |
| CD08 | +6 | 4 | v2-only in DB |
| CD09 | none | 2 | no match anywhere in ±3000, v1 or v2 |
| CD10 | −670 | 4 | |

**CD09 is anomalous.** No offset under ±3000 matches v1 or v2. Most
likely: the user's pressing isn't in AR's DB. Less likely: rip has
genuine errors. To diagnose: re-rip and re-verify.

**The offset spread (~676 samples) is bigger than typical pressing
variation.** Working hypothesis: cdparanoia's bootstrap sync is adding
a per-disc variable component on top of the drive's true read offset.
This was deliberately NOT untangled — irrelevant for the new ripper,
which won't use cdparanoia. Could be definitively tested by re-ripping
the same disc twice on the same drive and comparing detected offsets.

---

## Hard-won technical gotchas

Pinned here because each took real time to figure out:

- **Offset convention varies per ID type.** AccurateRip id1/id2 use LBA
  offsets (track 1 = 0). CDDB id and MusicBrainz disc id use **absolute**
  offsets (track 1 = 150). Wrong convention → plausible-looking ID that
  silently 404s. Reference: `whipper/image/table.py` (`accuraterip_ids`
  vs `getCDDBValues`).
- **AR URL path** uses id1's *last three hex digits* in order
  `id1_hex[-1] / id1_hex[-2] / id1_hex[-3]`, lowercase. Then file
  `dBAR-NNN-id1-id2-cddb.bin` with id1/id2/cddb as lowercase
  `%08x`.
- **AR per-track CRC** can be v1 or v2 depending on submitter tooling.
  Always check both.
  - v1 = `(Σ sample[i] * (i+1)) mod 2^32`
  - v2 = `v1 + (Σ ((sample[i]*(i+1)) >> 32)) mod 2^32`
  - v1 is **prefix-summable** → O(1)/offset fast scan possible.
  - v2 is not → for v2 you have to compute per offset (acceptable for
    targeted verification at a known offset, not for wide scanning).
- **AR checksum skips** the first 5 sectors of track 1 and the last 5
  of the last track (2940 stereo frames each = lead-in/lead-out
  transition regions).
- **`sample` in the AR checksum** is the stereo frame packed as
  `uint32`: left in the low 16 bits, right in the high 16 bits
  (little-endian WAV layout).
- **CTDB TOC format** is LBA offsets followed by total disc length in
  sectors, colon-separated: `0:t2_lba:t3_lba:...:disc_length_in_sectors`.
  NOT the 150-based form.
- **CDDA host interface only exposes post-CIRC PCM** — no access to raw
  channel bits or Reed-Solomon parity. Reed-Solomon (CIRC) and any
  Viterbi/PRML detection happen inside the drive's silicon. Nothing
  software-side can redo or improve them.
- **CTDB stores plain CRC32 (zlib-compatible, IEEE polynomial)** over
  the raw little-endian PCM bytes of each track — *not* AR v1 or v2.
  No AR-style skip regions. The disc-wide offset shift is the only
  thing in common with AR's math.
- **AR and CTDB cluster submissions into separate pressing buckets**,
  so the per-disc offset where CTDB matches routinely differs from the
  AR offset on the same physical disc. On the Rachmaninoff set: 1 of 9
  matches at the same offset (CD01); 8 of 9 don't. The verifier runs
  independent offset scans for each pool.
- **First/last track pressing variation is the rule, not the
  exception.** Different pressings often differ by a few samples of
  silence/fade in the first or last track, producing different CRCs
  there even when the middle tracks match cleanly. Reflected in
  AR's per-track confidence: typically lower on tracks 1 and N. CTDB
  shows the same pattern: 8 of 9 verified discs have exactly one
  track 1 or track N mismatch, with all middle tracks accurate.

---

## What's left

1. ~~Offset detection in the C++ verifier.~~ **Done.** Fast v1
   prefix-sum scan with a v2-aware slow fallback, both in
   [`src/ArVerify.cpp`](src/ArVerify.cpp) (`scanForOffset` +
   `scanForOffsetSlow`). Reproduces every offset in the table above.
2. ~~CTDB query in the verification pipeline.~~ **Done** (verify path).
   CTDB lookup, CRC32 verification, and an independent offset scan that
   prefers high-confidence canonical pressings. See findings below — AR
   and CTDB cluster submissions independently, so the offsets routinely
   differ between the two pools. Repair via CTDB parity is still future
   work and not on this list.
3. **Native CD reader** — macOS IOKit first, Linux ioctls second.
4. **Drive offset DB integration** — fetch/bundle the AR drive offset
   table; SCSI INQUIRY at runtime; cache per-user.
5. ~~MusicBrainz lookup + tagging in C++/Qt.~~ **Done.**
   [`MusicBrainz.{h,cpp}`](src/MusicBrainz.h) does the discId lookup
   against `musicbrainz.org/ws/2`, JSON parse, and medium picking
   (disc-ID match → track-count match → medium[0], the same precedence
   `rip_cd.sh` used). Tagging is plumbed through
   [`FlacEncode`](src/FlacEncode.h) — Vorbis comments written ahead of
   the audio, verified both via our own reader and `metaflac --list`
   externally on the 10-disc Rachmaninoff set.
6. ~~libFLAC encoder path.~~ **Done.** [`FlacEncode.{h,cpp}`](src/FlacEncode.h)
   wraps the libFLAC stream encoder, locked to Red Book CD audio at
   compression -8. Verified bit-identical across all 202 Rachmaninoff
   tracks via the new `flacrt_cli` round-trip target.
7. ~~Vendor libFLAC into `third_party/`.~~ **Done.** xiph/flac 1.4.3
   lives in [`third_party/flac/`](third_party/flac), pruned to the C
   library + its direct deps (no C++ wrapper, CLIs, tests, docs,
   examples). Built via the upstream CMake under
   `add_subdirectory(... EXCLUDE_FROM_ALL)` with `BUILD_CXXLIBS`,
   `BUILD_PROGRAMS`, `BUILD_TESTING`, `WITH_OGG` etc. all off.
   `otool -L` on the resulting `arverify_cli` no longer mentions
   `libFLAC.dylib` — only QtCore/QtNetwork and macOS system frameworks
   remain as runtime deps. Round-trip + AR/CTDB verify unchanged
   bit-for-bit after the swap.

---

## Reference artifacts from the build session

In `/tmp/` (ephemeral; may not survive across sessions):

- `toc_check.py` — full DB lookup probe (AR + MusicBrainz + CTDB).
- `offset_probe.py` — v1 fast-scan + v1+v2 verification at candidate
  offsets. **This is the reference algorithm** for the C++ offset
  detection task; the math and behavior are load-bearing.
- `holdout_probe.py` — focused v1+v2 scan for non-matching discs.
- `silence_probe.py` — leading-silence vs offset correlation check
  (negative result — content variation dominates, can't extract
  offset from silence).
- `probe-venv/` — Python venv with numpy installed for the probes.

These are diagnostic scratch, not to be ported as-is. The *algorithm*
in `offset_probe.py` is what to study when implementing task 1
(C++ offset detection).

---

## Deliberately deferred / out of scope

- **Full cdparanoia-grade jitter correction.** Not for v1. Must be our
  own clean-room implementation if/when added.
- **CD09 root-cause investigation.** Re-rip and re-verify; not blocking
  the build.
- **iOS ripper.** No optical drives. The verifier *might* run on iOS
  but the ripper is desktop-only.
- **Mac App Store distribution.** A bundled-cdparanoia-subprocess
  fallback would conflict with App Store GPL terms. The native-API
  approach we chose keeps us App-Store-eligible.
- **AccurateRip CRC submission.** We're a consumer of AR, not a
  submitter (for now).
- **Encoding profiles beyond FLAC -8.** Match existing rip_cd.sh.

---

## Posture for a new session

Skim structure first; then verify the non-negotiables haven't been
relitigated. If a conversation drifts toward "let's just link libcdio"
or "use cdparanoia as a subprocess to keep moving," that's a
re-decision of something already settled — surface the licensing
constraint, don't restart from "what's easiest." The slower
native-API path *is* the path, and `arverify` is the proof-of-concept
that this stack works.

The empirical findings about cdparanoia's per-disc offset variation
on the existing rips are also a settled question — interesting, but
not worth investigating further when the new ripper won't use
cdparanoia at all. Focus forward, not back.
