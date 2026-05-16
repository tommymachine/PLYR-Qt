# Concerto Metadata Plan

How Concerto obtains classical-music metadata (composer, performer, conductor,
orchestra, work, movement) for arbitrary audio CDs and multi-disc sets, and
how it wires that into the rip pipeline.

Status: implementation plan. The current `src/MusicBrainz.{h,cpp}` is a
working but rock-music-shaped client — it fetches album title and joined
artist-credit, no Work/Composer extraction. This document describes the
delta to make it classical-aware.

**Major revision 2026-05-16.** This document was rewritten under three
new hard constraints from the dev:

1. **Fully automated — zero user interaction during identification.** No
   chooser, no auth prompts, no manual-entry fallback. The pipeline
   either picks one answer or commits a degraded auto-result.
2. **Fully free for the end user.** No paid APIs, no per-user developer
   accounts, no Discogs PAT. The only acceptable auth is a single
   app-wide application key (or developer-signed JWT) that we (the
   developer) embed in the binary, with the source's ToS explicitly
   permitting that. **Closed-source distribution is fine; recurring
   developer-side costs are acceptable when the resulting catalog is
   materially better for the target genre.**
3. **Permissive-license clean.** Reaffirmed: no GPL/LGPL link surface.
   CD-TEXT is reimplemented from the MMC-5 / Red Book Annex J spec
   rather than linking libcdio (GPL-3+). See §1.3.

Sections substantially revised under those constraints are marked
**(UPDATED 2026-05-16)**. The **Apple Music Stage 1.5 enrichment layer**
(added 2026-05-16) is marked **(UPDATED 2026-05-16 — Apple Music)** in
the sections it touches; see §1.7 for the full rationale and §4.1a for
the gating logic.

License posture (recap from `CDRIP_STRATEGY.md`): the binary stays
permissive. Code dependencies must be MIT/BSD/Apache/MPL/zlib, or LGPL
**if** dynamically linkable. GPL is off-limits. Data licenses (CC0,
CC-BY-NC-SA) are about content redistribution, not binary infection — so
they're tractable.

---

## 0. TL;DR — pathway in one screen (UPDATED 2026-05-16 — Apple Music)

Three hard constraints reset the design:

- **Fully automated** — no chooser, no PAT prompt, no manual-entry form. The
  pipeline either picks one answer or accepts a degraded auto-result.
- **Fully free for the end user** — no per-user accounts, no paid APIs at
  the user end. Auth tokens needed by app-wide endpoints are baked into the
  binary; the dev funds the developer-program side. Concerto is closed-source
  distribution, so the embedded credentials are protected by the same
  threat model as the rest of the binary.
- **Permissive-license clean** — no GPL/LGPL link surface; CD-TEXT is
  reimplemented from the MMC-5 spec (via Apple's IOKit `IOCDMedia::readTOC`
  with format 0x05) rather than linking libcdio.

The pipeline:

1. **Primary: MusicBrainz disc-ID lookup.** Anonymous, no auth, CC0 core
   data, first-class classical fields. Same two-hop fetch as before:
   `/ws/2/discid/<id>?inc=artist-credits+recordings+release-groups` →
   pick best release deterministically (Section 3.3) →
   `/ws/2/release/<mbid>?inc=...work-rels+work-level-rels...` for the
   composer/conductor/performer relations.
2. **Deterministic auto-pick (no chooser).** When MB returns N candidate
   releases, score them by classical-completeness / curation signals and
   pick the top score. Final tiebreaker: lowest MBID lex order — same disc
   gets the same pick on every machine. The score is logged to the cache
   for diagnostics. See Section 3.3.
3. **Enrichment (Stage 1.5, Apple Music) — only when MB is sparse for
   classical needs.** Apple Music has no disc-ID endpoint, so it is *not*
   a primary resolver; it enriches MB winners that lack proper Work /
   Movement structure. Gating: only fires if the MB result has a barcode
   AND (≥30% of recordings lack work-rels OR any recording lacks a
   composer relation), or per-track if any track has a readable ISRC and
   no composer in the MB result. Auth: ES256 JWT signed in-process from a
   `.p8` embedded in the binary; signed via macOS Security framework, no
   OpenSSL dep. See §1.7 and §4.1a.
4. **If MB returns zero:** read **CD-TEXT** via our own MMC reader. macOS
   `IOCDMedia::readTOC(format=0x05)` returns the raw 18-byte packs; we
   parse them ourselves (Section 1.3). Spec: MMC-5 §6.26.3.7.1 + Red Book
   Annex J. No libcdio link.
5. **If CD-TEXT is empty:** rip track 1 to memory, fingerprint with
   **Chromaprint** (MIT-licensed when built against vDSP on macOS, no
   FFmpeg/LGPL surface), and query **AcoustID** (app-wide application key
   shipped in the binary — AcoustID's model explicitly permits this; user
   keys are only for fingerprint *submission*, which we never do). From
   the recording-MBID, walk to release(es) and apply the same scoring.
6. **If everything misses:** write deterministic stub tags
   (`ALBUM="Unknown Album (Disc ID: <id>)"`, `ARTIST="Unknown Artist"`,
   `TRACKNUMBER=N`) and queue the disc-ID + TOC into a local
   `pending-submissions.db` so the dev can push them upstream as a batch
   under their own MB account. The user sees a successful rip with
   minimal tags; no prompt.
6. **Tag schema:** Picard's canonical mapping verbatim (Section 2.4). No
   change.
7. **Cache:** `<AppDataLocation>/metadata-cache/<discid>.json` carrying the
   raw MB JSON, the parsed `AlbumMeta`, **the scoring log** for the
   chosen release, and (when Stage 1.5 fired) the raw Apple Music JSON
   plus the `stage15_decision` row. 30-day refresh.
8. **Threading:** `MetadataResolver` lives on the `RipWorker` thread, owns
   its own `QNetworkAccessManager`, fully async via signals — no nested
   `QEventLoop` (the existing one in `MusicBrainz.cpp::httpGet` gets
   deleted as part of this work). JWT signing is in-process and
   synchronous (~200 µs on Apple Silicon); the signer is reused across
   the session.
9. **UI:** A status banner on the rip view showing "Matched MusicBrainz:
   <title>" or "Matched MusicBrainz + enriched via Apple Music:
   <title>" or "CD-TEXT only" or "Identified by AcoustID" or "No
   metadata found — saved as Unknown Album." No interactive surface.
   The cache browser (separate feature, out of scope here) is where the
   user eventually edits if they want to.

What was dropped from v1 (was in earlier draft of this doc):

- Discogs as a fallback — requires per-user PAT since 2023; incompatible
  with constraint #2. Deleted from the pipeline entirely.
- Manual-entry UI — incompatible with constraint #1. Replaced by stub
  tags.
- Release-chooser UI — incompatible with constraint #1. Replaced by the
  deterministic scoring function in §3.3.
- "Auto-accept threshold" / "confidence banner asking user to confirm" —
  the pipeline always commits; the score lives in the cache log.

Re-added 2026-05-16: **Apple Music as a Stage 1.5 enrichment provider.**
Previously dropped because of the $99/year Apple Developer Program cost;
re-added because the dev has an active membership and the catalog's
classical curation (first-class `composerName` / `workName` /
`movementName` / `movementNumber` / `movementCount` fields) materially
improves the ~30–50% of MB classical results that come back without
proper work-rels. Closed-source distribution covers the embedded-key
threat model. See §1.7.

Detailed reasoning, JSON examples, license citations, and class
skeletons follow.

---

## 1. Metadata sources

### 1.1 MusicBrainz — primary

**Why first.** Only large open database that models classical
correctly: separate `Work` entities for compositions, parent-work
relationships for symphony → movement, typed artist-relations
(`composer`, `conductor`, `performer`, `instrument`, `performing
orchestra`, `vocal`, `arranger`, `lyricist`).

**Endpoints.**

- Disc lookup (free, no auth):
  `https://musicbrainz.org/ws/2/discid/<discid>?fmt=json&inc=artist-credits+recordings+release-groups`

  The disc-ID endpoint **rejects** `work-level-rels` and
  `recording-level-rels` — the server returns HTTP 200 with an `error`
  body. Verified by direct curl during this research:
  `recording-level-rels is not a valid inc parameter for the discid resource.`

  Available `inc=` values that *do* work here: `artist-credits`,
  `recordings`, `release-groups`, `isrcs`, `labels`, `media`,
  `aliases`, `genres`, plus the entity-link relations (`area-rels`,
  `artist-rels`, `label-rels`, `place-rels`, `event-rels`,
  `release-rels`, `release-group-rels`, `series-rels`, `url-rels`,
  `work-rels`, `instrument-rels`) at the *release* level only.

- Release deep-fetch (for the chosen release, free, no auth):
  `https://musicbrainz.org/ws/2/release/<mbid>?fmt=json&inc=artist-credits+recordings+work-rels+work-level-rels+artist-rels+recording-level-rels+release-groups+labels+isrcs`

  This is where `work-level-rels` and `recording-level-rels` are
  allowed and where the composer/conductor/performer relations live.

