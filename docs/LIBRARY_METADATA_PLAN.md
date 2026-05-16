# Concerto — Folder-Time Library Metadata Plan

How Concerto enriches a **folder of audio files** with the same
classical-rich metadata the rip pipeline produces (composer, work,
movement, conductor, typed performers, orchestra, recording venue/date,
MusicBrainz IDs). Companion to `METADATA_PLAN.md` (rip-time) and
`METADATA_PIPELINE_AUTOMATED.md` (rip-time distilled).

Status: research and plan. No code in this pass. Implementation steps
are sketched in §10 and reference (where possible) extension points on
classes that already exist for the rip-time pipeline so the next agent
can reuse rather than rewrite.

The rip-time pipeline is the prior art everywhere in this document:
the same MusicBrainz client (`src/MusicBrainz.{h,cpp}`), the same JSON
flattener (`musicbrainz::flattenRelease`), the same scoring algorithm
(`src/MetadataScoring.{h,cpp}`), the same FLAC tag mapping
(`src/FlacTags.{h,cpp}`), and the same on-disk cache
(`src/MetadataCache.{h,cpp}`) all carry over. The new work is:

1. an **identification frontend** that produces an MB release MBID (or
   a recording MBID, or "give up") from a folder full of files — i.e.
   what disc-ID computation does at rip time;
2. a **library data store** that holds the resolved metadata so the
   library QML can query it without re-reading every FLAC; and
3. **lookup-timing** policy — when do we actually call out to MB?

The recommendation is at the end (§11) — read that first if you want
the answer, then come back for the reasoning.

---

## 0. TL;DR — folder-time pathway in one screen

Constraints stay the same as the rip pipeline:

- Fully automated; **no per-user setup, no auth prompts, no manual
  entry** (constraint #1 from `METADATA_PLAN.md` §0).
- Free for the end user; embedded app-wide keys are acceptable
  (constraint #2). MusicBrainz, AcoustID, Cover Art Archive only — **no
  Apple Music in v1.** Apple Music enrichment from the rip pipeline is
  explicitly deferred for the library path until after the basic flow
  ships; see §5.4.
- Permissive-license clean (constraint #3). AcoustID/Chromaprint is the
  one component with an LGPL gate — see §3.4.

The folder identification chain, ranked by cost-vs-accuracy:

```
[Folder of audio files]
   │
   ├─ Stage Z: Trust marker (added 2026-05-16; see §A)
   │   Scan tags for CONCERTO_PIPELINE_VERSION. If a quorum carries it
   │   with version ≥ kConcertoPipelineVersion AND a valid
   │   MUSICBRAINZ_ALBUMID, RETURN immediately. Zero web calls. This
   │   is the steady-state fast path — every file Concerto ever
   │   enriched hits here on re-open. DB-as-authority equivalent
   │   covers the non-writeback case via content-hash (§A.4).
   │
   ├─ Stage A: Embedded MBID tags
   │   Scan ≤N tags per file. If MUSICBRAINZ_ALBUMID is present on a
   │   majority quorum of files, fetch /ws/2/release/<mbid> directly.
   │   Cost: zero network until the hit. ~30–60% of in-the-wild
   │   classical FLACs in our experience.
   │   → flatten via existing musicbrainz::flattenRelease → DONE.
   │
   ├─ Stage B: AlbumDiscId / disc-ID tag
   │   If MUSICBRAINZ_DISCID is present, that's the rip-time cache
   │   key — re-uses MetadataCache::getByDiscId. Costs zero or one
   │   network round-trip.
   │   → DONE if hit; else fall through with the disc-ID retained as
   │     a search hint.
   │
   ├─ Stage C: TOC / track-count + duration fingerprint
   │   No tags. Build a folder-TOC (sort files, total each duration in
   │   seconds, count tracks). Query MB for releases with the same
   │   track count and similar total duration. Apply the rip-time
   │   scoring function (with the duration-defender component doing
   │   the heavy lifting). ~15–25% additional hits, mostly classical
   │   re-issues whose source files lost their MUSICBRAINZ_* tags
   │   along the way.
   │   → DONE if a strong score wins; else Stage D.
   │
   ├─ Stage D: AcoustID per-track fingerprint
   │   Decode N tracks (FLAC / MP3 / AAC), fingerprint each via
   │   Chromaprint, look up the *intersection* of releases across all
   │   per-track results. The release that contains every recording is
   │   the right release. Slow (decode + HTTP per track, MB rate cap of
   │   1/s); cheap to confirm a candidate from Stage C cheaply.
   │   → DONE if the intersection yields exactly one release; else
   │     Stage E.
   │
   └─ Stage E: Best-effort folder commit
       Keep whatever the user's existing tags say. Mark the folder
       "unresolved" in the sidecar DB so we don't keep paying the
       fingerprinting cost on every open. The user's existing tags
       still drive the UI; they're just not classical-enriched.
```

Storage choice (see §6 for the long version):

- **Hybrid: sidecar DB authoritative, file writeback opt-in.**
  Per-release rich metadata (typed performers, work-rels, MBIDs) lives
  in a SQLite DB at `~/Library/Application Support/Concerto/library.db`.
  Files are read-only by default. A user preference
  `Settings → Library → Write enriched tags back to files` (off by
  default) opt-in writes the resolved tags into the FLAC files
  themselves. Either way, the sidecar is always populated so the
  library UI works the same with or without writeback.

Lookup timing (see §7):

- **Lazy-eager hybrid.** When the user opens a single folder, eagerly
  resolve that folder (one MB round-trip; ~500ms cold, ~10ms cached).
  When the user opens a parent folder containing many album folders,
  resolve all of them as a **background sweep** (1 MB request/sec under
  the rate cap). Per-track lookup is *never* triggered by playback
  alone — fingerprinting is opt-in via a per-folder "identify this"
  action.

Library data model (see §6):

- **SQLite, normalized.** A flat-JSON-per-album approach mirroring the
  rip-time `MetadataCache` does *not* scale to the library queries
  classical listeners actually want ("all Pollini recordings", "every
  Brahms symphony in my library"). The sidecar starts as SQL on day
  one. Schema sketch in §6.3 — 7 tables, ~12 indices.

What we explicitly **defer past v1** (called out so the dev sees them
before the implementation order):

- Apple Music Stage 1.5 in the folder flow (§5.4).
- WAV / AIFF tag *writeback* (read-only in v1; embedded-ID3 write is
  niche and fragile). FLAC / MP3 / M4A / OGG writeback all land in v1
  via TagLib — see §4.
- Perceptual cover-art hash matching.
- Library-wide "fix all duplicates / merge" tools.

What we keep open as **toggles for the user**:

- Whether to write enriched tags back to files (default off).
- Whether to run AcoustID fingerprinting (default on, but the per-track
  cost is high enough that we surface it as a setting).

---

## 1. The problem statement

The rip pipeline answers: *"User just inserted a CD; identify it."*
The input is a TOC, the canonical key is the MB disc-ID, and the
fallback chain bottoms out in a deterministic stub.

The library pipeline answers: *"User just opened a folder of files;
identify what it is and enrich it."* The input is an unstructured set
of audio files. There is no canonical-fingerprint analogue to the TOC
disc-ID — different signals exist (tags, filenames, audio content) and
they have wildly different cost-vs-accuracy tradeoffs.

A few realities to anchor the design:

- **The dev's collection is overwhelmingly FLAC** (see `Track.h`,
  `FlacTags.cpp`, `PlaylistModel.cpp` — every existing path is
  FLAC-only). MP3 / AAC matter eventually but defer.
- **The user often has *some* existing tags.** Sources include: ripping
  software that wrote MB tags (Picard, the rip-pipeline we just built,
  EAC + a CUE sheet); third-party file dumps (varies wildly); manual
  taggers (mp3tag, kid3, Picard interactive). Coverage on "useful tags"
  ranges from 0% (anonymous track01.flac files) to ~95% (a clean Picard
  rip with every Vorbis comment populated).
- **The dev is a classical listener.** Composer / work / movement /
  conductor / typed performer are the load-bearing fields. Pop-style
  ARTIST + TITLE is not enough.
- **The rip-time pipeline is the better-documented half.** It got
  ~1000 lines of careful design in `METADATA_PLAN.md` and a working
  implementation (`MetadataResolver` + friends). The folder-time
  pipeline should share as much of that as possible.

---

## 2. Identification — how do we figure out what an unknown folder *is*?

This is question 1 from the brief. Rank the available signals by
cost-vs-accuracy, build a fallback chain, and quantify expected hit
rates.

### 2.1 The signal table

| Signal | Cost (per folder) | Accuracy | Coverage in the wild | When useful |
| --- | --- | --- | --- | --- |
| **`MUSICBRAINZ_ALBUMID` Vorbis tag** | 0 (local read) + 1 HTTP | very high — direct MBID lookup | ~30–50% of classical FLACs | Picard / our own ripper output |
| **`MUSICBRAINZ_DISCID` Vorbis tag** | 0 + 1 HTTP (or cache hit) | high; goes through existing `MetadataCache::getByDiscId` | ~10–20% | Older Picard versions, EAC + foo_musicbrainz |
| **`MUSICBRAINZ_RELEASEGROUPID`** | 0 + 1–N HTTP | medium — picks *some* release from the group | <5% (rare to have RG but not Release) | Edge cases |
| **`ALBUM` + `ARTIST` strings** | 0 + 1 HTTP (search) | medium-low — fuzzy text match | ~70%+ have *some* values | Catches mass-of-pop-and-classical cases without MB IDs |
| **`BARCODE` / `CATALOGNUMBER` Vorbis tag** | 0 + 1 HTTP | medium — single result usually | ~15–25% | Major-label classical rips often have BARCODE |
| **`ISRC` per track** | 0 + 1 HTTP per composer-less track | high (recording-level), low (release-level) | ~20% on classical, much higher on pop | Deferred; useful as a tiebreaker, not a primary signal |
| **Folder / filename heuristics** | 0 (local string parsing) | low; brittle on classical | most folders have *some* pattern | Last-resort hint to MB text search |
| **Track count + sum of durations** | 0 (local FLAC STREAMINFO read) | medium | 100% available | Reuses rip-time §3.3.1 duration-defender |
| **Per-track Chromaprint + AcoustID intersection** | ~150–400ms decode + 1 HTTP per track + MB rate cap | high (≥95% on classical with good audio) | needs Chromaprint lib | Only when everything else has failed |
| **Cover art perceptual hash** | image hash + MB CAA browse | low-medium and very expensive | needs `cover.jpg` in folder | Defer to v2 |

Two principles fall out:

1. **Fall through tag-based signals first.** They cost zero network on
   miss, are highly accurate on hit, and recover ~60–80% of the
   classical wild in our estimate.
2. **AcoustID is the last resort because it's expensive.** Per-track
   decode is acceptable; per-track network at MB's 1-req/s rate cap on
   a 100-album folder is *not* acceptable as a default. Surface it
   behind a per-folder action.

### 2.2 The chain (UPDATED 2026-05-16 — multiformat + provenance)

```
identifyFolder(folder) -> { release_mbid? recording_mbid? confidence }

Z. Trust marker: scan every audio file in folder for the
   CONCERTO_PIPELINE_VERSION provenance tag (§A). If a quorum of files
   carries it AND each version ≥ kConcertoPipelineVersion AND those
   files also carry valid MUSICBRAINZ_ALBUMID tags consistent across
   the quorum → return (release_mbid=that, "concerto-trusted",
   skip_web=true). The DB-as-authority equivalent (§A.4) covers the
   no-writeback case via content-hash lookup. ZERO network calls in
   the trusted path — Stage Z is the steady-state fast path.

A. Tag-quorum: scan every audio file in folder for MUSICBRAINZ_ALBUMID.
   if the majority share the same value and that value parses as a
   UUID → return (release_mbid=that, "tag-album").

B. Disc-ID fallback: if MUSICBRAINZ_DISCID is present on a majority,
   check MetadataCache::getByDiscId. Cache hit → return cached AlbumMeta.
   Cache miss → MB disc-ID lookup → score → top-1 → second hop → return.

C. RG / Barcode / Catalog: if MUSICBRAINZ_RELEASEGROUPID is present,
   fetch /ws/2/release-group/<id>?inc=releases and apply the rip-time
   scoring algorithm with the folder's TOC.
   else if BARCODE present, GET /ws/2/release?query=barcode:<digits>
   else if CATALOGNUMBER + LABEL present,
        GET /ws/2/release?query=catno:<...>+AND+label:<...>

D. Folder TOC fingerprint (no tags or none useful):
   Build a TocSummary equivalent — sort files by track number, total
   their durations from STREAMINFO. Query MB:
     GET /ws/2/release?query=tracks:N+AND+dur:<approx>
   Apply rip-time scoring, with the duration-defender carrying most of
   the weight. If a single release scores >threshold, commit.

E. AcoustID intersect (when D fails AND the user has opted in OR the
   folder is small enough to be cheap):
   Fingerprint every file via Chromaprint, look up each via AcoustID,
   intersect the resulting recording-MBID sets' release lists. Return
   the unique release that contains all recordings.

F. Unresolved: leave the folder as-is. Use the user's own tags for
   display. Mark the folder "unresolved" in the sidecar so we don't
   re-attempt every open.
```

Stage Z is the trust gate: it's checked **before** Stage A and bypasses
every network call. The rest of the chain still applies the first time
Concerto sees a file. After that, every subsequent open of the same
folder (or a folder containing that file after a move/rename, via the
content-hash DB lookup) hits Stage Z and skips A–E entirely.

The post-Z stages **directly mirror the rip-time fallback chain**
(`MetadataResolver` runs Stages 1 → 1b → 2 → 3 → 4). The right
primitive is "a frontend that produces a release MBID + medium
position", and **the rest of the pipeline below that point is reused
verbatim from rip-time** — `flattenRelease`, scoring, `MetadataCache`,
`buildVorbisTags`.

### 2.3 Quorum and degenerate cases

"Tag quorum" is doing real work in Stage A. Concrete rules:

- The MBID must appear on **≥60% of files** in the folder for us to
  trust it. Below 60%: treat as "some random file has a tag", fall
  through.
- All files agreeing wins outright.
- Mixed MBIDs (≥2 distinct values, each on ≥30% of files) — the folder
  is a **multi-album dump** (e.g. "Greatest Hits Compilation/" with
  files from many albums). Don't pick one; commit each MBID separately
  in the sidecar, key per-file rather than per-folder. Surface a
  "mixed-content folder" badge in the UI (see §9). This is a rare-but-
  real case; the spec deserves it.

If the user has multi-disc folders (e.g. `Mahler Box/CD1/`,
`Mahler Box/CD2/`), every disc is its own folder, every disc has its
own `DISCNUMBER` tag, and every disc gets independently resolved. The
sidecar joins them via `MUSICBRAINZ_RELEASEGROUPID` so the UI can show
the box as one logical album (§7.5).

If a single disc spans multiple subdirectories in the same release
(rare — usually the user concatenated something), the
`MUSICBRAINZ_ALBUMID` quorum catches it. If it doesn't, the folder
fails Stage D with "track count too high for any candidate release."
Mark unresolved, don't fight.

### 2.4 Predicted hit rates (estimated, no benchmark)

These mirror the rip-time §3.3.3 estimate format. They are *bounded
guesses* based on the dev's collection and structural reasoning:

| Outcome | Estimated share | Reasoning |
| --- | --- | --- |
| Stage A (`MUSICBRAINZ_ALBUMID` tag) | ~30–50% of classical folders | Coverage depends almost entirely on the user's tagging history. Higher in tightly-curated libraries; lower in scraped-from-the-internet collections. |
| Stage B (`MUSICBRAINZ_DISCID`) | ~5–15% | Older tag flow; usually overlaps Stage A. |
| Stage C (RG / barcode / catno) | ~5–10% | Captures partially-tagged files. |
| Stage D (TOC fingerprint) | ~15–20% | Track-count + total-duration is surprisingly discriminative on classical — symphonies have very specific durations. |
| Stage E (AcoustID intersect) | ~5–10% | When the user opts in. |
| Stage F (unresolved) | ~5–15% | Rare classical, indie, self-recorded, very long-tail. The user's own tags still drive display. |

Net expectation: **~85% of the user's classical library auto-enriches
to a verified MB release**, ~5–10% sit on user-supplied tags only, ~5%
are completely unidentified. About the same hit rate as the rip-time
pipeline, with a different distribution across stages.

Caveat — same as `METADATA_PLAN.md` §3.3.3: a user who only owns
unimpeachable Picard rips sees ~95% Stage A. A user whose library is
mostly bootlegs / private recordings sees Stage F dominate. The dev's
collection is somewhere in between.

### 2.5 Where AcoustID lives in the chain (and why later than the brief implied)

The brief asked us to think about AcoustID up front. Doing fingerprinting
on every track of every folder during a normal "open folder" is
~150–400ms decode + ~1s MB rate-cap-imposed delay per track. For a
13-track album that's 17s per folder. For a 500-folder library bulk
import, that's an unacceptable ~2.5 hours.

**Recommendation: AcoustID never fires automatically on folder-open.**
It fires only when:

- The folder has no useful tags AND Stage D fails (a rare combination
  the user would notice), OR
- The user explicitly invokes a "Re-identify this folder" action from
  the per-folder UI.

The fingerprinting result *is* cached per-file (keyed by content hash
— see §6.4), so re-identification is cheap on repeat.

The dev's "AcoustID, but slow" intuition was right. Push it deep
enough in the chain that 85% of folders never touch it, and gate the
remaining 15% behind explicit intent.

---

## 3. Reuse of the existing rip-time pipeline

This is question 5 from the brief and the single most important
section, so it sits up front rather than at the end. The principle:
**every shared concept gets exactly one implementation.**

### 3.1 What's already a perfect fit

| Existing class | Reused as-is for folder flow? |
| --- | --- |
| `musicbrainz::Client` (async MB HTTP, second-hop) | Yes — same signatures, same caller pattern. |
| `musicbrainz::flattenRelease()` | Yes — same release JSON → `AlbumMeta`. |
| `concerto::metadata::scoring::pick()` | Yes — same deterministic ranking, given the same `TocSummary`. |
| `concerto::metadata::scoring::pickMedium()` | Yes — needed for multi-disc-set selection (§7.5). |
| `concerto::metadata::MetadataCache` (rip-time) | **Partially** — see §3.3. The disc-ID-keyed cache file format works for folders that *have* a disc-ID; for folder-keyed cases we want a different storage layer (the library DB, §6). |
| `concerto::metadata::AlbumMeta` / `TrackMeta` POD | Yes — the runtime shape stays identical. |
| `concerto::metadata::buildVorbisTags()` | Yes — for FLAC writes; the function now also emits the §A.1 `CONCERTO_*` provenance markers on every bundle. TagLib `AudioTagIo` covers non-FLAC writebacks. |
| Tag mapping table (Picard / `METADATA_PLAN.md` §2.4) | Yes verbatim — extended to the cross-format mapping in §4.2. |

### 3.2 What needs a small extension

| Class | Extension |
| --- | --- |
| `musicbrainz::Client` | Add a `fetchReleaseByMbid()` overload that doesn't go through disc-ID at all. Currently the resolver calls `fetchRelease(mbid)` which is exactly what we need — same signature works. **No code change needed; just a new caller.** |
| `musicbrainz::Client` | Add a *search* method for folder-TOC and barcode queries: `searchReleases(QString query)` that wraps `/ws/2/release?query=...&fmt=json`. ~25 lines, parallels the existing async pattern. |
| `MetadataScoring` | Extend `TocSummary` to optionally carry per-file content hashes so the score can prefer releases whose recording MBIDs have already been seen elsewhere in the library. ~5-line extension. |
| `MetadataCache` | Add a `getByReleaseMbid(mbid)` accessor (release-only lookup, no medium position needed for the folder case). The alias-by-release-MBID is already half-built (`releaseAliasPath` in `MetadataCache.cpp`); we just need to expose it. |

### 3.3 What's genuinely new

| Component | Purpose |
| --- | --- |
| **`FolderIdentifier`** | Strategy chain over Stages Z + A–E. Outputs a release MBID + confidence. |
| **`AudioTagIo`** | Format-agnostic tag read AND write via TagLib (§4). Covers FLAC, MP3, M4A/ALAC/AAC, OGG, WAV, AIFF — the full superset of `QAudioDecoder` formats. FLAC fast-paths through existing `FlacTags::read()` to avoid TagLib spin-up on the rip pipeline hot path. |
| **`AudioFrameHash`** | The content-hash routine that survives tag edits and file moves (§6.2). One small per-format header-skip function. |
| **`AudioFingerprinter`** | Wraps Chromaprint. Per-file decode. Vendored library, MIT-clean (rip-time plan §1.3a.1). Shared by rip-time Stage 3 and folder-time Stage E. |
| **`LibraryDatabase`** | SQLite-backed. Per-release / per-track / per-performer rows. The sidecar (§6). Also stores `releases.pipeline_version` and `files.writeback_at` for the DB-side trust check (§A.4/§A.5). |
| **`LibraryResolver`** | High-level "scan this folder, populate the library" orchestrator. Reuses `MetadataResolver`'s pipeline-runner pattern, just with a different frontend. |
| **`LibrarySweeper`** | Background QThread that runs through unresolved folders at the MB rate-cap. The polling worker behind §7.3. |

The pattern: **a `LibraryResolver` is to `FolderIdentifier` what
`MetadataResolver` is to disc-ID computation.** Both orchestrate the
same downstream pipeline (`musicbrainz::Client::fetchRelease` →
`flattenRelease` → cache write); they differ only in how they discover
the release MBID.

A concrete refactor option: pull the rip-time `MetadataResolver`'s
"Stage 1b onward" into a `ReleaseResolver` (release MBID → AlbumMeta),
have `MetadataResolver` keep "Stage 1 disc-ID → MBID" as its frontend,
and have `LibraryResolver` carry its own "Stages A-E folder → MBID"
frontend. They both invoke `ReleaseResolver` on the back end. The
refactor is **strictly optional for v1** — `MetadataResolver` already
exposes `fetchRelease(mbid)` as a side-door via `musicbrainz::Client`,
and `LibraryResolver` can call that side-door directly. The split is
a refactor pass to take after the basic library flow ships.

### 3.4 What the rip pipeline's `MetadataCache` should become

Right now the rip cache is one-JSON-file-per-disc-ID. That's optimal
for the rip flow (disc-ID is the only key the rip path has). For the
folder flow:

- We rarely have a disc-ID; we have a *release MBID* (Stage A) or
  nothing (Stage D).
- We frequently want library-wide queries ("every Pollini recording"),
  which JSON-per-album makes painful.

**Recommendation:** keep the rip-time JSON cache as a *raw-JSON
mirror* (write-through from the SQL DB for rip-pipeline back-compat
and debugging) and treat the new SQLite library DB as the
authoritative store. Rip-time results write to both — the JSON file
for the rip flow's existing `MetadataCache::getByDiscId()` callers,
and the SQL DB so the library UI sees the freshly-ripped album
immediately. Folder-time results write only to the SQL DB.

This costs ~20 lines of glue in `MetadataResolver::onReleaseResolved`
(call `libraryDb.upsertRelease(album)` after the existing
`m_cache->put`). The two stores diverge only on schema; the data is
the same `AlbumMeta`.

---

## 4. Audio formats and the tag-read surface (UPDATED 2026-05-16 — multiformat + provenance)

Playback already supports every common format: `AudioWorker.cpp` uses
`QAudioDecoder` (CoreAudio/AudioToolbox on macOS) and plays FLAC,
ALAC, AAC/M4A, MP3, WAV, and AIFF natively. A library feature that
only enriches FLAC gives an empty experience on every non-FLAC folder.
**All playable formats must be enrichable on day one** — the previous
v1.1-ID3-read / v2-ID3-write staging plan predated multi-format
playback and is now wrong.

### 4.1 TagLib as the single tag I/O backend

[TagLib](https://taglib.org/) (LGPL-2.1) is the de-facto C++ tag
library covering the full `QAudioDecoder` format superset (FLAC, MP3
ID3v1/v2.3/v2.4, MP4/M4A, OGG, Opus, WAV, AIFF). Mature backing for
Picard, Strawberry, Clementine, Audacious.

- **Dependency add.** Homebrew `taglib` 2.x (sufficient for MP4
  freeform atoms + ID3v2 movement frames). CMake:
  `find_package(TagLib CONFIG)`, PkgConfig fallback. No source-tree
  vendoring.
- **LGPL closed-source pattern: dynamic-link only.** Ship
  `libtag.dylib` in `Contents/Frameworks/`; `@rpath` resolution lets
  the user substitute a rebuilt copy, which is the relinkability
  LGPL-2.1 §6 demands. Mention TagLib + LGPL text in About. Same
  shape we already use for libFLAC. **Never statically link TagLib.**

### 4.2 Format ↔ tag-frame mapping (read)

Concerto's logical schema (the Picard 2.x canonical fields from
`METADATA_PLAN.md` §2.4 + [Picard Tag Mapping](https://picard-docs.musicbrainz.org/en/appendices/tag_mapping.html))
maps to each container as follows. TagLib's `PropertyMap` already
round-trips these exact keys for Vorbis/ID3/MP4 — we audit per-format
gaps explicitly rather than relying on TagLib defaults.

| Concerto field | Vorbis (FLAC/OGG) | ID3v2 (MP3) | MP4 (M4A/ALAC/AAC) | WAV (INFO) | AIFF |
| --- | --- | --- | --- | --- | --- |
| `TITLE` | `TITLE` | `TIT2` | `©nam` | `INAM` | `NAME` |
| `ARTIST` | `ARTIST` | `TPE1` | `©ART` | `IART` | `AUTH` |
| `ALBUMARTIST` | `ALBUMARTIST` | `TPE2` | `aART` | (none) | (none) |
| `ALBUM` | `ALBUM` | `TALB` | `©alb` | `IPRD` | (none) |
| `DATE` | `DATE` | `TDRC` (2.4) / `TYER` (2.3) | `©day` | `ICRD` | (none) |
| `ORIGINALDATE` | `ORIGINALDATE` | `TDOR` / `TORY` | `----:com.apple.iTunes:ORIGINALDATE` | (none) | (none) |
| `LABEL` | `LABEL` | `TPUB` | `----:com.apple.iTunes:LABEL` | (none) | (none) |
| `CATALOGNUMBER` | `CATALOGNUMBER` | `TXXX:CATALOGNUMBER` | `----:com.apple.iTunes:CATALOGNUMBER` | (none) | (none) |
| `BARCODE` | `BARCODE` | `TXXX:BARCODE` | `----:com.apple.iTunes:BARCODE` | (none) | (none) |
| `ASIN` | `ASIN` | `TXXX:ASIN` | `----:com.apple.iTunes:ASIN` | (none) | (none) |
| `TRACKNUMBER` | `TRACKNUMBER` | `TRCK` | `trkn` (binary) | `ITRK` | (none) |
| `DISCNUMBER` | `DISCNUMBER` | `TPOS` | `disk` (binary) | (none) | (none) |
| `COMPOSER` | `COMPOSER` | `TCOM` | `©wrt` | (none) | (none) |
| `COMPOSERSORT` | `COMPOSERSORT` | `TSOC` / `TXXX:COMPOSERSORT` | `soco` | (none) | (none) |
| `CONDUCTOR` | `CONDUCTOR` | `TPE3` | `----:com.apple.iTunes:CONDUCTOR` | (none) | (none) |
| `PERFORMER` (multi) | repeated `PERFORMER` | `TMCL` (2.4) + `TIPL` | `----:com.apple.iTunes:PERFORMER` ×N | (none) | (none) |
| `WORK` | `WORK` | `TXXX:WORK` | `©wrk` | (none) | (none) |
| `MOVEMENTNAME` | `MOVEMENTNAME` | `MVNM` (2.4) | `©mvn` | (none) | (none) |
| `MOVEMENT` | `MOVEMENT` | `MVIN` (2.4) | `©mvi` (binary) | (none) | (none) |
| `MOVEMENTTOTAL` | `MOVEMENTTOTAL` | `MVIN` n/total | `©mvc` (binary) | (none) | (none) |
| `SHOWMOVEMENT` | `SHOWMOVEMENT` | `TXXX:SHOWMOVEMENT` | `shwm` (binary 0/1) | (none) | (none) |
| `ISRC` | `ISRC` | `TSRC` | `----:com.apple.iTunes:ISRC` | (none) | (none) |
| `GENRE` (multi) | repeated `GENRE` | `TCON` | `©gen` | `IGNR` | (none) |
| `MUSICBRAINZ_*ID` (Album/RG/Disc/Work/Artist/AlbumArtist) | same key in Vorbis | `TXXX:MusicBrainz <Name> Id` | `----:com.apple.iTunes:MusicBrainz <Name> Id` | (none) | (none) |
| `MUSICBRAINZ_TRACKID` | `MUSICBRAINZ_TRACKID` | `UFID:http://musicbrainz.org` | `----:com.apple.iTunes:MusicBrainz Track Id` | (none) | (none) |

WAV/AIFF native chunks are minimal; TagLib also reads an embedded ID3
in either when present (some encoders write `id3 `/`ID3 ` chunks).

### 4.3 Format ↔ tag-frame mapping (write)

Same table runs the other direction under writeback. Quirks to flag:

- **MP3: write ID3v2.4** (UTF-8, has `TDRC` / `TMCL` / `MVNM`); read
  2.3 too (no UTF-8, no `TMCL`, no movement frames). TagLib upgrades
  2.3 → 2.4 on save when configured.
- **MP4 `PERFORMER` with role attributes** has no native field
  matching Picard's "Name (instrument)" pattern. TagLib stores it as
  repeated freeform atoms under `----:com.apple.iTunes:PERFORMER`,
  each atom holding the full "Name (instrument)" string. iTunes/Music.app
  won't see this surface; Picard and Strawberry both interpret it
  identically — the cross-tool convention. Document in writeback help.
- **ID3 multi-value**: 2.4 supports null-byte-separated multi-values
  within a frame; 2.3 doesn't, so any 2.3-only re-save joins with `;`.
  Acceptable because we always save as 2.4.
- **WAV / AIFF writeback** deferred to v2 — embedded-ID3-in-WAV is
  fragile across consumers. Read-only in v1.

### 4.4 `AudioTagIo` — the architecture slot

A single wrapper in `src/AudioTagIo.{h,cpp}`, backed by TagLib's
`FileRef` + `PropertyMap`:

```cpp
namespace concerto::metadata {

enum class TagKey { Title, Artist, AlbumArtist, Album, Date,
    OriginalDate, Label, CatalogNumber, Barcode, Asin, TrackNumber,
    DiscNumber, Composer, ComposerSort, Conductor, Performer, Work,
    MovementName, Movement, MovementTotal, ShowMovement, Isrc, Genre,
    MusicBrainzAlbumId, MusicBrainzReleaseGroupId, MusicBrainzDiscId,
    MusicBrainzTrackId, MusicBrainzWorkId, MusicBrainzArtistId,
    MusicBrainzAlbumArtistId,
    ConcertoPipelineVersion, ConcertoSource, ConcertoEnrichedAt /* §A */ };

struct AudioTags {
    QHash<TagKey, QStringList> values;   // multi-valued for PERFORMER/GENRE
    double duration = 0.0;
    QString format;                       // "flac" | "mp3" | "m4a" | ...
};

class AudioTagIo {
public:
    static std::optional<AudioTags> readTags(const QString& path);
    static bool writeTags(const QString& path, const AudioTags& tags);
};

} // namespace
```

`readTags` pulls `PropertyMap` plus the format-specific extension for
MP4 freeform atoms (the `----:com.apple.iTunes:*` keys that some TagLib
versions don't surface through `PropertyMap`). `FlacTags.cpp` stays as
a fast-path optimisation for the rip pipeline's hot FLAC read path;
library-flow reads always go through `AudioTagIo`. `writeTags` always
emits the §A.1 provenance marker.

### 4.5 Writeback policy: opt-in, default off

Mutating user files by default is wrong for a player: round-trips
through Roon/foobar can clobber player-specific quirks, files in the
system-managed iTunes/Music.app hierarchy can be moved by macOS, a
writeback bug corrupts irreplaceable files, and some users object to
the modification on principle.

**Default: sidecar-only. Writeback off.** Under `Settings → Library →
Write enriched tags to files`:

1. **Augment, not overwrite.** Never replace a non-empty user tag.
   Exceptions: `MUSICBRAINZ_*` IDs (we just looked them up) and
   `CONCERTO_*` markers (§A.1, ours to manage).
2. Tag-block backup per file at
   `~/Library/Application Support/Concerto/library/tag-backups/<hash>.tagbak`,
   90-day retention.
3. Per-folder "Revert tag writeback" action restores from backup.

Sidecar is the source of truth; writeback is a slow optional
projection. Format-aware via TagLib: FLAC = Vorbis, MP3 = ID3v2.4,
M4A = MP4 atoms, OGG = Vorbis, WAV/AIFF = read-only in v1.

---

## A. Provenance markers — "Concerto handled this" (UPDATED 2026-05-16 — multiformat + provenance)

When Concerto writes a tag bundle, it embeds a provenance marker.
Future identification passes recognise the marker and skip web lookup
entirely — the existing tags are trusted. This is the **fast path** for
every file Concerto has previously touched, and bounds steady-state MB
load to one round-trip per *newly-added* release.

### A.1 Schema — three granular tags

```
CONCERTO_PIPELINE_VERSION = 1
CONCERTO_SOURCE           = musicbrainz   (musicbrainz | cd-text | acoustid | stub | unknown)
CONCERTO_ENRICHED_AT      = 2026-05-16T22:36:00Z   (ISO-8601 UTC)
```

**Chosen: three granular tags.** Human-readable in any tag inspector
(mp3tag / kid3 / `ffprobe` / `metaflac --list`); trivially comparable;
bumping the version field needs no blob-parsing. Three tag entries per
file (few dozen bytes) is negligible.

Rejected: a single `CONCERTO_META_PROVENANCE` JSON blob (opaque to tag
tools, loses grep-ability) and a single concatenated string (parsing
string-with-separators across ID3v2.3 charset quirks and MP4 freeform
UTF-8 is more error-prone than three independent values).

The three fields cover the load-bearing roles: **version** gates the
trust check (§A.3), **source** drives merge logic and the debug pane
(§9.5), **timestamp** feeds the 30-day TTL (§7.6). New v2 fields
become new granular tags (`CONCERTO_CONFIDENCE`, etc.) and bump
`version` to `2`.

### A.2 Format-specific embedding via TagLib

| Format | How `CONCERTO_PIPELINE_VERSION=1` lands on disk |
| --- | --- |
| FLAC / OGG Vorbis | Vorbis comment `CONCERTO_PIPELINE_VERSION=1` |
| MP3 ID3v2.4 | `TXXX` frame, description=`CONCERTO_PIPELINE_VERSION`, value=`1` |
| MP4 / M4A | freeform atom `----:com.apple.iTunes:CONCERTO_PIPELINE_VERSION` = `1` |
| WAV / AIFF (with embedded ID3) | same as MP3 ID3v2.4 inside the format's ID3 chunk |

Same shape for `CONCERTO_SOURCE` / `CONCERTO_ENRICHED_AT`. TagLib's
`PropertyMap` round-trips these keys exactly. Apple Music.app's tag
editor doesn't surface MP4 freeform atoms — a feature, not a bug.

### A.3 Stage Z (trust) — the first stage of `FolderIdentifier`

Inserted before Stage A:

```
Stage Z (trust):
  for each audio file in folder:
    read tags via AudioTagIo
    if file has CONCERTO_PIPELINE_VERSION AND that value ≥ kConcertoPipelineVersion (=1)
       AND a valid MUSICBRAINZ_ALBUMID (UUID-shaped)
       AND that MBID is consistent across the folder's ≥60% quorum:
      → return { source: "concerto-trusted", release_mbid: <albumid>,
                 confidence: 100, skip_web: true }
  fall through to Stage A
```

Net effect: a Concerto-enriched file **never** triggers a web call on
re-open, even cold-cache. The marginal cost is zero — Stage A reads
the same tags anyway, Stage Z is a check on the same `AudioTags`.
Quorum (≥60%, same as Stage A) prevents a single Concerto-enriched
file in a 13-file mixed folder from spoofing the whole folder as
trusted.

### A.4 The non-writeback case — DB-as-authority

When writeback is off (the default), or for read-only formats
(WAV/AIFF in v1), the file can't carry the marker. The **library DB
is the authority** for these files — already the load-bearing
mechanism in §6.2's content-hash schema. Making it explicit:

When `FolderIdentifier` succeeds for a non-writeback file, the result
lands in `releases` (keyed by MBID) and in `files` (content-hash →
MBID). On the next folder open, the per-file content-hash lookup
hits the DB; if `releases.pipeline_version ≥ kConcertoPipelineVersion`,
Stage Z trusts the DB row same as an in-file marker.

| Scenario | In-file marker | DB-as-authority |
| --- | --- | --- |
| FLAC/MP3/M4A/OGG + writeback on | `CONCERTO_*` tags | also written to `releases` |
| Any format + writeback off (default) | (none) | DB content-hash row is the only marker |
| WAV/AIFF (read-only in v1) | (none — by design) | DB content-hash row |

DB-as-authority is the **default** path. In-file markers are the bonus:
portable across machines, survive DB loss, verifiable with any tag
editor.

### A.5 Schema addition for the DB-side trust column

Fold into the §6.3 schema on initial create:

```sql
ALTER TABLE releases ADD COLUMN pipeline_version INTEGER NOT NULL DEFAULT 1;
ALTER TABLE files    ADD COLUMN writeback_at     INTEGER;  -- UNIX time; NULL = never written back
```

`pipeline_version` drives Stage Z's DB-side check. `writeback_at` tracks
whether the file currently carries `CONCERTO_*` on disk (for the
"re-write tags now" debug action and the §A.4 distinction).

### A.6 Versioning bump path

When v2 adds new fields:

- v2 bumps `kConcertoPipelineVersion` 1 → 2.
- **v1 files seen by v2**: re-fetch the release (one cheap MB
  second-hop) to land the new fields and bump the in-file/DB version
  to 2. Existing v1 fields aren't re-fetched unless their semantics
  changed — per-field decision via a small in-code
  `kConcertoFieldsByVersion` manifest.
- **v2 files seen by v1**: trusted as-is (their version ≥ what v1
  expects). v1 reads what it knows, ignores the v2 fields it doesn't.
  Forward-compatible.

```cpp
constexpr int kConcertoPipelineVersion = 1;  // bump on field-add

bool isTrusted(const AudioTags& tags) {
    bool ok = false;
    const int v = tags.get(TagKey::ConcertoPipelineVersion).toInt(&ok);
    return ok && v >= kConcertoPipelineVersion
        && !tags.get(TagKey::MusicBrainzAlbumId).isEmpty();
}
```

Same shape as `MetadataCache`'s 30-day TTL (§7.6): newer is fine,
older triggers re-fetch.

### A.7 Why the marker is unconditional

Not gated behind a config flag. Emitted on every bundle the rip
pipeline produces, regardless of which Stage (1/2/3/4) supplied the
data. Three namespaced tag entries are inert to every tool that doesn't
recognise them (Picard, Strawberry, foobar, mp3tag, Apple Music's tag
editor); a user who round-trips a Concerto-ripped folder through
another tool and back still gets Stage Z trust on re-open. The marker
is the "Concerto remembers" mechanism even across foreign tools.

This implementation already landed in `src/FlacTags.cpp::buildVorbisTags()`
and `src/MetadataModel.cpp::debugDumpTrack()` on 2026-05-16; see §12
Step 0.

---

## 5. Sources we use and don't use

This is a deliberate subset of `METADATA_PLAN.md` §1. The folder flow
uses **a strict subset** of the rip flow's sources — the constraint
is identical, and adding sources increases the surface area to
maintain across two paths.

### 5.1 MusicBrainz — primary (same as rip-time)

The disc-ID endpoint is replaced by:

- **Direct release fetch** (`/ws/2/release/<mbid>`) — Stages A, B (via
  cache) of §2.2.
- **Release-group fetch** (`/ws/2/release-group/<mbid>?inc=releases`)
  — Stage C variant.
- **Release search** (`/ws/2/release?query=...&fmt=json`) — Stages C
  and D. New for the folder flow; not currently in `MusicBrainz.cpp`.

Rate limit is the same: 1 req/sec per IP (anonymous). The new piece
of math: **for a 10,000-track / ~700-release library, the MB cost is
~700 / 1 req/sec / 60 = ~12 minutes of MB time** if every release
needed a full second-hop fetch and every fetch was a cache miss. In
practice cache hits dominate after the first sweep, so steady-state
is ~1 request per *newly-added* release.

The search endpoint is unaffected by the disc-ID-specific limits in
`METADATA_PLAN.md` §1.1 ("disc-ID endpoint forbids work-level-rels"):
the second-hop release fetch from `Client::fetchRelease` already
requests the full set of inc params. So once we have an MBID, the
folder pipeline reuses the existing fully-tagged path.

### 5.2 Cover Art Archive — pair with MB (same as rip-time)

Already used at rip-time for the §3.3.1 cover-art-penalty scoring.
For folder enrichment, we additionally use it to fetch the 500px
thumbnail for the library UI's album view. Same anonymous,
no-auth, mixed-image-license rules apply.

`coverArtUrl` is already a field on `AlbumMeta`. No new shape needed.

### 5.3 AcoustID / Chromaprint — deeper in the chain than at rip-time

Rip-time Stage 3 fingerprints **track 1 only** because that's all
that's available (the rip is in-flight; only track 1 is in memory at
that moment). Folder-time Stage E can do **all tracks**, and the
intersection of recording-MBID-to-release sets is what disambiguates.

This requires Chromaprint to be vendored, which the rip flow already
needs for its own Stage 3 (see `METADATA_PLAN.md` §1.3a.1). **Don't
vendor it twice.** The library flow shares the same `Fingerprinter`
class.

LGPL caveat (repeated for clarity): Chromaprint is MIT *when built
with `-DAUDIO_PROCESSOR_LIB=vDSP` on macOS*. The rip plan already
locks this configuration in (`METADATA_PLAN.md` §1.3a.1, §5). The
folder flow inherits it.

### 5.4 Apple Music — deferred from v1

The rip pipeline includes Apple Music Stage 1.5 enrichment
(`METADATA_PIPELINE_AUTOMATED.md` "Stage 1.5"). For folder enrichment
**we do not include Apple Music in v1**.

Reasons:

1. **The rip flow's Stage 1.5 fires on ~30–50% of classical rips at a
   gating cost of one MB second-hop already in hand.** For the folder
   flow, applying the same gate would also be reasonable — but it adds
   significant complexity (JWT signer, gate logic, merge logic, the
   `.p8` build-time integration) to a path that's already complex
   enough on its own.
2. **The folder flow's downstream consumers (library UI) don't yet
   exist.** Until the basic flow is shipping and we see real
   classical-sparse folders in field testing, optimizing for Apple's
   `composerName` / `workName` / `movementName` is premature.
3. **The dev's instruction was "no Apple Music in v2; deferred."**
   Take this literally for the folder flow.

When we *do* add it: the integration point is `LibraryResolver`
post-MB-flatten, identical to the rip flow's gate (`AppleMusic.h::
shouldEnrich`). The same `AppleMusicJwt` signer can be reused; both
flows live in the same process. Add it in a "v2 Apple Music" pass,
not in the v1 library pass.

### 5.5 What we explicitly skip in the folder flow

Mirroring `METADATA_PLAN.md` §1's drop list:

- **Discogs** — same PAT-per-user problem; no.
- **gnudb / freedb** — no composer field, useless for classical.
- **Spotify / Last.fm / Tidal / Deezer / Qobuz** — pop-first or
  partner-only; weak on classical.
- **CD-TEXT** — there is no disc to read from; the folder flow has no
  analogue.

---

## 6. Storage architecture — sidecar SQLite, file writeback opt-in

This is question 2. The dev's instinct ("maybe sidecar") is correct.
The right *shape* of the sidecar is the subtle part.

### 6.1 The options reviewed

**Pure file-tag writeback.** Every resolved field gets written into
the file's Vorbis comments. Pros: standard, portable, every other tool
sees the enriched data. Cons: mutates user files, risks of overwrite,
slow on bulk-tag operations (writing 10,000 file headers takes
several minutes of disk I/O), audit trail is in-file only, no
library-wide query without re-reading every file.

**Pure sidecar DB.** Never touch user files. App-local DB keyed by
some identifier. Pros: fast, non-destructive, supports library-wide
queries. Cons: the data isn't visible to other tools, lost if the
user moves the app to a new machine without copying it, doesn't
survive a file move/rename if path-keyed.

**Hybrid.** Sidecar always populated, files written-back opt-in.
Pros: best of both, user has control, library queries fast. Cons:
slightly more implementation complexity, two-source-of-truth question.

**User-togglable.** Same as hybrid but framed as a toggle. Equivalent
in practice.

**Recommendation: Hybrid, sidecar authoritative.** The sidecar
SQLite DB is the source of truth. File writeback is opt-in and
*augmentative* (never destructive). The sidecar is what the library
UI reads; files are projected outputs.

The dev hinted at this in the brief ("Pure sidecar DB...interaction
with existing user-written tags") — we keep the user's tags intact,
overlay our enrichment on top in the sidecar, and surface a single
toggle for when the user wants the enrichment to materialize in the
file.

### 6.2 Path-keyed vs content-hash-keyed cache

The dev called this out specifically. The question: when the user
moves or renames a file, does the cache invalidate?

**Path-keyed.** Simple. Fast lookup. Loses everything when the user
moves the file to another folder or renames it (which they will). The
file's enrichment has to be re-computed from scratch every time, and
the work isn't preserved across reorganizations.

**Content-hash-keyed.** Survives moves and renames. The hash is over
the **audio data**, not the tag block — that way edits to user tags
don't invalidate the cache, and tag writeback doesn't either. The
hash is computed once per file (the first time we see it), stored
alongside the file's path in a `file_index` table, and used as the
foreign key for any per-file rows.

**Recommendation: content-hash-keyed primary, path-indexed secondary.**

Concretely:

- The `files` table has columns `(content_hash PRIMARY KEY, path,
  format, duration_sec, last_seen_ts, ...)`. The hash is SHA-256 over
  the file's audio frames *only* (skip the tag block and STREAMINFO
  header — they change with tag edits, but the frames don't).
- All per-file rows in other tables (`track_recording`,
  `track_acoustid_fp`) foreign-key on `content_hash`.
- The `path` column is indexed but **not** unique — duplicates are
  fine. We index on path so opening a folder by path is fast; we look
  up by hash for stability.
- When the path-index lookup misses (newly-seen file), we hash it,
  index it, and resolve via §2.2.
- When the hash exists but the path is new (the user moved the file),
  the lookup succeeds without re-resolution. The path column is
  updated to reflect the new location.

Hashing cost: one full file read. ~1 GB/sec on an SSD; a 100MB FLAC
hashes in ~100ms. The first scan of a 500-album library: ~5-10
minutes of hashing. Subsequent re-scans only hash *new* files. This
is run on the background sweep thread (§7.3); the UI never blocks.

A subtle point: the audio-frames-only hash is *not* the same as the
file's content hash. A "tag-edited" duplicate of the same FLAC will
have the same audio-frames hash as the original, which is exactly
what we want — they're the same recording, just re-tagged. If the
user wants to distinguish them, the (path, hash) pair does that.

The `MUSICBRAINZ_TRACKID` Vorbis tag is *not* a great key for this
purpose: many of the user's files may not carry it, and it's the
recording MBID (not the file content), so a re-encode at a different
quality is "the same recording" by MB's lights but a different file by
ours. Use the audio-frame hash.

### 6.3 SQLite schema sketch

The dev asked for a 5–8 table sketch. Here it is:

```sql
-- Primary file index. Every audio file the library has ever seen.
-- content_hash is SHA-256 over audio frames only (tag-edit stable).
CREATE TABLE files (
  content_hash      TEXT PRIMARY KEY,
  path              TEXT NOT NULL,             -- absolute file path
  format            TEXT NOT NULL,             -- 'flac' | 'mp3' | ...
  duration_sec      REAL NOT NULL,
  bit_depth         INTEGER,
  sample_rate       INTEGER,
  channel_count     INTEGER,
  size_bytes        INTEGER,
  last_seen_ts      INTEGER NOT NULL,          -- UNIX time of last scan
  first_seen_ts     INTEGER NOT NULL,
  -- Forward link into the library tree. Set after identification;
  -- NULL means "unresolved".
  release_mbid      TEXT,
  track_position    INTEGER,                   -- 1-based within medium
  medium_position   INTEGER                    -- 1-based within release
);
CREATE INDEX idx_files_path           ON files(path);
CREATE INDEX idx_files_release        ON files(release_mbid, medium_position, track_position);
CREATE INDEX idx_files_last_seen      ON files(last_seen_ts);

-- One row per resolved release. Mirrors AlbumMeta album-level fields.
CREATE TABLE releases (
  release_mbid        TEXT PRIMARY KEY,
  release_group_mbid  TEXT,
  title               TEXT,
  artist_credit       TEXT,
  album_artist        TEXT,
  album_artist_mbid   TEXT,
  date                TEXT,
  original_date       TEXT,
  country             TEXT,
  barcode             TEXT,
  catalog_number      TEXT,
  label               TEXT,
  asin                TEXT,
  cover_art_url       TEXT,
  source_tag          TEXT NOT NULL,           -- 'musicbrainz' | 'cd-text' | 'acoustid' | 'unresolved' | 'user'
  confidence          INTEGER NOT NULL,        -- 0..100
  pick_reason         TEXT,                    -- from MetadataScoring
  scoring_log_json    TEXT,                    -- the rip-time scoringLog as a JSON blob
  pipeline_log_json   TEXT,                    -- the rip-time pipelineLog
  raw_mb_json         TEXT,                    -- raw second-hop response
  cached_at           INTEGER NOT NULL         -- UNIX time
);
CREATE INDEX idx_releases_rg          ON releases(release_group_mbid);
CREATE INDEX idx_releases_album_artist ON releases(album_artist);
CREATE INDEX idx_releases_label       ON releases(label);

-- One row per medium per release. Multi-disc sets expand here.
CREATE TABLE mediums (
  release_mbid        TEXT NOT NULL,
  medium_position     INTEGER NOT NULL,        -- 1-based
  track_count         INTEGER NOT NULL,
  disc_subtitle       TEXT,
  mb_disc_id          TEXT,                    -- if known (CD rip)
  PRIMARY KEY (release_mbid, medium_position),
  FOREIGN KEY (release_mbid) REFERENCES releases(release_mbid)
);
CREATE INDEX idx_mediums_discid       ON mediums(mb_disc_id);

-- One row per track on a release (recording, not file). The 'file'
-- column joins to the user's files table — multiple files can claim
-- the same recording (re-encodes); FK is by content_hash.
CREATE TABLE recordings (
  recording_mbid      TEXT PRIMARY KEY,
  title               TEXT,                    -- movement-level
  work_mbid           TEXT,                    -- nullable
  duration_ms         INTEGER,
  isrc                TEXT
);
CREATE INDEX idx_recordings_work      ON recordings(work_mbid);

-- A track is a recording on a specific (release, medium, position).
-- One row per (release, medium, position) — the same recording can
-- appear under different positions in different releases.
CREATE TABLE release_tracks (
  release_mbid        TEXT NOT NULL,
  medium_position     INTEGER NOT NULL,
  track_position      INTEGER NOT NULL,
  recording_mbid      TEXT NOT NULL,
  movement_name       TEXT,
  movement_number     INTEGER,
  movement_total      INTEGER,
  PRIMARY KEY (release_mbid, medium_position, track_position),
  FOREIGN KEY (release_mbid, medium_position) REFERENCES mediums(release_mbid, medium_position),
  FOREIGN KEY (recording_mbid) REFERENCES recordings(recording_mbid)
);

-- Works (compositions). Distinct from recordings. The composer rel
-- lives here.
CREATE TABLE works (
  work_mbid           TEXT PRIMARY KEY,
  title               TEXT,                    -- full work-title from MB
  composer_mbid       TEXT,
  composer_name       TEXT,
  composer_sort       TEXT,
  parent_work_mbid    TEXT                     -- for movement→parent chains; nullable
);
CREATE INDEX idx_works_composer       ON works(composer_mbid);
CREATE INDEX idx_works_parent         ON works(parent_work_mbid);

-- Performers, typed. One row per (recording, performer, role) — a
-- track with conductor + soloist + orchestra has three rows here.
-- Role is the MB relation type: 'conductor' | 'performing orchestra'
--   | 'instrument' | 'vocal' | 'arranger' | 'chorus master' | ...
CREATE TABLE recording_performers (
  recording_mbid      TEXT NOT NULL,
  artist_mbid         TEXT NOT NULL,
  role                TEXT NOT NULL,
  ordinal             INTEGER NOT NULL,        -- preserves MB order
  instrument_or_voice TEXT,                    -- the relation 'attributes' joined with ', '
  PRIMARY KEY (recording_mbid, artist_mbid, role, ordinal),
  FOREIGN KEY (recording_mbid) REFERENCES recordings(recording_mbid)
);
CREATE INDEX idx_performers_artist    ON recording_performers(artist_mbid);
CREATE INDEX idx_performers_role      ON recording_performers(role);

-- Artists. Composer, conductor, soloist, orchestra — all share this
-- table. The 'kind' column reflects MB's artist 'type' attribute.
CREATE TABLE artists (
  artist_mbid         TEXT PRIMARY KEY,
  name                TEXT NOT NULL,
  sort_name           TEXT,
  kind                TEXT                     -- 'Person' | 'Group' | 'Orchestra' | 'Choir' | ...
);
CREATE INDEX idx_artists_sort         ON artists(sort_name);

-- Genres. Apple-only at rip-time; in v1 of library, only populated
-- from existing-tag scrape (the GENRE Vorbis tag the user already has).
CREATE TABLE release_genres (
  release_mbid        TEXT NOT NULL,
  genre               TEXT NOT NULL,
  source              TEXT NOT NULL,           -- 'user-tag' | 'apple-music' | 'musicbrainz'
  PRIMARY KEY (release_mbid, genre, source),
  FOREIGN KEY (release_mbid) REFERENCES releases(release_mbid)
);

-- AcoustID fingerprints, cached per file content-hash. Decoupled
-- from `files` to make the fingerprint reusable across path changes.
CREATE TABLE acoustid_fingerprints (
  content_hash        TEXT PRIMARY KEY,
  fingerprint_base64  TEXT NOT NULL,
  duration_sec        INTEGER NOT NULL,
  last_lookup_ts      INTEGER,
  matched_recording   TEXT,                    -- nullable
  match_score         REAL                     -- 0..1
);
```

Why this shape:

- **Queries the user will actually want.** "All Pollini" is
  `SELECT * FROM recording_performers WHERE artist_mbid = <pollini> JOIN
  recordings JOIN release_tracks JOIN releases`. "All Brahms symphonies"
  is `SELECT * FROM works WHERE composer_mbid = <brahms> AND title LIKE
  'Symphony%'`. Both are seconds on a 100k-track DB with the indices
  above.
- **Foreign-keyed**, so deleting a release cascades cleanly.
- **Source field on every external value** (`releases.source_tag`,
  `release_genres.source`) — the dev needs to know which row came from
  MB vs the user's own tag vs (later) Apple, both for trust scoring
  and for the "merge" / "prefer MB" UI in §9.
- **Raw MB JSON is stored** — same as the rip cache (`rawMb` in
  `MetadataCache`). Lets us re-derive without re-fetching. Storage
  cost: ~20-50 KB per release. 700 releases = ~30 MB. Fine.

A flat-JSON approach would skip the schema work but at the cost of
every library query becoming "load all 700 JSON files, parse them,
filter in memory" or "build a parallel index file". SQLite is the
right call from day one. SQLite is also already in the dev's stack
implicitly (Qt 6 ships `QtSql`, no new dep).

### 6.4 Persistence locations

```
~/Library/Application Support/Concerto/
  metadata-cache/                 # rip-time JSON cache (existing)
    disc/<discid>.json
    release/<mbid>__<pos>.json
  library/
    library.db                    # NEW — the sidecar
    library.db-wal
    library.db-shm
    tag-backups/                  # writeback opt-in backups
      <content-hash>.tagbak
  pending-submissions.jsonl       # rip-time stub queue (existing)
```

`MetadataCache::defaultDir()` already points at `Concerto/metadata-cache`
(it lives under `GenericDataLocation`, which on macOS resolves to
`~/Library/Application Support/`). The library files sit next to it.

The DB is opened by the singleton `LibraryDatabase` instance on
process start. WAL mode for concurrency (multiple readers + one
writer); writer is the `LibrarySweeper` thread (§7).

### 6.5 Versioning and migration

Pre-emptively add a `schema_version` table with one row. v1 is
version `1`. Migrations are forward-only, applied at app startup
inside a transaction. The dev's hand: the schema *will* evolve as we
add features (rating, play counts, Apple Music IDs). Save the
migration scaffolding before it's painful.

```sql
CREATE TABLE schema_version (
  version INTEGER PRIMARY KEY,
  applied_at INTEGER NOT NULL
);
INSERT INTO schema_version VALUES (1, strftime('%s', 'now'));
```

A migration is a numbered `.sql` file under
`resources/library-migrations/0002_add_play_counts.sql`. On startup,
read `schema_version`, find any unapplied numbered migrations under
the resource path, apply them in order, append a version row each.

---

## 7. Lookup timing — live vs precomputed vs background

This is question 3. The dev's actual question was a smear of "live"
through "background sweep" and "I'm not sure." Let's pick.

### 7.1 The options

- **Live, lazy, per-track.** Only resolve when the user clicks/plays
  a track. Lowest upfront cost. ~500ms delay on first display of an
  unresolved track. Don't precompute for the 10,000 tracks the user
  might never touch.
- **Live, eager, per-folder-open.** Resolve everything in the folder
  as soon as it's opened. ~1 round-trip per release (~500ms uncached,
  ~10ms cached). Sub-second for a single album open.
- **Background sweep.** Async resolution that runs over the next
  minutes/hours after a folder is added. UI shows partial data and
  fills in as it arrives. Necessary for bulk library-add.
- **Precomputed at "library scan" time.** Explicit "Scan library"
  command, blocks with a progress bar.

### 7.2 The recommendation: lazy-eager hybrid + background sweep

**Heuristic:** the right strategy depends on **how the user
discovered the folder**, exactly as the brief intuited.

1. **User opens a single folder (typical case).** Eager. Resolve
   everything in that folder synchronously (with a 5-second timeout
   per release). One MB request, one second-hop. Sub-second on cache
   hit. A simple "Identifying…" overlay covers the rare uncached
   case. This is `PlaylistModel::openFolder()`'s current entry point;
   we hook in there.

2. **User opens a parent folder containing many album folders.** The
   single-folder eager path triggers per-subfolder. The first ~5–10
   subfolders are resolved synchronously; the rest are queued for the
   background sweep. The library UI shows "12 of 80 albums
   resolving…" with progressive updates. Threshold (5–10) is the
   number that fits in our MB rate budget within the user's
   tolerance window (~5 seconds).

3. **User bulk-imports a 500-folder library.** A dedicated "Scan
   library" path that explicitly kicks the sweep. The user sees
   progressive enrichment; the UI is fully usable from the start
   (existing tags drive display until enrichment lands). At
   ~1 sec/release uncached + ~12 minutes per 700 releases, this is
   the once-per-library-import case. Show a non-blocking notification
   when complete.

4. **Per-track lookup is never triggered by playback.** The dev's
   intuition ("we'll look it up anyway each time, etc., something
   like that, so it would be plenty fast") was right *if* the lookup
   is from the cache. A cache hit is microseconds. A cache miss
   followed by an MB round-trip and a second hop, on the
   click-to-play path, is a noticeable ~500ms hiccup that we should
   avoid. Make sure the eager resolve in step 1 is *complete* before
   the user can click-to-play a track in that folder.

### 7.3 The `LibrarySweeper`

A long-lived `QThread` started at app boot. Owns a job queue
(`QQueue<SweepJob>`) and a `QNetworkAccessManager`. Pulls jobs at the
MB rate cap (1/sec), runs them through the standard pipeline, writes
results to the DB.

Job kinds:

| Job | When enqueued |
| --- | --- |
| `ResolveFolder(path)` | User opens a parent folder with many subdirs; bulk-import |
| `ResolveRelease(mbid)` | An A-stage hit; just need the second-hop fetch |
| `Fingerprint(content_hash)` | User invokes "Re-identify"; rare |
| `CoverArtFetch(release_mbid)` | After release commit, fetch the CAA thumb in the background |
| `MoveScan(folder)` | The user moves files; re-index paths |

The sweeper's invariants:

- One MB request per second (the rate-cap; align with rip flow).
- Cancellable per-job (the user closes the folder, jobs for it stop).
- Always commits *some* AlbumMeta — falls back to "unresolved" if the
  whole chain fails. Mirrors the rip-time "Stage 4 stub" property.
- Persistent across restarts: the job queue is itself in SQLite
  (`sweep_queue` table); on startup, resume in-flight jobs.

### 7.4 Concrete cost math

The dev asked for quantification. Concrete numbers, MB rate cap of
1 req/sec:

| Library size (tracks) | Releases (assume 15 tracks/release) | Stage A hit rate | MB second-hop calls | Wall-clock sweep |
| --- | --- | --- | --- | --- |
| 1,000 | ~67 | 50% | ~67 | ~70 seconds |
| 10,000 | ~667 | 50% | ~667 | ~11 minutes |
| 50,000 | ~3,300 | 50% | ~3,300 | ~55 minutes |
| 100,000 | ~6,700 | 50% | ~6,700 | ~110 minutes |

(Stage A hits still issue one second-hop fetch — the second-hop is
what produces the full work-rels needed for enrichment. The disc-ID
lookup is *not* needed; we already have the MBID.)

With cache hits over time the steady-state becomes "1 MB request per
*newly-added* release", which is trivial. So **even a 50k-track
library is a one-hour, one-time cost.** That's well within
acceptable for an explicit "Scan library" action.

For a 10k-track library, the user sees the sweep complete in under
15 minutes from app launch. The library UI is fully usable
throughout. This is the dev's threshold; we're comfortably under it.

### 7.5 Multi-disc and box-set handling

Multi-disc sets have two physical layouts:

- **Per-disc folder** (typical): `Mahler Box/CD01/`, `Mahler Box/CD02/`,
  etc. Each folder has its own `DISCNUMBER` tag and its own MBID
  references. The library flow resolves each disc independently, then
  joins them by `release_mbid + medium_position` in the DB. The UI
  shows the parent box as one logical album (group by
  `release_group_mbid` or by `release_mbid` depending on whether the
  reissues are catalogued separately on MB).
- **One folder, all discs intermixed**: rare but happens. Detection:
  the track count exceeds any plausible single-disc release (most CDs
  are ≤80 minutes / ~30 tracks); a `DISCNUMBER` tag is present and
  ranges over multiple values. Split conceptually into virtual
  per-disc subfolders in the DB (the `files.medium_position` column
  separates them). The physical layout stays as-is.

The existing rip-time `scoring::pickMedium()` already handles the
"which medium of a multi-medium release does this disc-ID belong to"
question. The library flow does the analogous task with the file
quorum: if all files share `DISCNUMBER=3`, that's our medium. If files
have mixed `DISCNUMBER`s, the folder spans multiple mediums.

### 7.6 What invalidates cache rows

Mirroring `MetadataCache`'s 30-day TTL — but more carefully, because
the library's DB isn't a "cache" exactly; it's a curated store. We
invalidate:

- **30-day-old `releases.cached_at`**: re-fetch on next access.
- **User explicit refresh**: a "Re-fetch metadata" right-click action
  on a release.
- **MBID mismatch**: if the user manually changes a file's
  `MUSICBRAINZ_ALBUMID` tag, on next scan the file's `release_mbid`
  pointer changes. Old enrichment isn't deleted — it's just no longer
  pointed at.

We **never silently overwrite** an MB-derived row from an unverified
source. The rip pipeline already maintains this invariant in the JSON
cache; the SQL store does likewise.

---

## 8. Edge cases

### 8.1 Mismatched track counts

Folder has 13 FLACs, MB release says 14 tracks. Options:

- Treat as unknown (Stage F).
- Try anyway — fuzzy match on track count ±1, prefer the candidate
  whose recording-MBIDs / durations align with what we have.
- Surface a warning badge.

**Recommendation: fuzzy match, ±2 tracks tolerance. Surface a
"track count off" badge in the UI** (yellow, not red). The most common
cause is hidden-track ripping artifacts or a known-track gap. The
duration-defender scoring weight (`MetadataScoring::Components::
durationDefender`) already handles the soft side of this — releases
that align poorly on duration get penalized.

If the count is off by more than ±2 tracks, fall to Stage F.

### 8.2 Compilations / box sets with mixed-release files

Folder has files from 3 different releases (e.g. a personal
playlist export). Detection: `MUSICBRAINZ_ALBUMID` quorum fails
(<60% on any single MBID), and ≥2 distinct MBIDs each have ≥30%.

**Recommendation: detect and surface, don't treat as a single thing.**
The DB schema already supports per-file release assignment
(`files.release_mbid` is per-row). The library UI groups files by
their assigned release; a folder containing files from 3 releases
shows 3 album cards. Drop a "mixed-content folder" tag on the parent
folder's display.

If the user wants this to be one "compilation" — that's a future
feature ("user-defined playlist"). Not v1.

### 8.3 Lossy formats (UPDATED 2026-05-16 — multiformat + provenance)

MP3, AAC/M4A, OGG, Opus. Concerto's playback path already handles all
of these via `QAudioDecoder` (CoreAudio/AudioToolbox on macOS), so the
library feature must enrich them on day one — not v1.1.

**Read AND write on day one via TagLib (§4).** The library flow uses
the single `AudioTagIo` wrapper for every format the player can play.
The previous "v1.1 ID3-read, v2 ID3-write" staging was written before
playback was multi-format. With playback already covering MP3 / AAC /
ALAC / WAV / AIFF, that staging produces a misleading first
impression on every non-FLAC folder.

ID3 modification is the technically painful part (rewriting the tag
block can change the file length and shift the audio start); TagLib
solves it correctly across formats, including the unsynchronization
and extended-header edge cases. The LGPL-2.1 dynamic-link pattern
(§4.1) is the standard closed-source pattern for TagLib and is the
same shape we already use for libFLAC.

WAV / AIFF writeback stays deferred (embedded-ID3-in-WAV is fragile
across consumers); read works through TagLib.

### 8.4 Files with no useful tags at all

`track01.flac` through `track13.flac`, no tags. Folder name is
`unknown_dump_2018/` or similar.

The chain reaches Stage D (TOC fingerprint). For a 13-track folder
with no further hints, the duration-defender score may or may not
disambiguate. If Stage D scores below threshold, **Stage E
(AcoustID) is the only hope.** Don't auto-fire it; surface a
"Couldn't auto-identify — run AcoustID?" prompt in the per-folder
UI. One-click action.

### 8.5 User's own previously-written tags from another tool (Picard, beets)

The user ran Picard on this folder five years ago; they have full
classical tags. Now we open it.

**Trust their tags as a baseline.** Specifically:

1. Read all tags into the in-memory `AudioFileTags` shape.
2. Run identification. If we resolve to the same MBID the user's tags
   point at (`MUSICBRAINZ_ALBUMID` matches our resolution), commit
   the MB-derived enrichment over the user's tags — they came from
   the same place, ours is fresher.
3. If we resolve to a *different* MBID (rare; only when the user's
   tag is wrong), prefer our resolution **but log the conflict**.
   Surface a "Different MB match found — keep user tag or accept
   new?" prompt for review. Most users will say "accept new"; some
   will say "keep mine" because they had a specific reason. Either
   way, never silently overwrite a user-set MBID.
4. If we *can't* resolve, fall back entirely to the user's tags.
   Even unenriched display is fine if the user had filled it in.

A subtle case: the user's tag has `COMPOSER=Beethoven`, our MB
resolution has `COMPOSER=Ludwig van Beethoven`. Both are correct;
ours is more canonical. Default: **ours wins on conflicts among
already-equivalent values.** For values the user *explicitly chose
differently* — there's no way to know "explicit" vs "inherited from
some old tool", so we default to MB-prefers and surface the conflict
log if the user wants to revisit.

### 8.6 Folders the user has marked "do not touch"

Some libraries have golden-copy albums the user manually curated. A
right-click "Lock metadata" action freezes the release row's
`source_tag` to `user`. The sweeper skips locked folders entirely —
not even cache refreshes fire.

Implementation: an `is_locked BOOLEAN` column on the `releases`
table, default 0. Locked releases are read-only for the resolver.

### 8.7 Disc-ID hash conflicts

Two different folders/releases can in principle share the same MB
disc-ID (the same TOC). The rip flow's §3.5 documents this. For the
folder flow, this is a non-issue: we don't compute disc-IDs from
folders (no TOC), and `MUSICBRAINZ_DISCID` tags in files are
post-hoc — they reflect what was on the CD, but the file is what we
trust.

### 8.8 Files that fail to read

Corrupted FLAC, partial download, etc. The reader returns
`nullopt`; we skip the file with a log line. The folder's track count
is reduced; identification proceeds with whatever survives.

### 8.9 Cross-machine sync

The user has Concerto on a laptop and a desktop. They want the
library DB to follow.

**Out of scope for v1.** The library DB lives in
`~/Library/Application Support`, which is *not* in iCloud or Dropbox
by default. Users who want sync put the data folder under iCloud
themselves — same as any other macOS app's data. SQLite has known
issues with concurrent multi-machine access (WAL files can corrupt),
so document this in a separate "library sync" plan, not here.

---

## 9. UI / UX surfaces

Question 6. **Just sketch — the dev decides the QML details.**

### 9.1 Library view (the new big thing)

A new top-level view alongside Playback / Rip. Layout:

```
[ sidebar: navigation ]                  [ main: album grid / list ]
  - All
  - Composers (with counts)
  - Performers (with counts)
  - Conductors (with counts)
  - Orchestras (with counts)
  - Works
  - Genres
  - Recent
  - Search (live filter)

Selecting a node populates the main pane:
  - "All" → 2D grid of every release, sortable by various keys
  - "Composers > Brahms" → all releases with any Brahms work
  - "Performers > Pollini" → all releases with Pollini in a performer role
  - "Works > Symphony No. 9 (Beethoven)" → all recordings of that work
```

Each release card shows the cover art (from CAA), title, primary
artist credit, year, and a small source badge (matches rip-time §6.5:
`MusicBrainz`, `AcoustID`, `Unknown`).

Click an album → expand to track list (movement-grouped if
`MOVEMENT`-tagged); double-click a track → play. Same playback path
as the existing `PlaylistModel.currentIndex` flow.

### 9.2 Folder-open interactive states

When the user opens a folder:

| State | Visible UI |
| --- | --- |
| **Resolving** | Folder card with skeleton placeholders. Track titles from existing tags. A subtle pulsing dot indicates lookup in progress. |
| **Resolved (MB)** | Standard view + small badge: `MusicBrainz` |
| **Resolved (cache)** | Standard view, no badge |
| **AcoustID-resolved** | Badge: `AcoustID` (lower-confidence hint) |
| **Unresolved** | Badge: `Unidentified`, plus a button: `Identify with AcoustID` (opt-in fingerprint run) |
| **Mixed content** | Folder shown as N separate cards (one per resolved release) |
| **Track count mismatch** | Yellow warning badge: `~14 tracks expected` |
| **MBID conflict with existing tag** | Yellow badge: `Tag mismatch` + `Review` button |

### 9.3 Library preferences

A new `Settings → Library` pane:

- `Write enriched tags to files` (toggle, default off; §4.5 writeback)
- `Fingerprint when no other identification works` (toggle, default
  on; surface the cost)
- `Background sweep speed` (slider: `Conservative (1 req/sec)` ↔
  `Polite (1 req/sec)`, only one real value since MB caps us; the
  slider is for when more sources land later)
- `Cache TTL` (dropdown: 30 days default, 7 / 30 / 90 / never)
- `Library location` (read-only display of the DB path; for
  troubleshooting)
- `Refresh all metadata` (button; nukes 30-day TTL and re-fetches)
- `Show identification debug pane` (toggle for developers — shows
  the scoring log under each release card)

### 9.4 Conflict resolution UX

When MB data disagrees with existing tags (§8.5 case), surface a
modal-style banner — *not* a blocking dialog — on the release card:

```
┌────────────────────────────────────────────────┐
│ Mahler Symphony No. 9 (Bernstein, NYP)         │
│ ⚠ Tag mismatch: file says "Symphony 9", MB     │
│   says "Symphony No. 9 in D Major". Prefer:    │
│   [ File tags ] [ MusicBrainz ] [ Always MB ]  │
└────────────────────────────────────────────────┘
```

Default action (Always MB) is one click. If "Always MB" is selected,
record the user's preference globally; future conflicts default to MB
without prompting until the user resets the preference. The actual
preference is a single bool — `prefer_musicbrainz_on_conflict` — in
QSettings, default `false`. This is the kind of decision that a power
user will set once and forget.

### 9.5 Identification debug pane (dev-only)

Behind a settings toggle (`Show identification debug pane`). For each
release, displays:

- The `releases.scoring_log_json` — same as rip-time §6.3 scoringLog
- The `releases.pipeline_log_json` — same as rip-time pipelineLog
- The path to the source files
- The path to the cache file (if rip-time JSON cache hit)
- The MB query URL used for resolution

Same information surface the rip flow already produces. The dev's
"why did it pick this?" debug surface, reused.

---

## 10. Implementation order (UPDATED 2026-05-16 — multiformat + provenance)

Seven steps from "no library flow" to "fully working classical
library." The 2026-05-16 revision folds TagLib into Step 2 (was
FLAC-only / v1.1 ID3-read) and prepends Stage Z to Step 3. Step 0
(the marker emission) has already landed.

### Step 0 — Provenance marker emission (LANDED 2026-05-16)

Implemented in `src/FlacTags.cpp::buildVorbisTags()`: every rip-time
bundle now carries `CONCERTO_PIPELINE_VERSION=1`, `CONCERTO_SOURCE=
<sourceTag>`, `CONCERTO_ENRICHED_AT=<ISO-8601>`. Mirrored in
`src/MetadataModel.cpp::debugDumpTrack()` for diagnostic parity. No
config gate (§A.7). Validated via the `mbquery_cli --disc-id ...`
harness; the Ravel test case's track 02 shows the new fields trailing
`ISRC`. Every new rip is Stage Z-trusted on first re-open.

### Step 1 — `LibraryDatabase` + schema migrations

`src/LibraryDatabase.{h,cpp}`. SQLite via `QtSql`, WAL mode. Opens
`library/library.db`; forward-only migrations on startup; typed
access methods (`upsertRelease`, `getFilesByContentHash`,
`queryByPerformer`, etc.). Fold in the §A.5 schema additions on
initial creation (`releases.pipeline_version`, `files.writeback_at`).
Foundation — everything else depends on it. ~600 LoC.

### Step 2 — `AudioTagIo` (TagLib, multiformat) + content hashing

`src/AudioTagIo.{h,cpp}`. Single tag I/O surface for every playable
format, backed by TagLib (§4.1), linked dynamically per the LGPL
pattern. FLAC reads fast-path through `FlacTags::read()` to avoid
TagLib spin-up on the rip hot path; everything else (and all writes)
goes through TagLib's `FileRef` + `PropertyMap`.

Audio-frame `content_hash` lands in `src/AudioFrameHash.{h,cpp}` —
streaming SHA-256 over audio frames only (skip the format-specific
metadata block: FLAC STREAMINFO + blocks, MP3 ID3v2 tag block, MP4
non-`mdat` boxes, OGG comment header pages). One small per-format
header-skip routine; TagLib doesn't expose frame offsets directly.

The TagLib Homebrew dep is the one new transitive added here.

### Step 3 — `FolderIdentifier` with Stages Z + A–D wired

`src/FolderIdentifier.{h,cpp}`. Strategy chain over §2.2 Stages Z +
A–D (Stage E deferred to step 5). Stage Z fires first: read tags via
`AudioTagIo`, check the §A.1 marker on a quorum; if trusted, return
without a web call. Stage Z's DB-side mirror lives in the same class
— `LibraryDatabase::getFilesByContentHash` returns rows where
`pipeline_version ≥ kConcertoPipelineVersion` are trusted same as
in-file markers (§A.4). Stages A–D reuse `musicbrainz::Client::
fetchRelease` / `MetadataCache::getByDiscId` / a new `searchReleases()`
(~25 LoC) / `scoring::pick` / `flattenRelease`. The orchestrator
mirrors `MetadataResolver` with the folder-frontend swap.

### Step 4 — `LibraryResolver` + `LibrarySweeper`

`src/LibraryResolver.{h,cpp}` — user-facing async API ("resolve this
folder, fire signals"); thin, queues work and forwards signals.
`src/LibrarySweeper.{h,cpp}` — background thread, owns a
`QNetworkAccessManager`, pulls from `sweep_queue` SQLite table at the
MB rate-cap, persists jobs across restarts, cancellable per-folder.
Start at app boot from `main.cpp`; hook `PlaylistModel::openFolder()`
to call `LibraryResolver` and prefer DB-derived display data.

### Step 5 — `AudioFingerprinter` (shared with rip-time Stage 3)

`src/AudioFingerprinter.{h,cpp}`. Vendors Chromaprint under
`third_party/chromaprint/` with `-DAUDIO_PROCESSOR_LIB=vDSP` (matches
`METADATA_PLAN.md` §1.3a.1). Folder Stage E and rip-time Stage 3 both
use it; rip-time `AcoustIdProvider::lookup()` (today a stub) gets its
real implementation. Done once, shared both ways.

### Step 6 — Library UI (QML)

`qml/LibraryView.qml` + `LibraryModel` (releases) + `LibrarySidebarModel`
(composer / performer / etc. nav tree). Reuses `PlaylistModel` for
playback. User-visible feature lands here; everything before is
plumbing.

### Step 7 — Tag writeback (opt-in, multiformat)

`AudioTagIo::writeTags()`'s write path goes live under a preference
toggle. TagLib handles the format-specific block-rewriting (ID3v2
in-place, MP4 freeform atom growth, Vorbis comment block resize).
Adds: the `Settings → Library` toggle; per-file tag-block backup to
`tag-backups/<hash>.tagbak`; augment-not-overwrite policy (never
clobber a non-empty user tag except `MUSICBRAINZ_*` IDs and
`CONCERTO_*` markers); the "Revert tag writeback" action;
`files.writeback_at` set on success. FLAC / MP3 / M4A / OGG light up
together; WAV/AIFF stay read-only.

### Future steps (post-v1, sketched for context)

- **Step 8**: Apple Music Stage 1.5 for library flow (§5.4); reuses
  `AppleMusicJwt` from the rip pipeline.
- **Step 9**: WAV / AIFF writeback (embedded-ID3, fragile cross-tool).
- **Step 10**: Cross-machine sync of the library DB.
- **Step 11**: Cover-art perceptual hash matching (Stage A refinement).

---

## 11. Recommended architecture (one paragraph)

**Reuse the rip-pipeline's downstream half verbatim** (`musicbrainz::
Client`, `scoring::pick`, `flattenRelease`, `AlbumMeta`,
`buildVorbisTags`), introduce a new **`FolderIdentifier` strategy
chain** that mirrors `MetadataResolver`'s pipeline-runner pattern but
walks **Stages Z → A → B → C → D → E** (trust marker → embedded MBID
tag → disc-ID tag → barcode/catno search → folder-TOC fingerprint →
AcoustID intersect) instead of disc-ID computation, **store results
in a content-hash-keyed SQLite sidecar DB** at `~/Library/Application
Support/Concerto/library/library.db` (file writeback off by default,
opt-in via preferences), use **TagLib (dynamic-linked, LGPL-clean) as
the single tag I/O backend** for every format Concerto plays (FLAC,
MP3, M4A/ALAC/AAC, OGG, WAV, AIFF), and run uncached resolutions
through a **persistent `LibrarySweeper` background thread** at MB's 1
req/sec cap with **eager resolution for single-folder opens** (≤5–10
albums synchronous; rest deferred) and **background sweeping for
bulk-import**.

The three key decisions: **(a) SQLite from day one** (flat JSON cannot
serve "all Pollini recordings"-style queries); **(b) content-hash
keying** (survives the user moving / renaming files, which they will);
**(c) Concerto provenance marker + Stage Z trust check** (a folder
Concerto previously enriched skips every web call on re-open,
in-file or via DB-as-authority — §A).

---

## 12. Prioritized implementation order (one block, UPDATED 2026-05-16)

0. **Provenance marker emission** — LANDED 2026-05-16 in
   `src/FlacTags.cpp` + `src/MetadataModel.cpp`. Every rip-time bundle
   now carries `CONCERTO_PIPELINE_VERSION`, `CONCERTO_SOURCE`,
   `CONCERTO_ENRICHED_AT`. No config gate; see §A.1.
1. **`LibraryDatabase` + schema migrations** (`src/LibraryDatabase.{h,cpp}`).
   SQLite via `QtSql`, WAL mode, the schema from §6.3 (including the
   §A.5 `pipeline_version` / `writeback_at` columns), forward-only
   migrations. Foundational; everything else depends on it.
2. **`AudioTagIo` (TagLib, multiformat) + audio-frame content hashing**
   (`src/AudioTagIo.{h,cpp}`, `src/AudioFrameHash.{h,cpp}`). One API
   covers FLAC / MP3 / M4A / OGG / WAV / AIFF read & write via TagLib.
   FLAC read fast-paths through existing `FlacTags::read()`. The
   content-hash routine lands here.
3. **`FolderIdentifier` (Stages Z + A–D)**
   (`src/FolderIdentifier.{h,cpp}`). The new strategy chain. Stage Z
   reads the §A.1 trust marker via `AudioTagIo` AND consults
   `LibraryDatabase::getFilesByContentHash` for the DB-as-authority
   case (§A.4). Stages A–D reuse `musicbrainz::Client::fetchRelease`,
   `scoring::pick`, `flattenRelease`. Adds
   `musicbrainz::Client::searchReleases()` (~25 LoC).
4. **`LibraryResolver` + `LibrarySweeper`** (`src/LibraryResolver.{h,cpp}`,
   `src/LibrarySweeper.{h,cpp}`). The orchestrator + background
   thread. MB rate-cap enforced here. Hook into `PlaylistModel::
   openFolder`.
5. **`AudioFingerprinter` (shared with rip-time Stage 3)**
   (`src/AudioFingerprinter.{h,cpp}`). Vendors Chromaprint under
   `third_party/chromaprint/` with the vDSP backend. Wires up folder
   Stage E and rip-time Stage 3 simultaneously.
6. **Library UI in QML** (`qml/LibraryView.qml` + supporting models).
   Sidebar (composers / performers / etc.) + album grid + per-album
   track list. Reuses `PlaylistModel` for playback.
7. **Tag writeback opt-in** (writeback path in `AudioTagIo::writeTags`).
   FLAC / MP3 / M4A / OGG all light up together via TagLib.
   Backup-and-restore. Preferences toggle.

Stop here for v1. v2 adds Apple Music Stage 1.5 for the library flow,
WAV/AIFF writeback, and the v2 marker fields (bumps
`kConcertoPipelineVersion` → 2; see §A.6).

---

## Sources cited

For visited URLs and external references, see the existing
`METADATA_PLAN.md` "Sources cited" section. This doc cites no new
external references — it's a derivation from already-cited material
plus the in-tree implementations:

- `docs/METADATA_PLAN.md` — primary reference for licensing, sources,
  scoring algorithm, threading model
- `docs/METADATA_PIPELINE_AUTOMATED.md` — distilled rip-time pipeline
- `src/MetadataResolver.{h,cpp}` — orchestrator pattern reused here
- `src/MusicBrainz.{h,cpp}` — async MB client reused as-is
- `src/MetadataCache.{h,cpp}` — JSON cache; supplemented by SQLite
- `src/MetadataScoring.{h,cpp}` — scoring algorithm reused as-is
- `src/MetadataModel.{h,cpp}` — `AlbumMeta` / `TrackMeta` reused as-is
- `src/FlacTags.{h,cpp}` — tag read + Vorbis comment build reused
- `src/PlaylistModel.{h,cpp}` — current library-adjacent state; hook
  point for the new flow
- `src/PendingSubmissions.{h,cpp}` — submission queue model (not
  directly reused but pattern-influencing)
- `src/RipWorker.cpp` — known nested-QEventLoop issue (separate fix);
  the library flow's `LibrarySweeper` avoids this pattern from day one
- `src/SystemPaths.{h,cpp}` — for the DB location resolution

MusicBrainz API endpoints used by the new flow (all anonymous,
no-auth, same UA as rip-time):

- `GET /ws/2/release/<mbid>?fmt=json&inc=...` — Stage A direct
  (already in `musicbrainz::Client::fetchRelease`)
- `GET /ws/2/release-group/<mbid>?fmt=json&inc=releases` — Stage C
  via release-group MBID (new)
- `GET /ws/2/release?query=...&fmt=json` — Stages C/D search (new)
- `GET /ws/2/recording/<mbid>?fmt=json&inc=releases` — Stage E
  intersection (already used for AcoustID at rip-time)
- Cover Art Archive — unchanged from rip-time §1.10

End of document.