- Optional barcode search (when disc lookup misses but the user
  reads the box's barcode):
  `https://musicbrainz.org/ws/2/release/?query=barcode:<digits>&fmt=json`

**JSON shape — releases array (verified live during research).**

```jsonc
{
  "releases": [
    {
      "id": "853b6a62-116a-4a11-bc22-533b7cd331e7",
      "title": "Rachmaninoff: Piano Concerto no. 3",
      "date": "2020-01-17",
      "country": "XW",
      "barcode": "190296872785",
      "asin": "...",
      "release-group": { "id": "...", "primary-type": "Album", ... },
      "label-info": [{ "catalog-number": "RCO 19003",
                       "label": { "id": "...", "name": "RCO Live" } }],
      "artist-credit": [
        { "name": "Rachmaninoff", "joinphrase": "; ",
          "artist": { "id": "44b16e44-...", "sort-name": "Rachmaninoff, Sergei Vasilievich" }},
        { "name": "Abduraimov", "joinphrase": ", ", "artist": { ... }},
        { "name": "Concertgebouworkest", "joinphrase": ", ", "artist": { ... }},
        { "name": "Gergiev", "artist": { "disambiguation": "conductor", ... }}
      ],
      "media": [{
        "position": 1, "format": "CD", "track-count": 4,
        "discs": [{ "id": "<mbdiscid>", "sectors": ..., "offsets": [...] }],
        "tracks": [{ "position": 1, "number": "1",
                     "title": "Piano Concerto no. 3 ... I. Allegro ma non tanto",
                     "length": 1234567,
                     "artist-credit": [...],
                     "recording": { "id": "...", "title": "...",
                                    "length": ..., "relations": [...] } }, ...]
      }]
    }
  ]
}
```

**Critical: `recording.relations` is where classical metadata lives.**
The relevant relation types (each is an object inside that array):

| `type` | `target-type` | What it means | Where the target's name is |
| --- | --- | --- | --- |
| `conductor` | `artist` | Conductor of this recording | `relation.artist.name` |
| `instrument` | `artist` | Soloist; instrument(s) in `attributes` | `relation.artist.name`; `relation.attributes[]` lists "piano", "violin", etc. |
| `vocal` | `artist` | Singer; range in `attributes` | `relation.artist.name`; e.g. `attributes: ["soprano"]` |
| `performing orchestra` | `artist` | The orchestra/ensemble | `relation.artist.name`, with `relation.artist.type` typically `Orchestra` or `Choir` |
| `performance` | `work` | This recording IS-OF this Work | `relation.work.title`; the Work in turn has its own `relations[]` with the composer |
| `recording` | `artist` | Engineer (audio engineer of the recording) | for completeness |

Verified live with release `853b6a62-...` (Rachmaninoff PC3 / Gergiev /
Abduraimov / Concertgebouworkest):

```jsonc
"recording": {
  "id": "...",
  "title": "Piano Concerto no. 3 ... I. Allegro ma non tanto",
  "relations": [
    { "type": "conductor", "target-type": "artist",
      "artist": { "name": "Валерий Гергиев", "sort-name": "Gergiev, Valery", ... },
      "target-credit": "Valery Gergiev" },
    { "type": "instrument", "target-type": "artist",
      "attributes": ["piano"],
      "artist": { "name": "Behzod Abduraimov", ... } },
    { "type": "performing orchestra", "target-type": "artist",
      "artist": { "name": "Koninklijk Concertgebouworkest",
                  "type": "Orchestra", ... } },
    { "type": "performance", "target-type": "work",
      "attributes": ["live"],
      "work": {
        "id": "e55e8d47-c07a-31f5-90e4-404b8e975255",
        "title": "Piano Concerto no. 3 in D minor, op. 30: I. Allegro ma non tanto",
        "relations": [
          { "type": "composer", "target-type": "artist",
            "artist": { "name": "Сергей Васильевич Рахманинов",
                        "sort-name": "Rachmaninoff, Sergei Vasilievich",
                        "disambiguation": "Russian composer", ... },
            "target-credit": "Sergei Rachmaninov",
            "begin": "1909", "end": "1909-09-23" } ] }}
  ]
}
```

The composer relation lives on the **Work**, not the recording. The
recording has the `performance` relation pointing to the Work, which
has the `composer` relation. So the access path for the composer is
`recording.relations[type=performance].work.relations[type=composer].artist`.
This *only* works when you fetched the release with `work-level-rels`
(which requires the second-hop release-MBID fetch — disc-ID
endpoint forbids it).

**Tip: `target-credit` is the credited name** (e.g. "Sergei
Rachmaninov" in Latin script) and `artist.name` is the canonical MB
name (often Cyrillic for Russian composers). Use `target-credit` if
non-empty, else `artist.name`, else `artist.sort-name`.

**Parent-Work traversal.** Works nest: "Piano Concerto no. 3 ... I.
Allegro ma non tanto" has a Work-Work `parts` relationship pointing
up to "Piano Concerto no. 3 in D minor, op. 30". beets' `parentwork`
plugin walks this chain. For Concerto v1, take a shortcut: the
movement-level Work's `title` already contains the parent (the colon
convention — "Piano Concerto no. 3 ... : I. Allegro"). Split on
`": "` to get parent (left) and movement (right). Document this
heuristic and revisit in v2.

**Rate limit.** 1 request/sec per IP (anonymous), 50 req/s/UA
globally. Throttle internally. With two hops per disc, an album-rip
sends ~2 requests total — well under quotas.

**User-Agent.** Required. Format: `Application/version ( contact )`.
Example from MB's docs: `MyAwesomeTagger/1.2.0 ( http://myawesometagger.example.com )`.
Recommended: `Concerto/<version> ( https://github.com/<user>/<repo> )`
or an email. The current code uses `concerto-arverify/0.1` — short of
the recommendation but accepted in practice. Tighten it.

**Data license.** Core data (artists, releases, recordings, works,
release-groups, labels, mediums, relationships, URLs) is **CC0** —
public domain, no attribution required, redistribution allowed.
Supplementary data (annotations, user-tags, ratings, edit history,
search indexes, derived statistics) is **CC-BY-NC-SA 3.0** — avoid
storing/redistributing those, or carry the attribution + ShareAlike
notice. For Concerto, *don't* request tags/ratings — sticking to
core entities side-steps the BY-NC-SA constraint entirely.

**Caching.** CC0 imposes no caching limits. CC-BY-NC-SA technically
restricts commercial use of the supplementary data. Strategy:
request only core entities, cache on disk, no problem.

**Sources:**
- [MusicBrainz API](https://musicbrainz.org/doc/MusicBrainz_API)
- [MusicBrainz API/Examples](https://musicbrainz.org/doc/MusicBrainz_API/Examples)
- [MusicBrainz API/Rate Limiting](https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting)
- [About/Data License](https://musicbrainz.org/doc/About/Data_License)
- [Disc ID](https://musicbrainz.org/doc/Disc_ID)
- [Disc ID Calculation](https://musicbrainz.org/doc/Disc_ID_Calculation)
- [Performance relationship](https://musicbrainz.org/relationship/a3005666-a872-32c3-ad06-98af558e99b0)
- [How to Use Works](https://musicbrainz.org/doc/How_to_Use_Works)
- [Style/Classical/Recording Artist](https://musicbrainz.org/doc/Style/Classical/Recording_Artist)

### 1.2 Discogs — DROPPED (UPDATED 2026-05-16)

**Status: removed from the pipeline.**

Reason: Discogs's `/database/search` endpoint requires a Personal Access
Token bound to an individual Discogs account since 2023. Three options
were on the table and all three fail at least one new constraint:

- **Each user supplies their own PAT** — fails constraint #1 (zero user
  interaction) and constraint #2 (no per-user accounts).
- **Ship a single app-wide PAT** — the 60 req/min limit is per *token*,
  so all users globally share the bucket; trivially abused, and Discogs's
  ToS explicitly disallows sharing a PAT across multiple end-user
  installs.
- **OAuth handshake** — definitely fails constraint #1.

The marginal classical coverage Discogs adds over MB+CD-TEXT+AcoustID
isn't worth the auth burden. Drop.

**Sources:**
- [Discogs developers](https://www.discogs.com/developers)
- [Discogs API rate limiting](https://www.discogs.com/developers/accessing.html)
- [Authentication requirements for /database/search](https://www.discogs.com/forum/thread/399958)

### 1.3 CD-TEXT — secondary, clean-room MMC reader (UPDATED 2026-05-16)

CD-TEXT is metadata stored in the lead-in area of the disc itself. The
SCSI MMC command is `READ TOC/PMA/ATIP` (opcode `0x43`) with the format
field set to `0101b` / `0x05`. It returns a stream of 18-byte "packs" that
carry strings keyed by pack type (`0x80` TITLE, `0x81` PERFORMER, `0x83`
COMPOSER, etc.). The spec is in MMC-5 §6.26.3.7.1 and Red Book Annex J;
the data layout is in Sony's CD-TEXT authoring documentation.

**Promoted from "v2 nice-to-have" to "secondary primary path"** under the
new constraints. When MB returns zero releases, CD-TEXT is what we try
before paying for a fingerprint decode + AcoustID call. On classical CDs
CD-TEXT is genuinely sparse (most major-label classical pressings don't
author it) but on the ones that do, it returns enough to write reasonable
tags without prompting.

#### 1.3.1 Why we reimplement rather than link libcdio

libcdio is GPL-3+ and incompatible with Concerto's permissive stance. The
dev's intuition is correct: CD-TEXT is a standard SCSI MMC command and
the pack format is a published Sony / Philips spec. There is no
proprietary art in the parser — the entire data layout is in the spec.
Per the project's clean-room rule (see `feedback_gpl_study_vs_copy.md`),
reading libcdio's `lib/driver/cdtext.c` to understand the protocol is
fine; what we cannot do is copy expression (identifiers, structure,
comments, control flow verbatim). We implement from the spec.

The same applies to Apple's `IOCDStorageFamily` source on
opensource.apple.com (APSL 2.0) — APSL is permissive enough that we
could technically vendor it, but we don't need to: the public IOKit
framework is the API surface, and Apple's headers are part of the SDK.

#### 1.3.2 The macOS path — IOKit, not raw SCSI

There are three plausible macOS paths and the cleanest one wins by a
mile:

1. **`IOCDMedia::readTOC()` with `format=0x05`** — the public IOKit
   method takes a `CDTOCFormat` byte parameter (0x00–0x05). Apple's
   `IOCDMedia` header (APSL 2.0,
   `/System/Library/Frameworks/Kernel.framework/.../IOCDMedia.h` via
   user-space mapping) exposes `readTOC(buffer, format,
   formatAsTime, trackOrSession, actualByteCount)`. Format `0x05`
   returns the raw CD-TEXT lead-in data as a `CDTEXT` structure —
   header bytes + array of `CDTEXTDescriptor` (the 18-byte packs).
   The struct typedefs live in `IOCDTypes.h` and we can use them
   directly (APSL allows redistribution; for paranoia we redeclare
   our own equivalent structs in our own header so we don't carry
   APSL-licensed text in our source tree).

   **This is the chosen path.** No SCSITaskUserClient, no
   `DKIOCCDREADTOC` ioctl on `/dev/rdisk*`, no manual CDB
   construction.

2. **`DKIOCCDREADTOC` ioctl on `/dev/rdisk*`** — works, but requires
   the `/dev/rdisk*` node (DiskArbitration unmount/re-mount dance to
   get exclusive access), more error-prone than the IOKit
   user-client.

3. **SCSITaskUserClient with manual CDB** — overkill. Reserved for
   when we need a command IOCDMedia doesn't wrap. CD-TEXT is wrapped.

Reference for path #1 (do NOT link against any of these — they're
APSL-licensed in Apple's tree, and we re-declare equivalents):

- `IOCDTypes.h` — defines `CDTOC`, `CDTOCDescriptor`, `CDTEXT`,
  `CDTEXTDescriptor`, plus the `kIOCDMediaTypeROM` etc. property
  constants. Lives in
  `/System/Library/Frameworks/Kernel.framework/Headers/IOKit/storage/IOCDTypes.h`
  on disk; published source on opensource.apple.com.
- `IOCDMedia.h` — the user-space wrapper class. Method:
  `IOReturn readTOC(IOMemoryDescriptor *, CDTOCFormat, UInt8
  formatAsTime, UInt8 trackOrSession, UInt16 *actualByteCount)`.
- `IOCDMediaBSDClient.h` — the ioctl interface (path #2). Lists
  `DKIOCCDREADTOC` but no CD-TEXT-specific ioctl; the format byte is
  in the request struct.

The Concerto user-space code path:

```
DASessionRef → DADiskRef → DADiskClaim()
  → IOServiceGetMatchingService(kIOCDMediaClass)
  → IOCDMediaInterface (IOCFPlugin or direct via IOKit)
  → readTOC(buffer, 0x05, ...)
  → parse pack stream
```

#### 1.3.3 The 18-byte pack structure (verbatim from MMC-3 Annex J / Sony CD-TEXT)

```
Byte 0    Pack type (0x80–0x8F)
Byte 1    Track number (0 = disc-level; 1..99 = track-specific)
Byte 2    Sequence counter (sequential within a block)
Byte 3    BNCPI — Block Number / Character Position Indicator
            bits 0..3 character position (continuation marker; 15 = new text)
            bits 4..6 block number (0..7; each block = one language)
            bit 7    double-byte flag (0 = single-byte; 1 = double-byte)
Bytes 4..15  12 bytes of payload (zero-terminated strings; one pack may
             carry multiple short strings; long strings span packs and
             use the BNCPI position byte to mark continuation)
Bytes 16..17 CRC-16 (polynomial 0x11021, inverted, big-endian)
```

Pack type IDs:

| Code  | Name           | Carries                                       |
| ----- | -------------- | --------------------------------------------- |
| 0x80  | TITLE          | Album title (track 0) and per-track titles    |
| 0x81  | PERFORMER      | Performer (often the album-level credited artist) |
| 0x82  | SONGWRITER     | Songwriter                                    |
| 0x83  | COMPOSER       | Composer                                      |
| 0x84  | ARRANGER       | Arranger                                      |
| 0x85  | MESSAGE        | Provider/artist message                       |
| 0x86  | DISC_ID        | Catalog number / disc identifier (binary)     |
| 0x87  | GENRE          | Genre code + freetext (binary)                |
| 0x88  | TOC_INFO       | TOC info (binary)                             |
| 0x89  | TOC_INFO2      | Second TOC info (binary)                      |
| 0x8D  | CLOSED_INFO    | Closed info (binary, not for display)         |
| 0x8E  | UPC_EAN / ISRC | UPC/EAN at disc level, ISRC per track         |
| 0x8F  | SIZE_INFO      | Block summary; first byte is character set    |

Character-set byte (in 0x8F SIZE_INFO):

| Value | Charset                                                             |
| ----- | ------------------------------------------------------------------- |
| 0x00  | ASCII (default)                                                     |
| 0x01  | ISO-8859-1                                                          |
| 0x80  | MS-JIS (Shift-JIS variant for Japanese)                             |
| 0x81  | Korean                                                              |
| 0x82  | Mandarin Chinese                                                    |

Anything else: treat as ASCII and best-effort.

#### 1.3.4 Qt6/C++ skeleton (reference — not compiled)

```cpp
// CdText.h
#pragma once
#include <QString>
#include <QStringList>
#include <QByteArray>

namespace concerto::cdtext {

struct CdText {
    QString album;
    QString performer;           // disc-level
    QString composer;            // disc-level
    QString songwriter;
    QString arranger;
    QString message;
    QString catalogNumber;       // from 0x86 / 0x8E disc-level
    QStringList trackTitles;     // index 0 unused; 1..N = track
    QStringList trackPerformers;
    QStringList trackComposers;
    QStringList trackIsrc;
};

CdText parsePacks(const QByteArray& rawPackData);  // CRC-checked, charset-aware

} // namespace
```

```cpp
// CdText_macOS.mm (Objective-C++ for the IOKit bridge)
#import <DiskArbitration/DiskArbitration.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/storage/IOCDTypes.h>
// We do NOT include IOCDMedia.h directly; we resolve readTOC via the
// user-client port to keep the project link surface free of APSL'd
// kernel headers. (If link surface is acceptable, the direct call is
// equivalent and clearer.)

QByteArray readCdTextRaw(io_service_t cdMedia) {
    // 1. Open a user-client connection on the IOCDMedia service.
    io_connect_t conn;
    kern_return_t kr = IOServiceOpen(cdMedia, mach_task_self(),
                                     kIOCDMediaUserClientTypeID, &conn);
    if (kr != KERN_SUCCESS) return {};

    // 2. Build an MMC pass-through CDB:
    //   READ TOC/PMA/ATIP (0x43), format=0x05 (CD-TEXT), allocLen=65530
    //   The kernel readTOC wrapper does this for us when format=0x05.
    //   Either go through the wrapper (preferred) or pass-through if
    //   the wrapper isn't reachable from user space — both produce the
    //   same raw 18-byte pack stream after the 4-byte response header.

    // 3. Read into a buffer up to 65,530 bytes (the max allocation length
    //    for format 0x05 — Section 6.26.3.7.1 of MMC-5 lists this as
    //    "Bytes 7..8 of the CDB").
    QByteArray buf;
    buf.resize(65530);
    UInt16 actual = 0;
    // Pseudocode for the actual readTOC call — wired up via the kernel
    // user-client method dispatch table:
    //   args = { format=0x05, formatAsTime=0, trackOrSession=0,
    //            bufferPtr=buf.data(), bufferLen=buf.size() }
    //   IOConnectCallStructMethod(conn, kMethodReadTOC, &args, ...)
    //   actual = (return value)

    IOServiceClose(conn);
    buf.resize(actual);

    // 4. Skip the 4-byte header (data-length BE16, reserved, reserved)
    //    to land on the first 18-byte pack.
    if (buf.size() < 4) return {};
    return buf.mid(4);
}
```

```cpp
// CdText.cpp (pure C++ — no Apple API)
CdText parsePacks(const QByteArray& packs) {
    CdText out;
    if (packs.size() % 18 != 0) {
        // We tolerate trailing junk; just stop at the last full pack.
    }

    QByteArray charsetMap;  // populated by 0x8F packs; defaults to ASCII
    // Accumulator for spans of text across multiple packs (one logical
    // string can span >12 bytes; null-terminator marks end).
    struct Acc { quint8 type; quint8 track; QByteArray bytes; };
    QVector<Acc> acc;

    for (int i = 0; i + 18 <= packs.size(); i += 18) {
        const auto* p = reinterpret_cast<const quint8*>(packs.constData() + i);
        quint8 type  = p[0];
        quint8 track = p[1];
        quint8 seq   = p[2];
        quint8 bncpi = p[3];
        bool isDoubleByte = bncpi & 0x80;
        // CRC check: bytes 16..17, polynomial 0x11021, inverted, BE.
        if (!crcOk(p)) continue;

        if (type == 0x8F) {
            // SIZE_INFO — byte 4 of payload = charset for this block
            if (i + 4 < packs.size()) {
                quint8 charset = p[4];
                // store charset for the active block (bncpi bits 4..6)
            }
            continue;
        }

        // Append the 12 payload bytes to the accumulator for (type, track);
        // strings end on a 0x00 byte; one pack can carry multiple short
        // strings (e.g. track-1-title + track-2-title in the same pack
        // when both are short — null-separator marks the break and the
        // next pack's seq counter starts from there).
        acc.push_back({type, track,
                       QByteArray(reinterpret_cast<const char*>(p + 4), 12)});
    }

    // Coalesce by (type, track), split on null bytes, decode per charset.
    // Then route into the CdText struct:
    //   type=0x80 → out.album (track==0) or out.trackTitles[track]
    //   type=0x81 → out.performer or out.trackPerformers
    //   type=0x83 → out.composer or out.trackComposers
    //   type=0x8E → out.catalogNumber (track==0) or out.trackIsrc[track]
    //   etc.
    return out;
}
```

This is reference material — the dev fills in the details. The shape is
right. Coverage: ~40 effective lines of state machine in
`parsePacks` plus ~25 lines of IOKit glue. Compare to libcdio's
`lib/driver/cdtext.c` which is ~600 LoC; most of that handles Linux ioctl
quirks and BSD variants we don't have.

#### 1.3.5 What we can use vs not use

| Source                                  | License  | Status                                                    |
| --------------------------------------- | -------- | --------------------------------------------------------- |
| MMC-5 spec (T10/INCITS 430-2007)        | T10      | Read freely; spec defines the data, no copyright on facts |
| Red Book / Sony CD-TEXT auth docs       | Sony     | Read freely                                               |
| libcdio `lib/driver/cdtext.c`           | GPL-3+   | Read for understanding; **do not** copy expression        |
| Apple `IOCDTypes.h` (`CDTEXT` struct)   | APSL 2.0 | Either re-include or re-declare; we re-declare to keep our source permissive-only |
| Apple `IOCDMedia.h` (`readTOC` wrapper) | APSL 2.0 | Call via Kernel.framework runtime; not bundled in source  |
| libcdio-osx fork                        | GPL-3+   | Same as libcdio                                           |

**Sources:**
- MMC-3 Annex J (CD-TEXT pack format)
- MMC-5 §6.26 READ TOC/PMA/ATIP, §6.26.3.7 format 0101b
- Sony CD-TEXT authoring spec (pack type IDs, charset codes)
- Apple `IOCDTypes.h` opensource.apple.com
- Apple Developer documentation, `IOCDMedia readTOC` method
- [CD-Text — Wikipedia](https://en.wikipedia.org/wiki/CD-Text) (cross-reference)

### 1.3a Chromaprint + AcoustID — last-line auto-identifier (UPDATED 2026-05-16)

When MB disc-ID and CD-TEXT both fail, we have audio. Audio fingerprinting
can recover MB identity. The de-facto open implementation is **Chromaprint**
(generates the fingerprint locally) + **AcoustID** (the web service that
looks up the fingerprint).

#### 1.3a.1 Chromaprint licensing — MIT-clean is achievable

Chromaprint is a slight headache to read about because the project's
upstream README says "LGPL 2.1 as a whole" — but that's only when built
*against FFmpeg* (LGPL) for audio decode. The Chromaprint source itself
is MIT, and on macOS the recommended build is against **vDSP**
(Accelerate.framework, Apple-permissive), not FFmpeg:

> "FFmpeg is preferred on all systems except for macOS, where you should
> use the standard vDSP framework." — Chromaprint README

Build options ranked for permissive cleanliness:

| FFT backend | License of resulting binary | Recommended for Concerto? |
| ----------- | --------------------------- | ------------------------ |
| **vDSP** (Accelerate.framework) | MIT (Chromaprint) + Apple SDK | **Yes** — macOS path |
| KissFFT     | MIT (Chromaprint) + MIT (KissFFT) | Fine alternative if vDSP unavailable |
| FFmpeg      | LGPL-2.1 | Avoid                    |
| FFTW3       | GPL-2+                     | Avoid                    |

We vendor Chromaprint source under `third_party/chromaprint/` configured
with `-DAUDIO_PROCESSOR_LIB=vDSP`. The resulting static library is
MIT-clean.

Audio decode for the rip's track-1 PCM is already in our pipeline (we
just ripped it); no decoder needed. We feed raw 16-bit signed PCM
straight into Chromaprint's `Context::feed()` and get a fingerprint
string out. ~30 lines of glue in a `Fingerprinter` class.

#### 1.3a.2 AcoustID auth model — app-key embeddable

AcoustID's documented model has **two key types**:

- **Application API key** (also called "client key" in the API): obtained
  by the app developer at acoustid.org/new-application. Free, no review,
  no commercial restrictions. Passed in the `client=` query parameter on
  `/v2/lookup`. **This is what gets embedded in Concerto's binary.**
- **User API key**: obtained by each individual user from
  acoustid.org/u/api-key. Only required for the `/v2/submit` endpoint
  (contributing a new fingerprint→MBID link back to AcoustID). Concerto
  does not submit fingerprints, so no user key is needed.

Picard, beets, and several other apps embed their own application key
exactly this way. This is the documented, supported pattern. The AcoustID
FAQ confirms application keys are for distribution; user keys are
"meant to be entered into an application like MusicBrainz Picard for the
purpose of identifying yourself" only for submission. ToS does not
restrict free desktop apps from embedding their app key.

Rate limit: ~3 req/s/IP soft-throttle per AcoustID community guidance;
we run well under that (one rip = one fingerprint request).

#### 1.3a.3 The endpoint

```
GET https://api.acoustid.org/v2/lookup
  ?client=<our app key>
  &meta=recordings+releasegroups+releases+tracks+sources
  &duration=<seconds, integer>
  &fingerprint=<chromaprint base64-ish string>
  &format=json
```

`meta=` values we use:
- `recordings` — recording-level metadata + MBID
- `releaseids` / `releases` — release-level metadata + MBIDs
- `releasegroups` — release-group MBIDs (lets us route into the same
  scoring algorithm as MB disc-ID hits)
- `sources` — count of submissions backing this fingerprint (a
  confidence proxy)

Response shape relevant to us (one branch shown):

```jsonc
{
  "status": "ok",
  "results": [
    { "id": "<acoustid uuid>",
      "score": 0.987,         // 0..1 fingerprint match confidence
      "recordings": [
        { "id": "<recording-mbid>",
          "duration": 481,
          "title": "...",
          "artists": [...],
          "releasegroups": [
            { "id": "<rg-mbid>", "title": "...",
              "releases": [{ "id": "<release-mbid>" }, ...] } ] } ] } ]
}
```

We pick the result with the highest `score`. From `recording-mbid`, we
walk to releases (any release containing this recording is a candidate),
score those releases using the same algorithm as §3.3, and second-hop
the winner.

When AcoustID returns *nothing* (or `score < 0.5`), we drop to the stub
tag path (Section 4.4). No prompt.

**Trade-off.** Fingerprinting requires audio data. We avoid the cost by
running it **only on the first track, only when MB disc-ID returns
nothing and CD-TEXT is empty.** Track 1 PCM is already in memory at that
point in the rip — no extra disk read.

**Sources:**
- [AcoustID Web Service](https://acoustid.org/webservice)
- [AcoustID FAQ](https://acoustid.org/faq)
- [AcoustID + MusicBrainz](https://musicbrainz.org/doc/AcoustID)
- [Chromaprint GitHub](https://github.com/acoustid/chromaprint)
- [Chromaprint LICENSE.md](https://github.com/acoustid/chromaprint/blob/master/LICENSE.md)

### 1.4 gnudb / freedb — not useful for classical

GnuDB inherited FreeDB's schema: artist/title only, no separate
composer field. From their docs: *"there is no data field in the
Freedb database for composer."* Useless for classical. Skip.

Not in the pipeline. If we ever wanted a fifth stage between AcoustID
and the stub (for the small fraction of CDs that are in gnudb but not
MB), the endpoint is `http://gnudb.gnudb.org/~cddb/cddb.cgi`, anonymous,
keyed by the CDDB1 disc-id hash already computed in
`ArVerify.h::DiscIds::cddbDiscId`. The titles it returns are still
artist/title-only, no composer — so it would only improve on the stub
slightly. Not worth adding for v1.

**Sources:**
- [gnudb.org](https://gnudb.org/)
- [gnudb CDDB protocol](https://gnudb.org/howtognudb.php)
- [FreeDB on MusicBrainz](https://musicbrainz.org/doc/FreeDB)

### 1.5 CUETools DB (CTDB) — interesting third party but data-thin

CTDB's `/lookup2.php?version=3&toc=<toc>&metadata=extensive` *can*
return MusicBrainz / Discogs / FreeDB metadata as part of its
response. CTDB replicates MB hourly, Discogs and FreeDB monthly. The
catch: it's an XML response that wraps responses from the underlying
services, not an independent corpus of classical metadata.

CTDB's real value is rip-verification CRCs and parity (already used
by `ArVerify.cpp`). Don't treat it as a metadata source — query
the underlying services (MB, Discogs) directly. Less indirection,
less latency, fewer formats to parse.

**Source:** [CTDB EAC Plugin](http://cue.tools/wiki/CTDB_EAC_Plugin)

### 1.6 AccurateRip — verification CRCs only, no metadata

Confirmed: AccurateRip's `.bin` response contains only the dBAR
header (13 bytes for disc identifier) followed by per-track CRCs
(9 bytes each: confidence count, CRC v1 32-bit LE, CRC v2 32-bit
LE). No titles, no artists, no composers.

URL pattern (already in `ArVerify::accurateRipPath`):
`http://www.accuraterip.com/accuraterip/<n>/<m>/<p>/dBAR-NNN-id1-id2-cddb.bin`

Don't ask AR for metadata. It doesn't have any.

**Source:** [AccurateRip](https://www.accuraterip.com/)

### 1.7 Apple Music — Stage 1.5 enrichment (UPDATED 2026-05-16 — Apple Music)

**Status: re-added as a Stage 1.5 enrichment layer over MusicBrainz.**

Previously dropped because the Apple Developer Program is $99/year. That
constraint is relaxed: the dev maintains an active membership, and Concerto
is distributed closed-source, so the embedded developer-token side is
funded out-of-band and the binary's threat model is no worse off than any
other proprietary app on the platform. End users remain at zero cost and
zero setup; there is no per-user account, no prompt, no config file.

Apple Music has the cleanest classical schema of any catalog —
`composerName`, `workName`, `movementName`, `movementNumber`,
`movementCount` are all first-class fields on Song entities, with
curation that's notably better than MB's community parsing in the
classical wing. It also has **no disc-ID endpoint**, so it cannot be a
primary resolver; it cannot identify a CD from a TOC. What it can do
extremely well is *enrich* an MB-resolved release whose work-rels are
sparse or missing — exactly the failure mode that produces wrong-shape
classical tags today.

The role: **Stage 1.5, between MB and CD-TEXT.** MB stays the
disc-ID-driven primary; Apple fills in work / movement / composer fields
when MB returns a release with weak classical structure. Gating logic
keeps the network spend low (§4.1a). If Apple fails for any reason, the
pipeline silently falls back to whatever MB returned (§4.1b).

#### 1.7.1 Auth model — ES256 JWT, no per-user setup

Apple Music API requires every request to carry an `Authorization:
Bearer <jwt>` header. The JWT is signed with the developer's private key
(a `.p8` file downloaded once from developer.apple.com) and identifies
the developer's team and key. Header:

```jsonc
{ "alg": "ES256", "kid": "<10-char keyID>", "typ": "JWT" }
```

Payload:

```jsonc
{ "iss": "<10-char teamID>", "iat": <now-unix>, "exp": <now+lifetime> }
```

The Apple-permitted maximum lifetime is six months. We use **one hour**
— it limits exposure if the key is ever rotated or leaked, the time-math
is easy to reason about, and the regeneration cost is microseconds (a
single ECDSA P-256 sign). The token is cached in process; on each Apple
Music request we check `now < exp - 5min` and regenerate lazily if not.

The `.p8` is a PKCS#8-wrapped EC private key on the P-256 (`prime256v1`
/ `secp256r1`) curve, PEM-encoded:

```
-----BEGIN PRIVATE KEY-----
MIGTAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBHkwdwIBAQQg...
-----END PRIVATE KEY-----
```

It is embedded in the binary via the Qt resource system (§1.7.3). The
keyID and teamID are 10-character ASCII strings; we embed them as
`constexpr` strings.

#### 1.7.2 Signing the JWT — macOS Security framework, no OpenSSL

We avoid OpenSSL entirely. macOS ships a complete ECDSA implementation
under `<Security/Security.h>`; everything we need is in Apple's public
SDK. No third-party crypto, no LGPL surface, no link-time complication.

The signing flow:

1. **Load the .p8 from QResource** as a `QByteArray`, strip the PEM
   armor (`-----BEGIN PRIVATE KEY-----` / `-----END PRIVATE KEY-----`
   markers and any whitespace), base64-decode to get the raw PKCS#8 DER.
2. **Import via `SecItemImport`** with `kSecFormatPKCS8` and
   `kSecItemTypePrivateKey`. This returns a `SecKeyRef` for the private
   key. (Alternative: parse the PKCS#8 wrapper ourselves to extract the
   raw EC key bytes and call `SecKeyCreateWithData` with
   `kSecAttrKeyType = kSecAttrKeyTypeECSECPrimeRandom` and
   `kSecAttrKeyClass = kSecAttrKeyClassPrivate`. `SecItemImport` is
   simpler.)
3. **Build the JWT signing input**: base64url-encode the header JSON,
   base64url-encode the payload JSON, concatenate with `.` →
   `<headerB64u>.<payloadB64u>`. ASCII bytes.
4. **Sign with `SecKeyCreateSignature`** passing
   `kSecKeyAlgorithmECDSASignatureMessageX962SHA256`. That algorithm
   constant tells Security framework to SHA-256 the input message itself
   (we hand it the unhashed JWT signing input) and produce an
   ASN.1-DER-encoded ECDSA signature.
5. **Convert DER signature → JWT raw R||S.** Apple returns
   `30 LL 02 rL <R...> 02 sL <S...>` (ASN.1 SEQUENCE of two INTEGERs).
   JWT (RFC 7515 / RFC 7518) requires raw concatenation of R and S, each
   left-padded with zeros to exactly 32 bytes for P-256 — total 64
   bytes. The INTEGER fields may carry a leading `0x00` byte (the ASN.1
   sign-padding rule when the high bit is set); strip it. They may also
   be *shorter* than 32 bytes if the value was small; left-pad with
   zeros. Both ends matter. A naive copy fails ~50% of the time at the
   server end.
6. **Base64url-encode** the 64-byte raw signature; append
   `.<sigB64u>` to the signing input.

C++ skeleton (≈60 lines, signatures only — actual implementation lives
in `AppleMusicJwt.{h,mm}`):

```cpp
// AppleMusicJwt.h
#pragma once
#include <QByteArray>
#include <QString>
#include <chrono>
#include <optional>

namespace concerto::metadata::applemusic {

struct JwtConfig {
    QByteArray p8Pem;        // raw .p8 contents
    QString    keyId;        // 10-char key identifier
    QString    teamId;       // 10-char team identifier
    std::chrono::seconds lifetime{3600};
};

class JwtSigner {
public:
    explicit JwtSigner(JwtConfig cfg);
    // Returns a current JWT; signs lazily, regenerates within 5min of exp.
    QString token();
private:
    QString sign() const;                                   // 1 ECDSA sign
    static QByteArray pemToDer(const QByteArray& pem);      // strip armor + b64dec
    static QByteArray derSigToJoseRaw(const QByteArray& der); // 30..02..02.. → R||S
    static QByteArray b64url(const QByteArray& bytes);

    JwtConfig m_cfg;
    QString   m_cached;
    qint64    m_expiresAtUnix = 0;
};

} // namespace
```

```objcpp
// AppleMusicJwt.mm — the SecKey* dance
#import <Security/Security.h>
#import <CoreFoundation/CoreFoundation.h>

QString JwtSigner::sign() const {
    QByteArray der = pemToDer(m_cfg.p8Pem);
    // CFData wraps the DER bytes without copying:
    CFDataRef pkcs8 = CFDataCreate(nullptr,
        reinterpret_cast<const UInt8*>(der.constData()), der.size());

    // Tell Security framework what we're importing.
    SecExternalFormat fmt = kSecFormatWrappedPKCS8;
    SecExternalItemType typ = kSecItemTypePrivateKey;
    SecItemImportExportKeyParameters params = {
        SEC_KEY_IMPORT_EXPORT_PARAMS_VERSION, 0, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    CFArrayRef out = nullptr;
    OSStatus s = SecItemImport(pkcs8, nullptr, &fmt, &typ,
                               kSecItemPemArmour, &params, nullptr, &out);
    // … (error check; release pkcs8)

    SecKeyRef key = (SecKeyRef)CFArrayGetValueAtIndex(out, 0);

    // Build the signing input: <header>.<payload>
    qint64 now = QDateTime::currentSecsSinceEpoch();
    QByteArray header  = b64url(QJsonDocument({
        {"alg","ES256"}, {"kid", m_cfg.keyId}, {"typ","JWT"}}).toJson(QJsonDocument::Compact));
    QByteArray payload = b64url(QJsonDocument({
        {"iss", m_cfg.teamId}, {"iat", now},
        {"exp", now + m_cfg.lifetime.count()}}).toJson(QJsonDocument::Compact));
    QByteArray msg = header + "." + payload;

    CFDataRef msgCf = CFDataCreate(nullptr,
        reinterpret_cast<const UInt8*>(msg.constData()), msg.size());
    CFErrorRef err = nullptr;
    CFDataRef sigDer = SecKeyCreateSignature(key,
        kSecKeyAlgorithmECDSASignatureMessageX962SHA256, msgCf, &err);
    // … (error check; release msgCf, out)

    QByteArray der2(reinterpret_cast<const char*>(CFDataGetBytePtr(sigDer)),
                    (int)CFDataGetLength(sigDer));
    QByteArray rs = derSigToJoseRaw(der2);              // 30..02..02.. → 64 bytes
    QByteArray jwt = msg + "." + b64url(rs);
    CFRelease(sigDer);
    return QString::fromLatin1(jwt);
}
```

`derSigToJoseRaw` is a ~25-line ASN.1 nibbler:

```cpp
QByteArray derSigToJoseRaw(const QByteArray& der) {
    // Expect: 0x30 totalLen 0x02 rLen <R> 0x02 sLen <S>
    int i = 0;
    if (der[i++] != 0x30) return {};                  // SEQUENCE
    int total = (quint8)der[i++];
    if (total & 0x80) { /* long-form length: not used at P-256 sizes */ }
    if (der[i++] != 0x02) return {};                  // INTEGER (R)
    int rLen = (quint8)der[i++];
    QByteArray r = der.mid(i, rLen); i += rLen;
    if (der[i++] != 0x02) return {};                  // INTEGER (S)
    int sLen = (quint8)der[i++];
    QByteArray s = der.mid(i, sLen);
    // Strip ASN.1 leading 0x00 sign-padding:
    while (r.size() > 1 && r[0] == 0x00) r.remove(0,1);
    while (s.size() > 1 && s[0] == 0x00) s.remove(0,1);
    // Left-pad to 32 bytes each (P-256 group order is 256 bits):
    QByteArray out(64, '\0');
    memcpy(out.data() + (32 - r.size()), r.constData(), r.size());
    memcpy(out.data() + 32 + (32 - s.size()), s.constData(), s.size());
    return out;
}
```

`b64url` is `QByteArray::toBase64(QByteArray::Base64UrlEncoding |
QByteArray::OmitTrailingEquals)` — Qt 6 has it built-in.

**No external crypto dep.** `<Security/Security.h>` is part of the macOS
SDK; `Security.framework` is already linked by every Cocoa app. JSON and
base64url are Qt 6 core. The total weight in the binary is ~200 lines of
C++ plus the framework that's already there.

**Apple docs cited:**
- `SecKeyCreateSignature` — `https://developer.apple.com/documentation/security/1643916-seckeycreatesignature`
- `kSecKeyAlgorithmECDSASignatureMessageX962SHA256` — `https://developer.apple.com/documentation/security/kseckeyalgorithmecdsasignaturemessagex962sha256`
- `SecItemImport` — `https://developer.apple.com/documentation/security/1394136-secitemimport`
- Apple Music API auth — `https://developer.apple.com/documentation/applemusicapi/generating-developer-tokens`

**Is there anything Qt 6 itself does cleaner?** No. QtNetworkAuth ships
OAuth 1/2 helpers but not JWT signing. QSslKey can load EC keys (Qt 6.4+
added P-256 support) but Qt has no public API for raw ECDSA signing — it
exposes signing through the TLS stack, not as a general primitive. The
direct Security framework call is shorter and avoids OpenSSL.

#### 1.7.3 Embedding the .p8 in the binary

The `.p8` is shipped as a Qt resource. `applemusic.qrc`:

```xml
<RCC>
  <qresource prefix="/keys">
    <file>applemusic.p8</file>
  </qresource>
</RCC>
```

`CMakeLists.txt` lists it next to the existing `.qrc` files. The .p8
itself stays out of git (`.gitignore`); build-time the developer drops
their key into `src/keys/applemusic.p8` (or wherever the .qrc points)
and the bundled binary picks it up. CI builds — when there is one —
read the key from a secret env var written to disk before `qmake`/`cmake`
runs.

Runtime load:

```cpp
QFile f(":/keys/applemusic.p8");
f.open(QIODevice::ReadOnly);
QByteArray pem = f.readAll();
```

**Threat model on the embedded key.** Qt resource data is compiled into
the binary but is *trivially extractable*: `strings`, a hex editor, or
the `binwalk`-style tooling reveals the PEM in seconds. This is
unavoidable for any client-side credential and is the accepted cost of
having a fully-automated zero-user-setup pipeline. The blast radius is
metadata-only catalog access; an extracted key gives an attacker the
ability to make Apple Music API calls under the dev's identity. Apple's
mitigations: per-app rate-limiting + 6-month-max key rotation. If an
extracted key shows up in the wild (visible from spikes in the dev's
Apple Music API analytics), the dev rotates: issue a new keyID on
developer.apple.com, update the embedded resource, ship a new build.
Old binaries keep working until Apple revokes the old key.

**Optional light obfuscation: not recommended.** XOR-ing the .p8 bytes
with a runtime-derived constant (e.g. concatenation of `__DATE__` +
some app-specific string, XORed against the PEM) would technically
slow a casual grep, but it adds compile-time + runtime complexity and
fails against any attacker willing to attach a debugger. The threat
model is "key rotates eventually anyway"; obfuscation buys ~zero
durability against a determined extractor and breaks reproducibility
in the build. **Ship the .p8 as a plain Qt resource. Rotate if abuse
shows up.**

#### 1.7.4 Catalog endpoints we use

Three endpoints. The first two are in the auto pipeline; the third is
documented but not in v1 (see end of this section). All take the
`Authorization: Bearer <jwt>` header.

**Storefront strategy.** Default to `us`. Classical metadata coverage in
Apple is strongest in `us` / `gb` / `de` / `jp`; using `us` as the
single value avoids per-locale fan-out and keeps the rate-limit budget
predictable. Re-evaluate in v2 if field testing shows non-US classical
gaps.

**(a) Barcode lookup — primary Stage 1.5 path.**

Triggered when MB Stage 1 produced a release with a barcode populated
and the gating in §4.1a is satisfied.

```
GET https://api.music.apple.com/v1/catalog/{storefront}/albums
       ?filter[upc]=<14-digit-or-13-digit-UPC>
       &include=tracks
Authorization: Bearer <jwt>
```

Trimmed response (classical-relevant fields only):

```jsonc
{
  "data": [{
    "id": "1481675577",
    "type": "albums",
    "attributes": {
      "name": "Rachmaninoff: Piano Concerto No. 3",
      "artistName": "Behzod Abduraimov, Royal Concertgebouw Orchestra & Valery Gergiev",
      "genreNames": ["Classical", "Music"],
      "releaseDate": "2020-01-17",
      "upc": "0190296872785",
      "artwork": {
        "url": "https://is1-ssl.mzstatic.com/.../{w}x{h}cc.jpg",
        "width": 3000, "height": 3000
      }
    },
    "relationships": {
      "tracks": {
        "data": [{
          "id": "1481675578",
          "type": "songs",
          "attributes": {
            "name": "Piano Concerto No. 3 in D Minor, Op. 30: I. Allegro ma non tanto",
            "composerName": "Sergei Rachmaninoff",
            "artistName": "Behzod Abduraimov, Royal Concertgebouw Orchestra & Valery Gergiev",
            "albumName": "Rachmaninoff: Piano Concerto No. 3",
            "workName": "Piano Concerto No. 3 in D Minor, Op. 30",
            "movementName": "I. Allegro ma non tanto",
            "movementNumber": 1,
            "movementCount": 3,
            "trackNumber": 1,
            "discNumber": 1,
            "durationInMillis": 970000,
            "isrc": "NLB502000123",
            "genreNames": ["Classical"]
          }
        }, /* … */ ]
      }
    }
  }]
}
```

Critical fields: `composerName`, `workName`, `movementName`,
`movementNumber`, `movementCount`. These are flat strings (not relations
into a Work entity), which is *less* powerful than MB's structured model
but vastly cleaner than MB's community-parsed track titles when MB's
work-rels are missing.

If `data[]` is empty (Apple has no album with that UPC), Stage 1.5
returns "no enrichment" and Stage 2 of the merge step (§5 / §4.1b) commits
the bare MB result unchanged.

**(b) ISRC lookup — secondary per-track Stage 1.5 path.**

Triggered when the MB result lacks per-track work-rels and the CD has
readable ISRCs in the subchannel Q-data (already extracted by the
existing ripper pipeline; populated on `TrackMeta.isrc` from the MMC
read or the MB record). Apple's ISRC endpoint is per-track and may
return multiple songs (different releases carrying the same recording):

```
GET https://api.music.apple.com/v1/catalog/{storefront}/songs
       ?filter[isrc]=<12-char-ISRC>
Authorization: Bearer <jwt>
```

```jsonc
{
  "data": [{
    "id": "<song id>",
    "type": "songs",
    "attributes": {
      "name": "Piano Concerto No. 3 in D Minor, Op. 30: I. Allegro ma non tanto",
      "composerName": "Sergei Rachmaninoff",
      "artistName": "Behzod Abduraimov & Royal Concertgebouw Orchestra",
      "workName": "Piano Concerto No. 3 in D Minor, Op. 30",
      "movementName": "I. Allegro ma non tanto",
      "movementNumber": 1,
      "movementCount": 3,
      "albumName": "Rachmaninoff: Piano Concerto No. 3",
      "isrc": "NLB502000123",
      "genreNames": ["Classical"]
    }
  }, /* maybe more — different albums carrying the same recording */ ]
}
```

When `data[]` has multiple entries, prefer the one whose
`attributes.albumName` matches the MB-resolved album title (case-folded,
diacritic-stripped Levenshtein < 5). If none match, take `data[0]` —
the same recording on a different album still gives correct composer /
work / movement.

ISRC lookups run **only for tracks that lack a composer in the MB
result** (per §4.1a gating). On a 12-track classical CD with 2
composer-less tracks, we issue 2 ISRC requests, not 12.

**(c) Fuzzy text search — NOT in v1 auto pipeline.**

```
GET https://api.music.apple.com/v1/catalog/{storefront}/search
       ?term=<url-encoded-query>&types=albums
Authorization: Bearer <jwt>
```

Listed for completeness. Considered as an optional Stage 1.6 (fires only
when MB + Stage 1.5 barcode + Stage 1.5 ISRC have all returned nothing
useful), but text search is unreliable enough on classical (matches the
*work* title across many performers' albums) that auto-committing its
result risks worse tags than the stub path. **Excluded from v1.** Add
later if field data shows a meaningful win.

#### 1.7.5 Rate limit and data license

Apple does not publish a public rate limit number for catalog reads; the
practical observed envelope is ~20 req/s per JWT in field use, and the
JWT is shared across all Concerto installs (because the user does not
authenticate — the JWT identifies the developer, not the user). One disc
rip generates **at most 1 album call + N ISRC calls** (only for
composer-less tracks). At a worst-case 10-track classical CD with all
ISRCs needing per-track lookup, that's 11 requests in <1 second per disc.
For the dev's user base, this is comfortably under any plausible limit.

If Apple ever returns `429`, the resolver retries once with 1s backoff
and then skips Stage 1.5 (commits the bare MB result; §4.1b).

**Data license.** Apple Music API responses are subject to the Apple
Developer Program License Agreement, §3.3.8 / §3.3.27 — broadly, data
returned by the API may be used to enable functionality in the app
that called it, may not be redistributed as a standalone dataset, and
must be discarded if the user no longer uses the feature. Concerto
writes the enriched values into the user's own FLAC tags on the user's
own disk; this is the standard model that every Apple Music-aware
tagger uses (e.g. Soundiiz, Doppler) and is consistent with the ADPLA.
We do *not* cache Apple Music JSON beyond the per-disc cache file (same
30-day TTL as MB).

**Apple docs cited:**
- Apple Music API overview — `https://developer.apple.com/documentation/applemusicapi/`
- Find Albums by UPC — `https://developer.apple.com/documentation/applemusicapi/get_multiple_catalog_albums_by_upc`
- Find Songs by ISRC — `https://developer.apple.com/documentation/applemusicapi/get_multiple_catalog_songs_by_isrc`
- Album resource — `https://developer.apple.com/documentation/applemusicapi/albums`
- Song attributes (composerName, workName, movementName, movementNumber, movementCount) — `https://developer.apple.com/documentation/applemusicapi/songs/attributes`

### 1.8 IDAGIO, Presto Music, Naxos, Qobuz — no useful public API

- **IDAGIO**: classical-only streaming, beautiful classical catalog,
  but no public developer API. They have a "web player integration"
  via internal SDK — partner-only.
- **Presto Music**: large classical retailer; no public API.
- **Naxos**: classical-first label/streaming service; no public
  metadata API.
- **Qobuz**: hi-res streaming with classical depth; partner API
  only.

All skip.

### 1.9 TheAudioDB, Last.fm, Spotify, Deezer — pop-first, weak on classical

- **TheAudioDB**: hobby DB, pop-oriented, sparse classical.
- **Last.fm**: scrobble-driven; metadata is user-supplied
  artist/title strings. Skip.
- **Spotify**: catalog API; classical coverage exists but composer
  is buried in `artists[]` with no role tagging. Skip for v1.
- **Deezer**: same shape as Spotify, weaker classical curation.
  Skip.

### 1.10 Cover Art Archive — pair with MB

`https://coverartarchive.org/release/<release-mbid>` returns a JSON
manifest of cover art for an MB release. Use to fetch a thumbnail to
embed in the FLAC files and display on the resolved rip-view banner.
Anonymous, no auth. Data is contributed by users under terms that
permit display in MB-using apps. Image files (250px, 500px, 1200px
thumbnails) at
`https://coverartarchive.org/release/<mbid>/<image-id>-<size>.jpg`.

Also doubles as the "has cover art?" check for the §3.3.1 scoring
function: HEAD on the front-500 URL returns 200 or 307 if art exists,
404 if not. Cache the result in the disc-cache row.

**Source:** [Cover Art Archive API](https://musicbrainz.org/doc/Cover_Art_Archive/API)

### 1.11 Sources comparison (one table) (UPDATED 2026-05-16 — Apple Music)

Classified by auth tier — only the first two tiers are in the pipeline.

#### Truly anonymous & free (in pipeline)

| Source | Classical-aware fields | Disc-ID lookup | Auth | Rate | Data license | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| MusicBrainz WS/2 | composer, conductor, performer, orchestra, work, movement (first-class) | Yes (MB disc id) | None — UA string only | 1 req/s/IP | CC0 core | **Primary** |
| Cover Art Archive | n/a (artwork only) | via MB release MBID | None | None published | Mixed per image | Pair with MB |
| CD-TEXT (on-disc) | composer, performer, title (when present) | On-disc | n/a | n/a | n/a | **Secondary** |

#### Free-but-developer-key (single key shipped in the binary, no per-user setup)

| Source | Notes | Verdict |
| --- | ----- | ------- |
| AcoustID `/v2/lookup` | Application API key registered once by the dev, embedded in the binary; user does nothing. ToS permits. Picard/beets do exactly this. | **Tertiary** (gated on Chromaprint fingerprint of track 1) |
| Apple Music API (catalog) | ES256 JWT signed in-process from an embedded `.p8`. Dev funds the Apple Developer Program out-of-band; user pays nothing. Closed-source distribution; embedded credentials carry the standard extractable-secret risk, mitigated by 6-month key rotation. No disc-ID endpoint → cannot resolve discs alone. | **Stage 1.5 enrichment over MB** (gated; §4.1a) |

#### Free-but-per-user-account (excluded)

| Source | Why excluded |
| ------ | ------------ |
| Discogs `/database/search` | PAT bound to a user account since 2023; rate limit is per-token, so an app-wide token doesn't scale and ToS forbids it anyway. |
| Last.fm | API key bound to a user account; classical coverage is weak regardless. |

#### Paid / partner-only (excluded)

| Source | Why excluded |
| ------ | ------------ |
| IDAGIO / Presto / Naxos / Qobuz | Partner-only API; not available to indie developers. |

#### Already in Concerto for other reasons (not metadata sources)

| Source | Used for | Why not metadata |
| ------ | -------- | ---------------- |
| AccurateRip dBAR | Rip verification CRCs | No titles/artists/composers |
| CUETools DB (CTDB) | Rip verification + parity | Replicates MB/Discogs; query MB directly |
| gnudb / freedb (legacy) | n/a | No composer field in schema — useless for classical |

---

## 2. Classical-music modelling

### 2.1 The MB model in three lines

- A **Recording** is a single take/performance of a Work.
- A **Work** is a composition (e.g. "Symphony No. 9 in D minor,
  Op. 125").
- Recording → Work via `performance` relationship.
- Work → Composer via `composer` relationship on the Work.
- Recording → Performers via `instrument` / `vocal` /
  `performing orchestra` / `conductor` relationships on the
  Recording.
- Tracks reference Recordings; a Recording can appear on many
  tracks across many releases (typical for reissues).

### 2.2 Multi-movement works

MB models movements as **separate Works**, related to the parent
symphony via a Work-Work `parts` relationship. The movement-level
Work usually has a title like
`"Piano Concerto no. 3 in D minor, op. 30: I. Allegro ma non tanto"`
— the parent-title-colon-movement-title convention is enforced
by MB style guidelines for classical.

For v1 Concerto: split on `": "` to extract parent (left) and
movement (right). For v2, walk the parent-work chain (beets'
`parentwork` plugin does this) to handle edge cases like
`"Piano Concerto no. 3: I. Allegro: a. introduction"` where two
colons appear.

### 2.3 Recording-Artist vs Track-Artist in MB

MB's classical style:
- **Track-Artist** = the credited artist string on the release
  (often "Composer; Performer, Orchestra, Conductor"). This is
  what's printed on the CD jacket.
- **Recording-Artist** = the principal performer(s) of the
  Recording, separate from the composer.
- The Work's composer is via Work artist-rels.

For tagging: Concerto should write
- `TPE1` / `ARTIST` = the joined artist-credit string (sticks
  closest to what's on the CD; this is the existing code's
  current behaviour, keep it).
- `TPE2` / `ALBUMARTIST` = release-level artist-credit (often the
  same as TPE1 on classical, but on box sets it's the conductor
  or the orchestra).
- `TCOM` / `COMPOSER` = from the Work artist-rels, with
  `target-credit` preferred over `artist.name`.
- `TPE3` / `CONDUCTOR` = from the Recording artist-rels.
- `TPE2` semantics differ slightly per player: Roon and Apple
  Music both treat ALBUMARTIST as the primary album-level credit;
  for box sets, classical-first players expect the conductor or
  ensemble there. Default to the release's artist-credit joined
  string and let the user override.

### 2.4 Canonical tag mapping (single source of truth)

Adopted verbatim from MusicBrainz Picard 2.x ("Appendix B: Tag
Mapping" — fetched live during research and parsed below). This is
the table to encode as a constant in `MetadataResolver` and to
respect when writing FLAC (Vorbis), and in future MP3 (ID3v2.4) or
AAC (MP4):

| Logical field | ID3v2.4 frame | Vorbis (FLAC) | iTunes MP4 |
| --- | --- | --- | --- |
| Title (track / movement) | `TIT2` | `TITLE` | `©nam` |
| Subtitle | `TIT3` | `SUBTITLE` | `----:com.apple.iTunes:SUBTITLE` |
| Work | `TXXX:WORK` (also `TIT1` for compat) | `WORK` | `©wrk` (Picard ≥ 2.1) |
| Movement name | `MVNM` | `MOVEMENTNAME` | `©mvn` |
| Movement number | `MVIN` | `MOVEMENT` | `mvi` |
| Movement total | `MVIN` (same frame, second number) | `MOVEMENTTOTAL` | `mvc` |
| Show work & movement | `TXXX:SHOWMOVEMENT` | `SHOWMOVEMENT` | `shwm` |
| Composer | `TCOM` | `COMPOSER` | `©wrt` |
| Composer sort | `TSOC` (≥1.3) / `TXXX:COMPOSERSORT` (≤1.2) | `COMPOSERSORT` | `soco` |
| Conductor | `TPE3` | `CONDUCTOR` | `----:com.apple.iTunes:CONDUCTOR` |
| Artist (track) | `TPE1` | `ARTIST` | `©ART` |
| Artist (multi-valued) | `TXXX:Artists` | `ARTISTS` | `----:com.apple.iTunes:ARTISTS` |
| Artist sort | `TSOP` | `ARTISTSORT` | `soar` |
| Album artist | `TPE2` | `ALBUMARTIST` | `aART` |
| Album artist sort | `TSO2` (≥1.2) / `TXXX:ALBUMARTISTSORT` (≤1.1) | `ALBUMARTISTSORT` | `soaa` |
| Performer (per-instrument) | `TMCL:<instrument>` (v2.4) / `IPLS:<instrument>` (v2.3) | `PERFORMER` (with "{artist} (instrument)" convention) | n/a |
| Arranger | `TIPL:arranger` (v2.4) / `IPLS:arranger` (v2.3) | `ARRANGER` | n/a |
| Lyricist | `TEXT` | `LYRICIST` | `----:com.apple.iTunes:LYRICIST` |
| Writer | `TXXX:Writer` (≥1.3) | `WRITER` | n/a |
| Album | `TALB` | `ALBUM` | `©alb` |
| Album sort | `TSOA` | `ALBUMSORT` | `soal` |
| Title sort | `TSOT` | `TITLESORT` | `sonm` |
| Date | `TDRC` (v2.4) / `TYER`+`TDAT` (v2.3) | `DATE` | `©day` |
| Original date | `TDOR` (v2.4) / `TORY` (v2.3) | `ORIGINALDATE` | n/a |
| Original year | n/a | `ORIGINALYEAR` | n/a |
| Disc number | `TPOS` (e.g. "1/10") | `DISCNUMBER` | `disk` |
| Total discs | `TPOS` (same frame) | `DISCTOTAL` / `TOTALDISCS` | `disk` |
| Track number | `TRCK` (e.g. "5/12") | `TRACKNUMBER` | `trkn` |
| Total tracks | `TRCK` (same frame) | `TRACKTOTAL` / `TOTALTRACKS` | `trkn` |
| Label | `TPUB` | `LABEL` | `----:com.apple.iTunes:LABEL` |
| Catalog number | `TXXX:CATALOGNUMBER` | `CATALOGNUMBER` | `----:com.apple.iTunes:CATALOGNUMBER` |
| ISRC | `TSRC` | `ISRC` | `----:com.apple.iTunes:ISRC` |
| Barcode | `TXXX:BARCODE` | `BARCODE` | `----:com.apple.iTunes:BARCODE` |
| ASIN | `TXXX:ASIN` | `ASIN` | `----:com.apple.iTunes:ASIN` |
| Disc subtitle | `TSST` (v2.4 only) | `DISCSUBTITLE` | `----:com.apple.iTunes:DISCSUBTITLE` |
| Grouping | `TIT1` / `GRP1` | `GROUPING` | `©grp` |
| Genre | `TCON` | `GENRE` | `©gen` |
| Language | `TLAN` | `LANGUAGE` | `----:com.apple.iTunes:LANGUAGE` |
| Script | `TXXX:SCRIPT` | `SCRIPT` | `----:com.apple.iTunes:SCRIPT` |
| Compilation | `TCMP` | `COMPILATION` | `cpil` |
| Media | `TMED` | `MEDIA` | `----:com.apple.iTunes:MEDIA` |
| Release country | `TXXX:MusicBrainz Album Release Country` | `RELEASECOUNTRY` | (TXXX-style atom) |
| Release status | `TXXX:MusicBrainz Album Status` | `RELEASESTATUS` | (TXXX-style atom) |
| Release type | `TXXX:MusicBrainz Album Type` | `RELEASETYPE` | (TXXX-style atom) |
| MB Track ID | `UFID:http://musicbrainz.org` | `MUSICBRAINZ_TRACKID` | `----:com.apple.iTunes:MusicBrainz Track Id` |
| MB Recording ID | `UFID:http://musicbrainz.org` (above is Recording in Picard) | `MUSICBRAINZ_TRACKID` | (same) |
| MB Release ID | `TXXX:MusicBrainz Album Id` | `MUSICBRAINZ_ALBUMID` | `----:com.apple.iTunes:MusicBrainz Album Id` |
| MB Release-Group ID | `TXXX:MusicBrainz Release Group Id` | `MUSICBRAINZ_RELEASEGROUPID` | (...) |
| MB Artist ID | `TXXX:MusicBrainz Artist Id` | `MUSICBRAINZ_ARTISTID` | (...) |
| MB Album-Artist ID | `TXXX:MusicBrainz Album Artist Id` | `MUSICBRAINZ_ALBUMARTISTID` | (...) |
| MB Work ID | `TXXX:MusicBrainz Work Id` | `MUSICBRAINZ_WORKID` | `----:com.apple.iTunes:MusicBrainz Work Id` |
| MB Composer ID | `TXXX:MusicBrainz Composer Id` | `MUSICBRAINZ_COMPOSERID` | (...) |
| MB Release-Track ID | `TXXX:MusicBrainz Release Track Id` | `MUSICBRAINZ_RELEASETRACKID` | (...) |
| MB Disc ID | `TXXX:MusicBrainz Disc Id` | `MUSICBRAINZ_DISCID` | (...) |
| AcoustID Id | `TXXX:Acoustid Id` | `ACOUSTID_ID` | `----:com.apple.iTunes:Acoustid Id` |

(Source: [Picard tag mapping appendix](https://picard-docs.musicbrainz.org/en/latest/appendices/tag_mapping.html),
parsed verbatim from the HTML during this research. The MP3 entries
for `performer:instrument` use `TMCL` in ID3v2.4 with a sub-frame
where the *description* names the instrument and the *value* names
the artist — multi-valued. ID3v2.3 falls back to `IPLS`.)

#### 2.4.1 Practical write rules for v1 FLAC

FLAC is the only format Concerto currently writes (`FlacEncode`), so
this is the only column that matters for the first release. Use
Vorbis comments multi-valued where MB returns multiple values
(multiple soloists, multiple conductors). Concrete output for a
single Rachmaninoff track:

```
ALBUM=Rachmaninoff: Piano Concerto no. 3
ALBUMARTIST=Rachmaninoff; Abduraimov, Concertgebouworkest, Gergiev
ARTIST=Rachmaninoff; Abduraimov, Concertgebouworkest, Gergiev
TITLE=Piano Concerto no. 3 in D minor, op. 30: I. Allegro ma non tanto
WORK=Piano Concerto no. 3 in D minor, op. 30
MOVEMENTNAME=I. Allegro ma non tanto
MOVEMENT=1
MOVEMENTTOTAL=3
SHOWMOVEMENT=1
COMPOSER=Sergei Rachmaninov
COMPOSERSORT=Rachmaninoff, Sergei Vasilievich
CONDUCTOR=Valery Gergiev
PERFORMER=Behzod Abduraimov (piano)
PERFORMER=Koninklijk Concertgebouworkest (orchestra)
DATE=2020-01-17
ORIGINALDATE=2020-01-17
DISCNUMBER=1
DISCTOTAL=1
TRACKNUMBER=1
TRACKTOTAL=4
LABEL=RCO Live
CATALOGNUMBER=RCO 19003
BARCODE=190296872785
MUSICBRAINZ_ALBUMID=853b6a62-116a-4a11-bc22-533b7cd331e7
MUSICBRAINZ_RELEASEGROUPID=187cc74d-7afe-41d0-af0f-d7dfa3a1be57
MUSICBRAINZ_TRACKID=<recording-mbid>
MUSICBRAINZ_DISCID=<our mb disc id>
MUSICBRAINZ_WORKID=e55e8d47-c07a-31f5-90e4-404b8e975255
MUSICBRAINZ_ARTISTID=<each performer mbid, multi-value>
MUSICBRAINZ_COMPOSERID=44b16e44-da77-4580-b851-0d765904573e
MEDIA=CD
RELEASESTATUS=Official
RELEASETYPE=Album
COMPILATION=0
```

`SHOWMOVEMENT=1` is what tells Apple Music players (Music.app) to
render the `Work / Movement` view instead of the raw track title.
Roon honours `WORK` + `MOVEMENT`/`MOVEMENTNAME` directly and is
agnostic on `SHOWMOVEMENT`. Plex respects `WORK` and `MOVEMENT*`
since v1.30.

---

## 3. Multi-disc sets and disambiguation

### 3.1 The data model

- **Release** = one physical product (a specific pressing). Has
  `media[]`, one entry per disc.
- **Medium** = one disc inside a release. Has `position` (1-based)
  and `track-count`. The `discs[]` array on each medium lists every
  MB disc ID submitted for that pressing of that medium.
- **Release Group** = a logical album. All re-issues of the same
  album share a release-group ID. Critical key for the
  Concerto batch model (you can rip disc 3 of a 10-disc box from
  one re-issue and disc 4 from another; both share a
  release-group).

### 3.2 The matching algorithm (existing code is 90% there)

`src/MusicBrainz.cpp::pickMedium` already implements:
1. Look for the medium whose `discs[].id` contains our queried
   disc id (best case).
2. Else, look for the first medium with a matching `track-count`.
3. Else, the first medium of the release.

This is sound. For multi-disc sets, expose the chosen medium's
`position` and the release's total `media[].length` — already
plumbed via `Disc::position` and `Disc::totalCount`. Good.

### 3.3 Deterministic auto-pick when disc-ID maps to multiple releases (UPDATED 2026-05-16)

Common case: an original pressing and three reissues all hash to the
same MB disc-ID. `releases[]` returns multiple entries. Under the new
constraints we never prompt — we compute a score per candidate, take the
highest, log the entire scoring table to the cache, and commit. Score is
deterministic, so the same disc always yields the same pick on every
machine (reproducibility is a feature; if the pick is wrong the dev can
ship a corrected scoring weight and the next run flips the answer).

#### 3.3.1 Scoring function

Inputs: a candidate release JSON (from the disc-ID endpoint, includes
artist-credits + recordings + release-groups). The score starts at 0.

```
score(release):
    s = 0
    # Strong signals — "is this a classical-quality release?"
    if any recording in release has a relation of type "composer"
       on a work-rel target:                            s += 50
    if any recording has a relation of type "conductor"
       or "performing orchestra":                       s += 20
    # Curation proxies — well-edited releases populate these
    if release.barcode is non-empty:                    s += 30
    if release.country in {US, GB, DE, JP, FR}:         s += 15
    s += 10 * count(artist-credit entries with .artist.id set)
    if release.release-group.primary-type == "Album":   s += 5
    # Penalties
    if release.status in {"Bootleg", "Pseudo-Release"}: s -= 20
    if release has no cover art:                        s -= 10
    return s

pick(candidates):
    sorted = sort by:
      key1: score(c)                                     descending
      key2: c.release-group.first-release-date           ascending  (earliest wins)
      key3: c.id                                         ascending  (lex on MBID)
    return sorted[0]
```

Notes:

- The "any recording has a composer/conductor relation" checks need the
  recording-level rels, which the disc-ID endpoint **does not** return.
  Workaround: at disc-ID lookup time we don't have work-rels — we only
  see whether `recording[]` contains entries at all. So we score what we
  *do* have at the disc-ID stage:
  - +50 collapses to "+50 if `media[0].tracks[].recording` has
    `artist-credit` entries that include both a Person-type artist (the
    composer) **and** a Group-type artist (the orchestra) — a proxy for
    classical-style credit." This is imperfect; we accept it.
  - The full work-rel-based +50 / +20 scoring happens **after** the
    second-hop release fetch on the winner — at which point we already
    committed. If we want a re-pick on second-hop disagreement, do
    §3.3.2.
- "no cover art" is checked via the CAA `release/<mbid>/front-500.jpg`
  HEAD response. We cache the result so we don't re-HEAD per scoring
  call.

#### 3.3.2 Optional merge strategy (deferred to v2)

An alternative to "pick one and commit" is "fetch the top 2-3 candidates,
merge fields where they agree, prefer the higher-scored on conflicts."
For classical, remasters frequently share composer/performer but differ
on label/catalog-number. Merging would produce a more robust result.

Trade-off: each extra candidate = one extra second-hop request (+1
second of latency under MB's rate cap). At 3 candidates the rip stalls
by 3 seconds.

**Recommendation:** ship v1 with single-pick. Add merge in v2 if we
observe enough wrong-label / wrong-catalog tags in field testing.

#### 3.3.3 Honest accuracy estimate

There is **no published MB statistic on classical-coverage accuracy** —
the team publishes total counts (5.5 million releases, ~3,900 Symphony
works, ~7,100 Concerto works, ~7,800 Sonata works as of mid-2026) but
not "what fraction of inserted CDs find a match." So this is a bounded
estimate from first-hand experience and structural reasoning, not a
benchmark:

| Outcome | Estimated share of classical CDs in the wild | Reasoning |
| ------- | --- | --------- |
| Single unambiguous MB disc-ID match | ~55–70% | Major labels (DG, Decca, EMI, Sony Classical, Naxos, Harmonia Mundi) are well-covered; mid-tier (Hyperion, Chandos, BIS) likewise. Boxes pressed 2005+ are nearly all submitted. |
| Multiple MB matches; scoring picks correctly | ~15–20% with ~80% pick-correctness | The common multi-match case is "original 199x pressing + 200x reissue + 201x reissue." Composer/conductor/orchestra relations carry over to all of them; the score's job is mostly to prefer the well-edited one (barcode+country+cover-art weights). Wrong picks usually still produce *correct* composer/performer (the metadata that matters); they get the wrong label or catalog number. |
| Zero MB matches; CD-TEXT rescues | ~5–10% with very degraded quality | Mostly non-classical (because no major classical label authors CD-TEXT). When CD-TEXT *does* turn up on classical, it's title-only — no composer. So this branch produces minimally-useful tags. |
| Zero MB, CD-TEXT empty; AcoustID rescues | ~5–10% with ~70% correctness | AcoustID works track-by-track and looks up a recording, not a release. We then walk to candidate releases and apply §3.3.1 scoring. The "wrong release of the right recording" failure mode is more common here than in disc-ID hits. |
| Total miss — Unknown Album stub | ~2–5% | Private recordings, label test pressings, very rare imports, plus a long tail of obscure indie classical. |

Caveat: the user's collection skews these wildly. A user who only buys
DG box sets sees ~95% single-match. A user who buys obscure Eastern
European indie classical sees the long tail.

The number to internalize: **the pipeline always commits a tag set,
and the *correct* composer/performer is in those tags ~85% of the
time. Wrong-but-plausible labels and catalog numbers happen on ~10%.
Stubs happen on ~3%.** We track per-pipeline-stage outcomes in the
cache so we can revisit these estimates with field data.

### 3.4 Box-set workflow (UPDATED 2026-05-16)

`Ripper::batchChanged` plus `RipBatchStore` already track multi-disc
batches via `releaseGroupId`. Under the no-prompt constraint:

- When the user inserts a disc whose `release-group.id` matches an
  in-progress batch, auto-route the rip into that batch's folder.
  (`Ripper.h::batchExpectedDisc` already exposes the next disc;
  validate the inserted disc's `media.position` against it.)
- When the inserted disc's release-group doesn't match the
  in-progress batch, **start a new batch silently in a new folder**;
  do not prompt. The old batch stays open in `RipBatchStore` (the user
  can resume it later by re-inserting one of its discs). The
  diagnostic banner in the rip view shows "New batch started:
  `<title>`" — informational, not interactive.

### 3.5 Disc IDs that aren't unique (UPDATED 2026-05-16)

Two different albums can share a disc ID, especially short discs (EPs,
single-CD compilations). MB documents this and provides a "move disc id"
tool on the website. The user-facing failure mode: ambiguous-disc-id
releases that look totally unrelated land in the same `releases[]`
array.

Recovery (no chooser available):

- The scoring function (§3.3.1) handles "totally unrelated" by ranking
  the classical-rich release above the rock-EP-with-same-TOC release —
  a pop EP and a Brahms intermezzo collection won't have the same
  composer/conductor signals.
- Add a **duration-similarity bonus** to the score: for each candidate,
  compute the sum-of-absolute-differences between the candidate's
  recording lengths and our TOC track durations. Bonus = `max(0, 30 -
  totalDeviationSec)`. A release whose track durations differ by >30s
  total scores 0 here; a perfect duration match scores +30. This makes
  the "two unrelated albums with the same disc-ID" case usually pick
  the right one.
- The cache log records the duration deviation per candidate; if a
  wrong pick happens, the dev can see immediately whether duration was
  the deciding factor or the classical-signal weights.

---

## 4. Query strategy & fallback chain (UPDATED 2026-05-16 — Apple Music)

### 4.1 The single, deterministic, fully-automated pipeline

```
[Insert disc]
  │
  ├─ Compute MB disc-ID locally from the TOC (already in
  │  ArVerify::computeDiscIds; clean-room SHA-1-based algorithm).
  │
  ├─ Stage 1: MusicBrainz disc-ID lookup
  │  GET /ws/2/discid/<id>?inc=artist-credits+recordings+release-groups
  │  ├─ N>=1 candidates ─► score by §3.3 ─► top-1 wins ─► Stage 1b
  │  └─ N=0 / 404         ─► Stage 2
  │
  ├─ Stage 1b: Second-hop release fetch (composer/conductor/performer)
  │  GET /ws/2/release/<mbid>?inc=artist-credits+recordings+work-rels+
  │      work-level-rels+artist-rels+recording-level-rels+release-groups+
  │      labels+isrcs
  │  ─► parse → flatten ─► enrichment gate (§4.1a) ─► Stage 1.5 or DONE
  │
  ├─ Stage 1.5: Apple Music enrichment (CONDITIONAL; UPDATED 2026-05-16 — Apple Music)
  │  Sign JWT (cached, 1h lifetime). For the path the gate chose:
  │    barcode path: GET /v1/catalog/{sf}/albums?filter[upc]=<upc>&include=tracks
  │    ISRC path:    GET /v1/catalog/{sf}/songs?filter[isrc]=<isrc>  (per composer-less track)
  │  ├─ enrichment data returned ─► merge with MB result per §5
  │  │                              ─► tag ─► DONE (confidence=High,
  │  │                                  source=musicbrainz+applemusic)
  │  └─ Apple miss / 4xx / 5xx / timeout
  │                              ─► silently skip; commit bare MB result
  │                                  ─► tag ─► DONE (confidence=High, source=musicbrainz)
  │
  ├─ Stage 2: CD-TEXT (on-disc, MMC format 0x05)
  │  IOCDMedia::readTOC(buffer, 0x05) → parsePacks()
  │  ├─ pack data present ─► best-effort fill (album, title-per-track,
  │  │                       performer, composer if 0x83 present)
  │  │                       ─► tag → DONE (confidence=Medium, source=CD-TEXT)
  │  └─ empty / no packs   ─► Stage 3
  │
  ├─ Stage 3: Chromaprint + AcoustID
  │  fingerprint(track1_PCM) → POST /v2/lookup
  │  ├─ result with score >= 0.5 → walk to releases → score by §3.3 →
  │  │   second-hop → DONE (confidence=Medium, source=AcoustID)
  │  └─ nothing / low confidence  ─► Stage 4
  │
  └─ Stage 4: Stub
     write ALBUM="Unknown Album (Disc ID: <id>)",
           ARTIST="Unknown Artist", TRACKNUMBER=N
     queue (discId, toc, ripDirHash) into pending-submissions.db
     ─► DONE (confidence=None, source=stub)
```

Every stage commits. No prompt. No "Unresolved" state. Stage 1.5
**enriches** the Stage 1 winner; it never originates a release. If Stage
1 missed, Stage 1.5 is skipped entirely (Apple has no disc-ID lookup;
there is nothing to enrich).

### 4.1a Enrichment gate — when Stage 1.5 fires (UPDATED 2026-05-16 — Apple Music)

Stage 1.5 is **not** unconditional. It costs one (rarely a few) JWT-signed
HTTP requests, and MB's classical curation is good enough that the
majority of well-edited releases need no help. The gate runs over the
flattened `AlbumMeta` produced by Stage 1b:

```
def stage15_decision(meta, toc):
    # No barcode and no ISRC → nothing to look up.
    has_barcode    = bool(meta.barcode)
    composerless_tracks_with_isrc = [
        t for t in meta.tracks if not t.composerName and t.isrc
    ]

    if has_barcode:
        # "Is MB's classical structure incomplete?"
        n_tracks       = len(meta.tracks)
        n_no_workrel   = sum(1 for t in meta.tracks if not t.workId)
        any_no_composer= any(not t.composerName for t in meta.tracks)
        sparse = (n_tracks > 0 and
                  (n_no_workrel / n_tracks >= 0.30 or any_no_composer))
        if sparse:
            return ("barcode", meta.barcode)

    # Per-track ISRC pass — only for tracks where MB has no composer
    # AND a usable ISRC is present (either from MB or from the CD Q-data).
    if composerless_tracks_with_isrc:
        return ("isrc", composerless_tracks_with_isrc)

    return ("skip", None)
```

Pseudocode is the canonical version; the implementation lives in
`AppleMusicProvider::shouldEnrich(meta)`.

Plain-English summary:

- If MB returned a barcode **and** the result is "classical-sparse"
  (≥30% of tracks have no Work entity, OR any track is composer-less) →
  **fire the album-by-UPC call.**
- Else, if any individual track lacks a composer in MB **and** has a
  readable ISRC → **fire the per-ISRC call(s) for just those tracks.**
- Otherwise → **skip Stage 1.5.** MB nailed it; no Apple call.

Triggering both paths in the same disc rip is allowed but uncommon: the
album call usually fills the composer/work fields wholesale, so the
ISRC pass would find nothing to do. The implementation orders them
album-first; if the album call already populated every composer-less
track, the ISRC pass short-circuits.

The decision is logged to `scoringLog` (existing structure) under a new
key `stage15_decision: {path, reason, tracksTargeted}` so the dev can
see why each disc did or didn't enrich.

### 4.1b Apple Music failure mode — degrade silently (UPDATED 2026-05-16 — Apple Music)

If the Apple Music call fails for any reason — `401`/`403` (key revoked
or JWT malformed), `429` (rate-limited, after one retry), `4xx` other,
`5xx`, network error, or a timeout (default 5s) — Stage 1.5 returns "no
enrichment" and the pipeline commits the bare Stage 1b result. The
failure is recorded in `pipelineLog` (existing structure) with the HTTP
status and message:

```jsonc
{ "stage": "applemusic", "outcome": "fail",
  "reason": "http 401: invalid token", "durationMs": 312 }
```

**Apple Music being down never blocks a rip.** This is a non-negotiable
property: MB is the source of truth, Apple is decoration. The user sees
a successful rip with whatever MB returned; the source badge in the rip
view reads "MusicBrainz" rather than "MusicBrainz + Apple Music," and
that's the only externally observable difference.

The dev sees the failure in `pipelineLog`. A pattern of `401`s indicates
the embedded `.p8` was revoked (rotate); a pattern of `429`s indicates
sustained high request volume (rare given the gate). 5xx is Apple's
problem; nothing for us to do but retry on the next disc.

### 4.2 Stage details

#### Stage 1: MusicBrainz disc-ID lookup

`https://musicbrainz.org/ws/2/discid/<id>?fmt=json&inc=artist-credits+recordings+release-groups`

- 200 with `releases[]` non-empty → rank by §3.3.1 → take top-1 → Stage 1b.
- 200 with `releases[]` empty, or 404 → Stage 2.
- 503 → respect `Retry-After`, exponential backoff (max 3 retries, then
  treat as "MB unreachable" — log and proceed to Stage 2; the cache is
  not poisoned because we don't write a negative entry).

Disc-ID algorithm (already in `ArVerify::computeDiscIds`):

- SHA-1 over an ASCII string built from: `printf("%02X", firstTrack)`,
  `printf("%02X", lastTrack)`, then for 100 iterations `printf("%08X",
  frameOffset[i])` where `frameOffset[0]` is the lead-out and
  `frameOffset[1..lastTrack]` are per-track start LBAs (+150 for the
  pre-lead-in offset), padded with zeros up to index 99.
- SHA-1 digest base64-encoded with MB's variant alphabet: standard
  base64 but `+`/`/`/`=` replaced with `.`/`_`/`-`.
- Result is 28 chars including padding.

#### Stage 1b: Release second-hop

`https://musicbrainz.org/ws/2/release/<rel-mbid>?fmt=json&inc=artist-credits+recordings+work-rels+work-level-rels+artist-rels+recording-level-rels+release-groups+labels+isrcs`

Parse work-rels for composer (per Section 1.1). Flatten into `AlbumMeta`,
cache, tag.

#### Stage 2: CD-TEXT

`IOCDMedia::readTOC(buffer, format=0x05, formatAsTime=0, track=0,
&actualByteCount)`. Returns the 4-byte header + 18-byte pack stream
described in §1.3.3. Parse with our own `parsePacks()` (clean-room from
the MMC-3 Annex J spec; ~40 lines of state machine).

Map to `AlbumMeta`:
- `TITLE` pack at track 0 → `AlbumMeta.title`
- `TITLE` pack at track N → `tracks[N].title`
- `PERFORMER` track 0 → `AlbumMeta.artistCredit`
- `COMPOSER` track N → `tracks[N].composerName` (rare on classical CDs,
  but when present, gold)
- `UPC_EAN` at track 0 → `AlbumMeta.barcode`
- `ISRC` per track → `tracks[N].isrc`

If `parsePacks()` returns at least an album title and per-track titles,
emit the tag set with `confidence=Medium`, `sourceTag="cd-text"`. If only
the album title appeared, still commit.

#### Stage 3: Chromaprint + AcoustID

Pre-condition: track 1's PCM is in memory (we just ripped it; we hold the
buffer until Stage 1 + 2 confirm a miss, then either reuse or re-ask the
ripper for it).

```
fp = chromaprint::fingerprint(pcm, sampleRate=44100, channels=2, durationSec)
GET https://api.acoustid.org/v2/lookup
  ?client=<embedded app key>
  &meta=recordings+releases+releasegroups+sources
  &duration=<int>
  &fingerprint=<fp>
  &format=json
```

- `results[].score >= 0.5` → take highest. From `recording-mbid`, look up
  every release that contains it (`/ws/2/recording/<rec-mbid>?inc=releases`)
  → apply §3.3.1 scoring on those releases → top-1 → Stage 1b (second-hop
  on that release).
- All scores `< 0.5`, or `results[]` empty → Stage 4.

#### Stage 4: Stub

Write deterministic minimal tags:

```
ALBUM=Unknown Album (Disc ID: ABCDEFG_xyz123-)
ALBUMARTIST=Unknown Artist
ARTIST=Unknown Artist
TITLE=Track <NN>
TRACKNUMBER=<N>
TRACKTOTAL=<N>
DISCNUMBER=1
DISCTOTAL=1
MUSICBRAINZ_DISCID=<id>
```

Append a JSON row to `<AppDataLocation>/pending-submissions.jsonl` with
the full TOC, the disc-ID, the path to the ripped files, and the
timestamp. The dev (only, manually) batch-submits these to MB later.

### 4.3 No-prompt rule

Earlier drafts had a "when to prompt vs auto-resolve" table. Under the
new constraints we never prompt. The decision table is gone; the
pipeline above is the entire policy. The cache log records every
decision and the score that drove it (Section 6.3), which is what the
dev consults when investigating wrong picks — not the user.

### 4.4 Pseudocode for the orchestrator (UPDATED 2026-05-16 — Apple Music)

```cpp
AlbumMeta resolve(const Toc& toc, const PcmBuffer& track1Pcm) {
    QString discId = computeMbDiscId(toc);

    if (auto cached = cache.get(discId, /*maxAgeDays=*/30); cached) {
        return *cached;
    }

    // Stage 1 + 1b
    auto releases = mb.lookupByDiscId(discId);  // may be empty
    if (!releases.isEmpty()) {
        auto scored = scoreAndRank(releases, toc);  // §3.3.1
        auto winner = scored.first();
        auto full   = mb.fetchRelease(winner.releaseId);  // §1.1 second hop
        auto flat   = flatten(full, toc.matchedMediumPosition);
        flat.confidence = 90; flat.sourceTag = "musicbrainz";

        // Stage 1.5 — Apple Music enrichment (gated, never blocks)
        auto decision = applemusic.shouldEnrich(flat);   // §4.1a
        if (decision.path != Stage15Path::Skip) {
            try {
                auto am = applemusic.enrich(flat, decision); // §5 merge inside
                flat = mergeMbWithAppleMusic(flat, am);      // §5
                flat.sourceTag = "musicbrainz+applemusic";
            } catch (const ApiError& e) {
                // 4.1b: log and degrade silently.
                pipelineLog.append({"applemusic", "fail", e.what()});
            }
        }
        cache.put(discId, flat, scored.toLog());
        return flat;
    }

    // Stage 2 (unchanged)
    if (auto cdtext = cdtext::read(); !cdtext.album.isEmpty()) {
        auto flat = fromCdText(cdtext, toc);
        flat.confidence = 50; flat.sourceTag = "cd-text";
        cache.put(discId, flat, {});
        return flat;
    }

    // Stage 3 (unchanged)
    auto fp = chromaprint::fingerprint(track1Pcm);
    auto acoustid = acoustid::lookup(fp, track1Pcm.durationSec());
    if (acoustid.bestScore() >= 0.5) {
        auto recordingId = acoustid.bestRecordingId();
        auto candidateReleases = mb.releasesOfRecording(recordingId);
        if (!candidateReleases.isEmpty()) {
            auto scored = scoreAndRank(candidateReleases, toc);
            auto winner = scored.first();
            auto full   = mb.fetchRelease(winner.releaseId);
            auto flat   = flatten(full, toc.matchedMediumPosition);
            flat.confidence = 60; flat.sourceTag = "acoustid";
            // Stage 1.5 is intentionally NOT invoked from the AcoustID path.
            // AcoustID's selected release has already failed disc-ID match;
            // adding Apple enrichment on a low-confidence MB pick is more
            // likely to amplify the wrong-release error than to fix it.
            cache.put(discId, flat, scored.toLog());
            return flat;
        }
    }

    // Stage 4 — stub
    auto stub = makeStub(discId, toc);
    stub.confidence = 0; stub.sourceTag = "stub";
    cache.put(discId, stub, {});
    submissionQueue.append(discId, toc);
    return stub;
}
```

`scoreAndRank()` is deterministic. `cache.put(discId, meta, log)` writes
both the flat `AlbumMeta` and the `log` (an array of `{releaseId,
score, components}` rows) so the dev can audit picks. Stage 1.5's
decision is appended to the same log row as `stage15_decision:
{path, reason, tracksTargeted}`.

### 4.5 Merge strategy — MB ↔ Apple Music (UPDATED 2026-05-16 — Apple Music)

When Stage 1.5 returns data, we merge it into the MB-flattened
`AlbumMeta`. The rule is field-by-field: some fields prefer Apple, some
prefer MB, some are Apple-only because MB doesn't model them. There is
no global "Apple wins" or "MB wins."

| Field | Source preferred | Reasoning |
| --- | --- | --- |
| `composerName` / `composerId` | **MB if MB has a Work entity with a `composer` relation**; **Apple otherwise** | MB's structured composer relation (typed, MBID-linked) is the ground truth when present. Apple's `composerName` is a flat string that's gold-standard *content-wise* but breaks `MUSICBRAINZ_COMPOSERID` because no MBID is attached. Prefer MB when MB has the structure; fall through to Apple when MB returned only a joined artist-credit. |
| `workTitle` | **Apple** | Apple's classical curation produces clean canonical titles ("Piano Concerto No. 3 in D Minor, Op. 30"). MB's `work.title` is community-edited and inconsistent (sometimes "Piano Concerto No. 3", sometimes "Concerto for Piano and Orchestra No. 3 in D Minor"). Apple wins on consistency. Keep `workId` from MB if present so `MUSICBRAINZ_WORKID` round-trips. |
| `movementName` | **Apple** | Apple separates the parent work from the movement label ("I. Allegro ma non tanto") cleanly. MB requires the heuristic `split(": ", 1)[1]` on the recording title (§1.1 closing note), which is wrong ~10% of the time when a recording title contains a colon for non-movement reasons. Apple wins. |
| `movementNumber`, `movementCount` | **Apple** | First-class integer fields on Apple's `songs.attributes`. MB has no direct equivalent (you have to count siblings under the parent Work). Apple wins by default. |
| `conductor` / `conductorSort` / `conductorId` | **MB** | MB has a typed `conductor` artist-relation with an MBID. Apple flattens conductor into `artistName` ("Behzod Abduraimov, Royal Concertgebouw Orchestra & Valery Gergiev") with no structural separation. MB is materially better for this field. |
| Orchestra / ensemble (`performers[role=orchestra]`) | **MB** | Same reasoning as conductor: typed MB relation vs Apple's flat artist string. MB wins. |
| Soloist (`performers[role=soloist]`, with instrument attribute) | **MB** | Same. Apple has no instrument-attribute model on songs. |
| `albumTitle` | **MB** | MB is the authoritative record for the *specific pressing* the user is holding (it matched on disc-ID). Apple's `albumName` may differ slightly between Apple's release on the platform and the physical CD. MB wins. |
| `date` / `originalDate` | **MB** | Same reasoning. The physical pressing's date is on MB; Apple's `releaseDate` is for the Apple Music release entry. |
| `barcode`, `catalogNumber`, `label`, `asin` | **MB** | MB-only fields tied to the pressing. Apple doesn't model catalog number or label in album attributes. |
| `genreNames` | **Apple-only** | MB uses tags rather than a structured genre taxonomy, and tags are CC-BY-NC-SA (§1.1) which we don't request. Apple's `genreNames` is a clean array. Map to the FLAC `GENRE` tag (multi-valued). |
| `coverArtUrl` | **MB Cover Art Archive when available (HEAD 200/307); Apple `artwork.url` otherwise** | CAA is the canonical source and is what we already fetch elsewhere. Fall back to Apple's `artwork.url` (replace `{w}x{h}` with `1200x1200`) when CAA returns 404 — particularly useful for newer releases where CAA hasn't been populated. |
| `isrc` (per track) | **MB if present; Apple otherwise; CD Q-data as last resort** | MB carries ISRCs on Recording entities (via `inc=isrcs`). Apple returns ISRC on songs. If both are present they should agree; if they differ, prefer MB (the ISRC is a property of the recording, and MB's recording entity is the ground truth). |
| `MUSICBRAINZ_*` IDs | **MB-only** | Apple has no MBIDs; these are MB-native. Even when Apple wins on a field's *value*, the corresponding `MUSICBRAINZ_*` tag stays MB-derived (or absent if MB didn't have it). |
| `APPLE_MUSIC_ALBUM_ID`, `APPLE_MUSIC_SONG_ID` | **Apple-only** | New Vorbis-comment fields written when Apple enrichment fires. Not yet in §2.4's canonical mapping; they're additive and informational, mirroring how Picard treats `MUSICBRAINZ_*` IDs (custom TXXX-style fields). Useful for the dev's debugging stream and for future "open in Apple Music" deep-links. |

**Implementation sketch:**

```cpp
AlbumMeta mergeMbWithAppleMusic(const AlbumMeta& mb, const AppleMusicResult& am) {
    AlbumMeta out = mb;  // start from MB; overwrite per-field as per table

    // Track-level merges:
    for (int i = 0; i < out.tracks.size(); ++i) {
        auto& t = out.tracks[i];
        auto amTrack = am.findTrack(t);  // by ISRC, else by trackNumber

        // workTitle: Apple wins if present
        if (!amTrack.workName.isEmpty()) t.workTitle = amTrack.workName;

        // movementName / number / count: Apple wins if present
        if (!amTrack.movementName.isEmpty()) t.movementName = amTrack.movementName;
        if (amTrack.movementNumber > 0)     t.movementNumber = amTrack.movementNumber;
        if (amTrack.movementCount  > 0)     t.movementTotal  = amTrack.movementCount;

        // composer: MB wins if MB had a Work-rel composer; Apple otherwise
        if (t.composerName.isEmpty() && !amTrack.composerName.isEmpty()) {
            t.composerName = amTrack.composerName;
            // Note: t.composerId stays empty — Apple gives no MBID.
        }
    }

    // Album-level: only Apple-only fields and the CAA→Apple fallback
    if (out.coverArtUrl.isEmpty() && !am.artworkUrl.isEmpty()) {
        out.coverArtUrl = am.artworkUrl.replace("{w}x{h}", "1200x1200");
    }
    out.genreNames = am.genreNames;   // Apple-only
    out.appleMusicAlbumId = am.albumId;
    return out;
}
```

The merge is pure: input `mb` and `am`, output `out`. No I/O, no mutation
of inputs. Cache-friendly; we can replay it offline if we change the
merge rule and want to re-derive an old cache file's tags without
re-hitting either API.

---

## 5. Licensing implementation matrix (UPDATED 2026-05-16 — Apple Music)

| Component | License of code | License of data returned | Notes / commitments |
| --- | --- | --- | --- |
| **libdiscid** (LGPL-2.1+) — **do NOT use** | LGPL-2.1+ | n/a | `ArVerify::computeDiscIds` already reimplements the spec. Keep. |
| **libmusicbrainz5** (LGPL) — **do NOT use** | LGPL-2.1 | CC0 (the queries) | Current `MusicBrainz.cpp` uses `QNetworkAccessManager` directly. Keep that pattern; QNAM is Qt-LGPL but already dynamically linked. |
| **libcdio / libcdio-paranoia** — **do NOT use** | GPL-3+ | n/a | CD-TEXT is reimplemented in `CdText_macOS.{mm,cpp}` from the MMC-3 Annex J / MMC-5 spec via `IOCDMedia::readTOC(format=0x05)`. See §1.3. May read libcdio for understanding; do not copy expression. |
| **libFLAC** (BSD-like, "modified BSD") | BSD | n/a | Already in `third_party/flac`. Permissive, no issues. |
| **Qt 6** (LGPL-3 / commercial) | LGPL-3 with dynamic linking | n/a | Already in use. Same dynamic-link compliance applies to MetadataResolver. |
| **Chromaprint** (vendored under `third_party/chromaprint/`) | MIT — when built with `-DAUDIO_PROCESSOR_LIB=vDSP` | n/a | The Chromaprint sources are MIT; the LGPL surface only appears if you link FFmpeg or the GPL surface if you link FFTW3. vDSP (Accelerate.framework) avoids both. **macOS build uses vDSP.** Confirm with `otool -L` in CI that no LGPL .dylib gets pulled in. |
| **AcoustID** (HTTP API only, app-wide client key) | n/a | Mixed: fingerprint-DB is CC0-ish (built on submissions), responses include MB MBIDs (CC0) and supplementary fields | App-wide application key registered once at acoustid.org/new-application, embedded in the binary as a const string. Free for distribution. ToS permits embedding. **No per-user account.** |
| **MusicBrainz data (core)** | n/a (API access) | CC0 | No attribution required; cache locally freely. |
| **MusicBrainz data (supplementary: tags, ratings, annotations, search indexes)** | n/a | CC-BY-NC-SA 3.0 | **Don't request these in `inc=`.** Sticking to core entities keeps Concerto entirely on CC0 data. |
| **MusicBrainz UA + rate limits** | n/a | n/a | UA format: `Concerto/<ver> ( <contact-email-or-url> )`. Internal throttle to 1 req/s/IP. |
| **Cover Art Archive (images)** | n/a | Mixed (per-image; permission to display in MB ecosystem) | Safe to fetch and store thumbnails locally. Don't redistribute full-res. |
| **AccurateRip drive-offset DB** | proprietary HTML scraping | n/a | Already bundled as a JSON resource. Factual data; documented practice across rippers. |
| **Discogs API** — **DROPPED** | n/a | n/a | See §1.2. Per-user PAT requirement is incompatible with the no-user-auth constraint. |
| **Apple Music API** (UPDATED 2026-05-16 — Apple Music) | n/a (HTTP only; no code dep) | Apple Developer Program License Agreement — return data may be used to drive features in the calling app; no standalone redistribution | See §1.7. ES256 JWT signed in-process via macOS `Security.framework`; `.p8` private key embedded as a Qt resource. Funded by the dev's active Apple Developer Program membership; **end user pays nothing.** Closed-source distribution accepts the embedded-secret extractability risk. Used only for Stage 1.5 enrichment (gated; §4.1a). |
| **macOS `Security.framework`** (UPDATED 2026-05-16 — Apple Music) | Apple system framework, no separate license | n/a | Used for ES256 JWT signing (`SecItemImport`, `SecKeyCreateSignature` with `kSecKeyAlgorithmECDSASignatureMessageX962SHA256`). Already linked by any Cocoa app. Replaces an OpenSSL/LibTomCrypt dependency that would otherwise carry licensing complexity. |

**App Store guidance.** Unchanged from the previous draft. The
all-permissive-or-LGPL-dynamic-only approach makes a future Mac App
Store path painless. The Apple Music JWT does not change this — Apple's
own framework handles all crypto, and the catalog API is documented as
freely usable from indie apps within ADPLA terms.

---

## 6. Concrete Qt6/C++ implementation plan (UPDATED 2026-05-16 — Apple Music)

### 6.1 New files and existing-file diffs

```
src/
  MetadataResolver.h     [NEW]
  MetadataResolver.cpp   [NEW]
  MetadataCache.h        [NEW]
  MetadataCache.cpp      [NEW]
  MetadataModel.h        [NEW] — POD types: TrackMeta, AlbumMeta, plus QVariantMap conversions
  MetadataModel.cpp      [NEW]
  MetadataScoring.h      [NEW] — pure functions: scoreRelease(), rank()
  MetadataScoring.cpp    [NEW]
  MetadataMerge.h        [NEW; UPDATED 2026-05-16 — Apple Music] — pure mergeMbWithAppleMusic()
  MetadataMerge.cpp      [NEW; UPDATED 2026-05-16 — Apple Music]

  CdText.h               [NEW] — parsePacks(), CdText struct
  CdText.cpp             [NEW] — pure C++ pack decoder (no Apple API)
  CdText_macOS.mm        [NEW] — IOKit bridge: readTOC(format=0x05)

  Fingerprinter.h        [NEW] — wraps Chromaprint, returns base64 fp + duration
  Fingerprinter.cpp      [NEW]
  AcoustId.h             [NEW] — HTTP client; lookup(fp, duration) → recording MBID
  AcoustId.cpp           [NEW]

  AppleMusicJwt.h        [NEW; UPDATED 2026-05-16 — Apple Music] — JwtSigner class (§1.7.2)
  AppleMusicJwt.mm       [NEW; UPDATED 2026-05-16 — Apple Music] — Security framework calls
  AppleMusic.h           [NEW; UPDATED 2026-05-16 — Apple Music] — HTTP client; albumByUpc, songByIsrc
  AppleMusic.cpp         [NEW; UPDATED 2026-05-16 — Apple Music]

  PendingSubmissions.h   [NEW] — append-only JSONL log of stub-tagged discs
  PendingSubmissions.cpp [NEW]

  MusicBrainz.h          [MOD] — add fetchRelease(mbid, inc) for second hop;
                                 add releasesOfRecording(mbid) for AcoustID path
  MusicBrainz.cpp        [MOD] — replace nested-event-loop httpGet with async,
                                 add work-rels JSON parsing
  Ripper.{h,cpp}         [MOD] — add composer/conductor/performer Q_PROPERTYs,
                                 wire MetadataResolver; remove any chooser-UI signals
  RipWorker.{h,cpp}      [MOD] — call MetadataResolver after disc identify,
                                 publish flat AlbumMeta back to Ripper
  FlacEncode.{h,cpp}     [MOD] — accept VorbisComment map for write

third_party/
  chromaprint/           [NEW] — vendored Chromaprint, configured with vDSP backend

resources/
  applemusic.qrc         [NEW; UPDATED 2026-05-16 — Apple Music] — embeds the .p8
  applemusic.p8          [NEW, gitignored; provided by dev at build time]

[DELETED in this rewrite]
  - DiscogsProvider class (was speculative; never went in)
  - Manual-entry QML view
  - Release chooser QML view
```

### 6.2 Header skeletons (signatures only)

```cpp
// MetadataModel.h
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace concerto::metadata {

struct Performer {
    QString name;         // target-credit ?: artist.name
    QString sortName;
    QString role;         // "soloist", "vocalist", "orchestra", "conductor", "arranger"
    QStringList attrs;    // ["piano"] for an instrument; ["soprano"] for a vocal
    QString mbArtistId;
};

struct TrackMeta {
    int position = 0;            // 1-based, within disc
    QString title;               // movement-level title; full
    QString workTitle;           // parent work, e.g. "Piano Concerto no. 3 in D minor, op. 30"
    QString movementName;        // "I. Allegro ma non tanto"
    int movementNumber = 0;      // 0 = unknown
    int movementTotal  = 0;      // 0 = unknown
    qint64 durationMs = 0;
    QString recordingId;         // MB Recording MBID
    QString workId;              // MB Work MBID
    QString composerId;          // MB Artist MBID (composer)
    QString composerName;        // "Sergei Rachmaninov"
    QString composerSort;        // "Rachmaninoff, Sergei Vasilievich"
    QString conductor;
    QString conductorSort;
    QList<Performer> performers;
    QString isrc;                // optional
};

struct AlbumMeta {
    QString releaseId;           // MB Release MBID
    QString releaseGroupId;      // MB Release-Group MBID
    QString title;
    QString artistCredit;        // joined "Composer; Soloist, Orchestra, Conductor"
    QString date;
    QString originalDate;
    QString country;
    QString barcode;
    QString catalogNumber;
    QString label;
    QString asin;
    int discPosition = 1;
    int discTotalCount = 1;
    QString discSubtitle;        // for box sets — the medium's "title"
    QString coverArtUrl;         // 500px CAA thumbnail, optional
    QStringList genreNames;      // (UPDATED 2026-05-16 — Apple Music) Apple-only
    QString appleMusicAlbumId;   // (UPDATED 2026-05-16 — Apple Music) Apple-only
    QList<TrackMeta> tracks;
    int confidence = 0;          // 0..100; for UI banner
    QString sourceTag;           // "musicbrainz" | "musicbrainz+applemusic" | "cd-text" | "acoustid" | "stub"
    // QVariantMap conversion for QML binding:
    QVariantMap toVariantMap() const;
};

} // namespace
```

`TrackMeta` also gains `QString appleMusicSongId` (Apple-only,
optional) for the same reason — round-tripping the Apple link if the
dev wants to deep-link from the rip view in v2.

```cpp
// MetadataResolver.h
#pragma once
#include "MetadataModel.h"

#include <QObject>
#include <QNetworkAccessManager>
#include <memory>

namespace concerto::metadata {

class MetadataCache;

class MetadataResolver : public QObject {
    Q_OBJECT
public:
    explicit MetadataResolver(QObject* parent = nullptr);
    ~MetadataResolver() override;

    // Lives on the RipWorker thread; the caller owns lifetime.
    void setUserAgent(const QString& ua);                 // for MusicBrainz
    void setAcoustIdClientKey(const QString& key);        // baked-in constant, set once
    // UPDATED 2026-05-16 — Apple Music:
    void setAppleMusicJwtConfig(const applemusic::JwtConfig& cfg);
    void setAppleMusicStorefront(const QString& storefront = "us");
    void setCacheDir(const QString& path);                // default: AppDataLocation/metadata-cache

    // Single-shot async resolution. Always emits resolved(); the
    // pipeline never fails — worst case yields a stub AlbumMeta with
    // confidence=0 and sourceTag="stub".
    //
    // track1Pcm is held by RipWorker; we don't own it. If unavailable
    // (resolve happens before any audio data exists) the AcoustID
    // stage is skipped and we go from CD-TEXT directly to stub.
    void resolve(const QString& mbDiscId,
                 int trackCount,
                 const QList<qint64>& trackDurationsMs,
                 const QByteArray* track1Pcm = nullptr,
                 int track1SampleRate = 44100);

signals:
    void resolved(concerto::metadata::AlbumMeta meta);
    // Diagnostic, non-blocking. UI can show a stage banner ("Identifying...
    // via MusicBrainz" / "Enriching via Apple Music..." / "via CD-TEXT"
    // / "via AcoustID" / "no match — stub").
    void stageChanged(QString stageName);

private:
    // Four real providers: MB, AppleMusic (enrichment), CD-TEXT, AcoustID.
    // No chooser, no manual-entry, no Discogs.
    class MusicBrainzProvider;
    class AppleMusicProvider;   // UPDATED 2026-05-16 — Apple Music
    class CdTextProvider;
    class AcoustIdProvider;

    void runChain();

    QNetworkAccessManager m_nam;
    std::unique_ptr<MetadataCache> m_cache;
    std::unique_ptr<applemusic::JwtSigner> m_jwt;   // shared, 1h lifetime
    QString m_ua;
    QString m_acoustIdKey;
    QString m_appleStorefront = "us";

    // state for the current resolve:
    QString m_discId;
    int     m_trackCount = 0;
    QList<qint64> m_durations;
    const QByteArray* m_track1Pcm = nullptr;
    int     m_track1Rate = 44100;
};

} // namespace
```

```cpp
// AppleMusic.h (UPDATED 2026-05-16 — Apple Music)
#pragma once
#include "MetadataModel.h"
#include "AppleMusicJwt.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <optional>

namespace concerto::metadata::applemusic {

enum class Stage15Path { Skip, Barcode, Isrc };

struct EnrichDecision {
    Stage15Path path = Stage15Path::Skip;
    QString     barcode;             // for Barcode path
    QList<int>  trackIndices;        // for Isrc path: which tracks to look up
    QString     reason;              // human-readable for the scoringLog
};

struct AlbumEnrichment {
    QString albumId;
    QString artworkUrl;              // raw template "...{w}x{h}cc.jpg"
    QStringList genreNames;
    QList<TrackMeta> tracks;         // partial — only fields Apple knows about
};

class Provider : public QObject {
    Q_OBJECT
public:
    explicit Provider(QNetworkAccessManager* nam,
                      JwtSigner* jwt,
                      QString storefront,
                      QObject* parent = nullptr);

    // Pure decision; no I/O.
    EnrichDecision shouldEnrich(const AlbumMeta& mb) const;

    // Async; emits enriched(...) or failed(...).
    void enrich(const AlbumMeta& mb, const EnrichDecision& decision);

signals:
    void enriched(concerto::metadata::applemusic::AlbumEnrichment result);
    void failed(int httpStatus, QString message);

private:
    QNetworkAccessManager* m_nam;
    JwtSigner*             m_jwt;
    QString                m_storefront;
};

} // namespace
```

The orchestrator owns one `JwtSigner` for the lifetime of the
`MetadataResolver` (and therefore for the app session, because the
resolver is owned by the long-lived `RipWorker`). The signer holds the
loaded `SecKeyRef` so we only pay the PEM-parse cost once. Token
regeneration is in-process — no network, no I/O, just ~200 µs of
ECDSA-P256.

```cpp
// MetadataCache.h (SQLite-backed via QtSql, or just JSON files)
#pragma once
#include "MetadataModel.h"
#include <QString>
#include <optional>

namespace concerto::metadata {
class MetadataCache {
public:
    explicit MetadataCache(const QString& dir);
    std::optional<AlbumMeta> get(const QString& mbDiscId, int maxAgeDays = 30) const;
    void put(const QString& mbDiscId, const AlbumMeta& meta);
    void invalidate(const QString& mbDiscId);
private:
    QString m_dir;
};
}
```

### 6.3 Caching strategy (UPDATED 2026-05-16 — Apple Music)

- One JSON file per disc ID at
  `<AppDataLocation>/metadata-cache/<discid>.json`. SQLite is
  overkill — the cache is a primary key + blob.
- File contains five sections:
  - `albumMeta`: the parsed `AlbumMeta` (the only thing the runtime
    reads on cache hit). Now carries `genreNames` and
    `appleMusicAlbumId` when Stage 1.5 fired.
  - `rawMb`: the raw MB JSON (for re-derivation when we change the
    flattener).
  - `rawAppleMusic`: the raw Apple Music JSON when Stage 1.5 fired.
    Same justification as `rawMb`: lets the dev re-run the merge logic
    against an old cache file after iterating on §4.5 without re-paying
    the API call.
  - `scoringLog`: an array of `{releaseId, score, components: {composerHit:
    +50, conductor: +20, barcode: +30, ...}}` rows for every
    candidate the disambiguation algorithm saw, plus the
    `stage15_decision: {path, reason, tracksTargeted}` row when Apple
    Music was consulted. **This is the dev's debugging surface** —
    wrong-pick investigation reads this, not the user.
  - `pipelineLog`: an array of `{stage: "musicbrainz" | "applemusic" |
    "cd-text" | "acoustid" | "stub", outcome: "hit" | "miss" | "fail" |
    "skip", ...}` rows. Tells the dev which stages ran and what they
    produced.
- Cache lifetime: 30 days. After 30 days, re-fetch and overwrite.
- Force-refresh: delete the JSON file. (No UI button — re-running the
  rip flow on the same disc with the cache removed is the supported
  recipe; this is dev tooling.)
- Disc ID is the cache key, not the release MBID, because the same
  release MBID can serve multiple disc IDs (different pressings).

### 6.3.1 Pending-submissions log

Parallel to the cache, an append-only JSONL file:
`<AppDataLocation>/pending-submissions.jsonl`. Each line:

```jsonc
{
  "timestamp": "2026-05-16T10:23:11Z",
  "discId": "ABCDEFG_xyz123-",
  "toc": { "firstTrack": 1, "lastTrack": 9, "leadout": 234567,
           "offsets": [...] },
  "ripDir": "/Users/x/Music/Concerto/2026-05-16_Unknown_xyz123-",
  "stageFailures": ["musicbrainz", "cd-text", "acoustid"]
}
```

The dev can occasionally export this and submit the new disc-IDs to MB
under their own account. The user never sees the file.

### 6.4 Threading (UPDATED 2026-05-16)

The existing `RipWorker` lives on its own QThread (Qt worker pattern,
as in `src/AudioWorker.cpp`). `MetadataResolver` is instantiated
*inside* the RipWorker (lifetime tied to the worker) and shares the
RipWorker's thread affinity. The pipeline is fully async via QNetworkReply
finished signals; **no nested event loops anywhere**.

Concretely:

- `MetadataResolver` owns its own `QNetworkAccessManager`. QNAM is
  thread-affine; allocate it on the RipWorker thread, never share with
  the main-thread NAM.
- **Delete** the existing `QEventLoop loop; loop.exec();` in
  `MusicBrainz.cpp::httpGet`. Nested event loops in worker threads are a
  known footgun. This refactor is the right moment to fix it.
- Each stage is a private slot. State machine
  (UPDATED 2026-05-16 — Apple Music):

  ```
  resolve() ──► [Stage1: MB disc-ID]   ──► onMbDiscIdReply()
                  │ hit  → fetchRelease(mbid) → onMbReleaseReply()
                  │           │
                  │           ├─► shouldEnrich(flat)
                  │           │     ├─► Skip → done
                  │           │     └─► Barcode/Isrc → [Stage1.5: Apple Music]
                  │           │              ──► onAppleMusicReply()
                  │           │              │ ok    → merge → done
                  │           │              │ fail  → commit bare MB → done
                  │           │              │ 429   → retry once → commit
                  │           │              │ timeout(5s) → commit bare MB
                  │           │              ▼
                  │           │              done
                  │           ▼
                  │           (done with sourceTag="musicbrainz"
                  │            or "musicbrainz+applemusic")
                  │
                  │ miss → [Stage2: CD-TEXT]   ──► onCdTextDone()
                              │ hit  → done
                              │ miss → [Stage3: fingerprint+AcoustID]
                                        ──► onFingerprintDone()
                                        ──► onAcoustIdReply()
                                        ──► onAcoustIdReleasesReply()
                                        ──► done or [Stage4: stub] → done
  ```

  Stage 1.5 reuses the same `m_nam` (`QNetworkAccessManager`) and runs
  on the RipWorker thread. JWT signing is synchronous and fast (~200 µs
  on Apple Silicon); no need for a separate thread. The signer is
  invoked at most once per hour — the cached token is reused for every
  request inside that window.

- `Fingerprinter` runs on the RipWorker thread synchronously (it's a CPU
  job, ~200ms on a 3-minute track 1; not worth threading further). The
  PCM buffer lives in the RipWorker, passed by const-ref.
- `CdText_macOS.mm` runs on the RipWorker thread via `QtConcurrent::run`
  or direct call (IOKit calls are blocking but fast — ~50ms on most
  drives). It emits `cdTextDone(CdText)` when finished.

The MetadataResolver never reaches the GUI thread directly. It emits
`resolved(AlbumMeta)`, the RipWorker forwards via queued connection to
Ripper, Ripper updates Q_PROPERTYs that QML watches. Same pattern that's
already in use for `mbResolved`.

### 6.5 UI surface (UPDATED 2026-05-16)

Two QML states. No chooser. No manual-entry. The user observes; they do
not direct.

1. **Identifying** (already exists). Spinner + dynamic stage banner.
   Banner text driven by `MetadataResolver::stageChanged`
   (UPDATED 2026-05-16 — Apple Music):
   - "Looking up disc on MusicBrainz..."
   - "Enriching with Apple Music..." (Stage 1.5 only; appears in
     ~30–50% of classical rips and not at all on pop)
   - "Reading CD-TEXT..."
   - "Fingerprinting track 1..."
   - "Querying AcoustID..."
2. **Resolved** — show whatever we got:
   - Cover thumbnail (CAA 500px) if `AlbumMeta.coverArtUrl` is set.
   - Album title, composer, performers, year.
   - Per-track: work / movement number / movement name.
   - A small source badge in the corner: `MusicBrainz`,
     `MusicBrainz + Apple Music`, `CD-TEXT`, `AcoustID`, or
     `Unknown Album`. Tooltip on the badge gives the score (when MB or
     AcoustID), Stage 1.5's `stage15_decision` (when Apple was
     consulted, with the path and reason), and the cache-file path
     (always).

That's the entire interactive surface. No "Wrong album?" link, no edit
popover, no chooser, no manual-entry form. The metadata is what it is;
the user re-rips with a deleted cache file if they want a redo, or edits
the FLACs externally.

Audit trail: when the user clicks the source badge, a debug pane (only
visible in dev builds, gated behind a setting) shows the full
`scoringLog` from the cache file. That's the only "explain why it picked
this" surface, and it's read-only.

### 6.6 ID3v2.4 implementation note (when it lands)

The codebase currently writes Vorbis comments only. When ID3v2.4
support is added (likely via TagLib — Apache 2.0, permissive) the
mapping in Section 2.4 is what's needed. TagLib supports all the
classical frames (`TIT1`, `TIT3`, `TCOM`, `TPE3`, `TMCL`, `MVNM`,
`MVIN`, `TXXX`, `UFID`). Note: TagLib 1.x had spotty Movement
frame support; **TagLib ≥ 2.0** is the practical minimum for
`MVNM` / `MVIN`.

---

## 7. Edge cases

### 7.1 CDs not in any database (UPDATED 2026-05-16)

- Modern indie classical, private recordings, label test pressings.
- Disc-ID returns empty, CD-TEXT empty, AcoustID returns nothing →
  Stage 4 stub (§4.2 Stage 4).
- The rip succeeds. The user gets minimal tags
  (`ALBUM="Unknown Album (Disc ID: <id>)"`, sequential track numbers).
- The disc-ID + TOC is appended to
  `<AppDataLocation>/pending-submissions.jsonl` (§6.3.1) so the dev can
  later batch-submit them to MB under their account.
- **No prompt to the user, no "submit to MusicBrainz?" dialog.** That
  was the previous draft's behaviour and contradicts constraint #1.

### 7.2 Hidden / pregap tracks, CD+G, enhanced CD

- Hidden tracks before track 1 (HTOA — Hidden Track One Audio):
  rare on classical CDs. Just ignore for v1; the existing TOC
  reader skips pre-gap.
- CD+G: irrelevant for audio extraction.
- Enhanced (CD-Extra / multi-session): the audio session is read
  the same way; the data session is ignored. Already handled by
  the TOC parser. Disc-ID computation per MB's spec is over the
  audio session only — verify `ArVerify.h::DiscIds::musicBrainzDiscId`
  excludes data tracks (per spec it must).

### 7.3 Sets where the box has different metadata than individual discs

- Box-set wrapper has its own MB release (the box). Individual
  discs may or may not have their own per-disc MB releases.
- Strategy: prefer the box-level release if it includes our disc
  as a medium (track-count match); the per-disc release is usually
  redundant. The `release-group` key links them anyway.

### 7.4 Re-issues with identical TOC, different masters (UPDATED 2026-05-16)

- TOCs identical → MB disc ID identical → both releases land in
  `releases[]`. Use §3.3.1 ranking. Determinism is desirable here: the
  scoring function will pick the same one every time on every machine,
  so two users with the same disc see identical tags. If the picked
  release is wrong (e.g. a remaster that we should have picked over the
  original), the dev adjusts the weights and re-ships; the next rip
  flips.
- The user has no in-app override path. Their override path is "delete
  the cache file, re-rip" (which still produces the same answer until
  weights change) or "edit the FLAC tags externally." We accept this.

### 7.5 User-supplied corrections — write back to MB (UPDATED 2026-05-16)

- Submitting full release data via API requires a registered MB account
  and OAuth. Incompatible with constraint #2; **no in-app submission
  path of any kind.**
- The only contribution surface is the `pending-submissions.jsonl` log
  (§6.3.1), which the dev manually processes on their own machine,
  under their own MB account, between releases. Users contribute
  passively by ripping unmatched discs and (if they want) sharing the
  log file with the dev. Not in the v1 UI.
- The previous draft mentioned a "small edit-on-MB link beside each
  field" — that's still fine to add cosmetically (an `?edit=<mbid>`
  deeplink that opens the user's browser to MB), since opening a browser
  to a URL doesn't count as in-app prompting. Defer to a polish pass.

### 7.6 Stamper / pressing offsets confusing the disc ID

- Some CDs have multiple pressings with frame offsets shifted by
  a sector or two (the AccurateRip "offset scan" already
  handles this for CRC verification). MB disc IDs differ for
  these. If our computed disc ID misses, fall back to **TOC
  search**:
  `https://musicbrainz.org/ws/2/discid/-?fmt=json&toc=1+<lastTrack>+<leadoutSector>+<track1Lba>+...`
  MB's TOC-search endpoint computes a fuzzy match across known
  pressings. The bare `-` as discid signals "I don't know the
  disc ID, here's the raw TOC."

---

## 8. Explicit non-goals (UPDATED 2026-05-16 — Apple Music)

- **Discogs.** PAT required since 2023 → fails the no-per-user-auth
  constraint. Dropped from this plan entirely.
- **Apple Music as a primary disc resolver.** Apple has no disc-ID
  endpoint and never has. We use Apple only as a Stage 1.5 enrichment
  layer over an MB-resolved release (§1.7, §4.1a). When MB returns
  zero, we go to CD-TEXT, **not** to Apple Music search.
- **Apple Music fuzzy text search in the auto pipeline.** §1.7.4(c)
  documents the endpoint for completeness, but it's not wired in v1.
  Text search across the entire Apple Music catalog is too prone to
  matching the right *work* on the wrong *album*; auto-committing that
  produces worse tags than a stub.
- **OpenSSL / LibTomCrypt / third-party crypto.** macOS
  `Security.framework` covers ES256 JWT signing for the Apple Music
  path. Adding a crypto dep would pull license complexity for no gain.
- **Spotify / Last.fm / Tidal / Qobuz / scrapers.** Weak on classical,
  proprietary, or partner-only. Skip.
- **GPL tagging libraries.** No copying from beets, Picard, libcdio,
  EAC source, whipper, morituri, Aaru, redumper, cdparanoia. Reading for
  *understanding* is fine (`feedback_gpl_study_vs_copy.md`); writing
  code that reproduces their expression is not.
- **Building our own classical metadata DB.** Concerto is a client.
  Contributing back to MB (via the pending-submissions log + the dev's
  account) is the right answer; mirroring is not.
- **In-app user interaction during identification.** No chooser, no
  manual entry, no "submit to MB?" dialog, no PAT prompt. The pipeline
  always commits.
- **User-supplied Apple Music key.** The .p8 is the dev's, embedded in
  the binary. We will not ship a "bring your own Apple Developer
  account" mode — that would violate the no-prompt constraint.

Previously listed under non-goals but now **in scope** (this rewrite):

- **AcoustID / Chromaprint** — promoted to a tertiary stage. See
  §1.3a. Used only when MB disc-ID and CD-TEXT have both failed, so the
  marginal cost (~200ms fingerprint + 1 HTTP round-trip) is paid only on
  the ~5–10% of rips that need it.
- **CD-TEXT reader** — promoted to a secondary stage. See §1.3.
  Reimplemented clean-room from the MMC-3 / MMC-5 spec, no libcdio
  dependency.
- **Apple Music as Stage 1.5 enrichment** (UPDATED 2026-05-16 — Apple
  Music). Re-added after the previous DROPPED status. See §1.7. Fires
  on a gated subset of MB results (§4.1a); merges per §4.5; degrades
  silently on failure (§4.1b).

---

## 9. Implementation milestones (UPDATED 2026-05-16 — Apple Music)

1. **M1 — Schema + extractor.** Define `MetadataModel.h`. Extend
   `MusicBrainz.cpp` with a `fetchRelease(mbid)` that does the second
   hop and parses work-rels. No UI changes; verify via
   `mbquery_main.cpp` against the Rachmaninoff release ID
   (`853b6a62-...`) in this doc.
2. **M2 — Cache + scoring log.** `MetadataCache` JSON files; the
   `scoringLog` and `pipelineLog` fields land here. Force-refresh by
   file deletion.
3. **M3 — Tags.** Extend `FlacTags`/`FlacEncode` to accept the full
   Vorbis comment map. Verify the canonical Rachmaninoff output
   (Section 2.4.1) round-trips through encode → decode.
4. **M4 — MB-only orchestrator.** `MetadataResolver` with **only** the
   MusicBrainz stage wired (no CD-TEXT, no AcoustID, no Apple Music
   yet). Stub other stages with "skip" log rows. Wire into `RipWorker`.
   Expose new Q_PROPERTYs on `Ripper`. Replace nested QEventLoop with
   async. Implement the scoring function (§3.3.1) and verify on real
   multi-candidate disc-IDs.
5. **M4.5 — Apple Music enrichment** (UPDATED 2026-05-16 — Apple
   Music). Add `AppleMusicJwt.{h,mm}`, `AppleMusic.{h,cpp}`,
   `MetadataMerge.{h,cpp}`, and the `applemusic.qrc` resource file. Dev
   drops the `.p8` into `src/keys/` locally (gitignored). Implement the
   JWT signer against `Security.framework`; verify a hand-rolled JWT
   round-trips by decoding it with `jwt.io` once before shipping.
   Implement the gate (§4.1a), the two endpoint calls (§1.7.4 a + b),
   the merge (§4.5), and the silent failure path (§4.1b). Verify on the
   Rachmaninoff disc whose UPC is `190296872785`: the album call should
   fill `workName` / `movementName` / `movementNumber` /
   `movementCount` on every track. Network-mock test for 401/429/5xx
   paths to confirm degradation is silent.
6. **M5 — UI banner + source badge.** The diagnostic banner (states 1
   + 2 from §6.5), now including the "Enriching with Apple Music..."
   stage and the `MusicBrainz + Apple Music` badge variant. No chooser,
   no manual-entry; if MB returns 0 candidates we emit a stub-flavored
   AlbumMeta and the badge reads "Unknown Album."
7. **M6 — CD-TEXT.** Vendor MMC pack parser in
   `src/CdText.{h,cpp}`. Add `CdText_macOS.mm` for the IOKit bridge.
   Test on a known CD-TEXT-bearing disc (any non-classical pop CD from
   ~1998 onward; classical CD-TEXT is too rare to be a useful test
   bed). Plug into MetadataResolver Stage 2.
8. **M7 — Chromaprint + AcoustID.** Vendor Chromaprint under
   `third_party/chromaprint/` with `-DAUDIO_PROCESSOR_LIB=vDSP`. Add
   `Fingerprinter.{h,cpp}` and `AcoustId.{h,cpp}`. Register the app at
   acoustid.org/new-application, embed the client key as a const
   string. Plug into MetadataResolver Stage 3. Verify CI build has no
   LGPL surface via `otool -L`.
9. **M8 — Stub + pending-submissions log.**
   `PendingSubmissions.{h,cpp}` writes the JSONL append log. Stage 4
   is the final piece — once it's in, the pipeline is end-to-end.

The dev's reference test: rip a known well-curated Rachmaninoff
classical CD with conductor + soloist + orchestra and verify the output
tags hit every row in Section 2.4.1 by the end of M4. Add the
"composerless-MB classical" reference test at M4.5 — find a classical
disc whose MB record has weak work-rels (the wider classical catalog is
full of these), confirm Stage 1.5 fires and produces correct
`WORK`/`MOVEMENTNAME`/`MOVEMENT`/`MOVEMENTTOTAL` tags. End-to-end
behaviour (including the rare CD-TEXT-only path and the rarer AcoustID
path) is verifiable from M7 onward by deleting the cache file and
forcing stage failures with a network mock.

---

## Sources cited (UPDATED 2026-05-16 — Apple Music)

MusicBrainz:
- MusicBrainz API — `https://musicbrainz.org/doc/MusicBrainz_API`
- MusicBrainz API / Examples — `https://musicbrainz.org/doc/MusicBrainz_API/Examples`
- MusicBrainz API / Rate Limiting — `https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting`
- About / Data License — `https://musicbrainz.org/doc/About/Data_License`
- MusicBrainz Database (core vs supplementary) — `https://musicbrainz.org/doc/MusicBrainz_Database`
- MusicBrainz Database Statistics — `https://musicbrainz.org/statistics`
- Disc ID — `https://musicbrainz.org/doc/Disc_ID`
- Disc ID Calculation — `https://musicbrainz.org/doc/Disc_ID_Calculation`
- libdiscid (LGPL-2.1+, NOT linked) — `https://musicbrainz.org/doc/libdiscid`
- How to Use Works — `https://musicbrainz.org/doc/How_to_Use_Works`
- Style / Classical / Recording Artist — `https://musicbrainz.org/doc/Style/Classical/Recording_Artist`
- Performance relationship type — `https://musicbrainz.org/relationship/a3005666-a872-32c3-ad06-98af558e99b0`
- Recording-Work relationship types — `https://musicbrainz.org/relationships/recording-work`
- Cover Art Archive API — `https://musicbrainz.org/doc/Cover_Art_Archive/API`
- Live release JSON used as a verified example — `https://musicbrainz.org/ws/2/release/853b6a62-116a-4a11-bc22-533b7cd331e7?fmt=json&inc=...`

Picard / beets style references (read, not copied):
- Picard tag mapping appendix (canonical mapping table) — `https://picard-docs.musicbrainz.org/en/latest/appendices/tag_mapping.html`
- Picard Classical Music Tags variable doc — `https://picard-docs.musicbrainz.org/v2.4/en/variables/variables_classical.html`
- Picard Classical Extras plugin readme — `https://github.com/metabrainz/picard-plugins/blob/2.0/plugins/classical_extras/Readme.md`
- beets ParentWork plugin — `https://beets.readthedocs.io/en/latest/plugins/parentwork.html`

CD-TEXT / MMC / IOKit:
- CD-Text — Wikipedia — `https://en.wikipedia.org/wiki/CD-Text`
- libcdio CD-Text format reference doc (read for understanding only) — `https://www.gnu.org/software/libcdio/cd-text-format.html`
- T10 MMC-5 (working drafts at INCITS) — `https://www.t10.org/drafts.htm` (the published standard is INCITS 430-2007)
- MMC-2 / MMC-3 reference PDFs at 13thmonkey.org for the READ TOC/PMA/ATIP command — `http://www.13thmonkey.org/documentation/SCSI/`
- OSDev Optical Drive page (READ TOC/PMA/ATIP format byte reference) — `https://wiki.osdev.org/Optical_Drive`
- Apple opensource: `IOCDStorageFamily` (`IOCDTypes.h`, `IOCDMedia.h`, `IOCDMediaBSDClient.h`) under APSL 2.0 — `https://opensource.apple.com/source/IOCDStorageFamily/`
- Apple Developer doc: IOCDMedia `readTOC` method — `https://developer.apple.com/documentation/kernel/iocdmedia/1811431-readtoc?language=objc`

AcoustID / Chromaprint:
- AcoustID Web Service — `https://acoustid.org/webservice`
- AcoustID FAQ (key types) — `https://acoustid.org/faq`
- AcoustID + MusicBrainz docs — `https://musicbrainz.org/doc/AcoustID`
- AcoustID new-application registration — `https://acoustid.org/new-application`
- Chromaprint repository — `https://github.com/acoustid/chromaprint`
- Chromaprint LICENSE.md (MIT + LGPL-when-FFmpeg-linked clarification) — `https://github.com/acoustid/chromaprint/blob/master/LICENSE.md`

Apple Music API and crypto (UPDATED 2026-05-16 — Apple Music):
- Apple Music API overview — `https://developer.apple.com/documentation/applemusicapi/`
- Generating Developer Tokens (JWT structure, ES256, 6-month max) — `https://developer.apple.com/documentation/applemusicapi/generating-developer-tokens`
- Find Multiple Catalog Albums by UPC — `https://developer.apple.com/documentation/applemusicapi/get_multiple_catalog_albums_by_upc`
- Find Multiple Catalog Songs by ISRC — `https://developer.apple.com/documentation/applemusicapi/get_multiple_catalog_songs_by_isrc`
- Album resource (attributes) — `https://developer.apple.com/documentation/applemusicapi/albums`
- Song attributes (composerName, workName, movementName, movementNumber, movementCount) — `https://developer.apple.com/documentation/applemusicapi/songs/attributes`
- `SecKeyCreateSignature` — `https://developer.apple.com/documentation/security/1643916-seckeycreatesignature`
- `kSecKeyAlgorithmECDSASignatureMessageX962SHA256` — `https://developer.apple.com/documentation/security/kseckeyalgorithmecdsasignaturemessagex962sha256`
- `SecItemImport` — `https://developer.apple.com/documentation/security/1394136-secitemimport`
- RFC 7515 (JWS) — `https://datatracker.ietf.org/doc/html/rfc7515`
- RFC 7518 §3.4 (ES256 = ECDSA P-256 + SHA-256, raw R||S signature) — `https://datatracker.ietf.org/doc/html/rfc7518#section-3.4`
- Apple Developer Program License Agreement (data usage terms for catalog responses) — `https://developer.apple.com/support/terms/apple-developer-program-license-agreement/`

Already-in-tree, not metadata sources:
- gnudb home — `https://gnudb.org/`
- FreeDB on MusicBrainz — `https://musicbrainz.org/doc/FreeDB`
- CUETools DB — `http://cue.tools/wiki/CTDB_EAC_Plugin`
- AccurateRip — `https://www.accuraterip.com/`

Dropped from the pipeline (kept for the audit trail of *why* they're not here):
- Discogs developers (PAT required since 2023) — `https://www.discogs.com/developers`
