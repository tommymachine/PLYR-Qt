# Concerto — Automated Metadata Pipeline (Distilled)

The single, deterministic, fully-automated metadata pipeline for ripping
classical CDs. No user interaction. No paid APIs **at the end user**. No
per-user accounts. Permissive-license clean.

For the full reasoning, source citations, JSON shapes, and tag mapping
tables, see `METADATA_PLAN.md`. This is the "if I read one doc" version.

Sections substantially revised under the 2026-05-16 Apple Music
addition are marked **(UPDATED 2026-05-16 — Apple Music)**.

---

## Hard constraints

1. **Zero user interaction during identification.** No chooser, no auth
   prompts, no manual-entry form. The pipeline always commits.
2. **Free for the end user.** No paid APIs at the user end, no per-user
   developer accounts. Auth tokens needed for app-wide endpoints are
   embedded in the binary; the developer funds any developer-side cost
   (e.g. Apple Developer Program for the Apple Music JWT).
   Closed-source distribution accepts the embedded-secret risk.
3. **Permissive license.** No GPL/LGPL link surface. CD-TEXT is read
   via the MMC-5 / Red Book Annex J spec through Apple's IOKit
   `IOCDMedia::readTOC(format=0x05)`, not libcdio. Apple Music JWT
   signing uses macOS `Security.framework`, not OpenSSL.

## Four sources, five stages (UPDATED 2026-05-16 — Apple Music)

The four sources used:

| Source       | Auth                                             | License of data        |
| ------------ | ------------------------------------------------ | ---------------------- |
| MusicBrainz  | UA string only; 1 req/s/IP                       | CC0 (core)             |
| Apple Music (enrichment) | ES256 JWT signed in-process from embedded .p8 | Apple Developer Program License Agreement |
| CD-TEXT      | n/a (on-disc)                                    | n/a                    |
| AcoustID     | App-wide application key, embedded in binary    | Mixed (responses are MB MBIDs + supplementary) |

The five stages (each commits or falls through; no failure exits the
pipeline; Stage 1.5 enriches Stage 1 — it never originates a release):

```
[Insert disc]
  │
  ├─ Compute MB disc-ID (clean-room SHA-1 algorithm, already in ArVerify)
  │
  ├─ Stage 1: MusicBrainz disc-ID lookup
  │  GET /ws/2/discid/<id>?inc=artist-credits+recordings+release-groups
  │  ├─ N>=1 candidates → score (§ scoring below) → top-1 → Stage 1b
  │  └─ N=0 / 404         → Stage 2
  │
  ├─ Stage 1b: Release second-hop (composer/conductor/performer)
  │  GET /ws/2/release/<mbid>?inc=artist-credits+recordings+work-rels+
  │      work-level-rels+artist-rels+recording-level-rels+release-groups+
  │      labels+isrcs
  │  → flatten → enrichment gate → Stage 1.5 (maybe) → DONE
  │
  ├─ Stage 1.5: Apple Music enrichment (CONDITIONAL)
  │  Gate: barcode AND (≥30% tracks lack work-rels OR any track lacks composer)
  │        ─► album-by-UPC call
  │     OR: any composer-less track has a readable ISRC
  │        ─► per-track ISRC call(s)
  │     OR: neither ─► skip; commit MB result
  │  ├─ enrichment ok → merge (workName/movement*/genres from Apple;
  │  │                         conductor/orchestra/MBIDs from MB) →
  │  │                   tag → DONE (confidence=High,
  │  │                   source="musicbrainz+applemusic")
  │  └─ 4xx/5xx/timeout → log; commit bare MB result silently → DONE
  │                       (confidence=High, source="musicbrainz")
  │
  ├─ Stage 2: CD-TEXT (on-disc MMC)
  │  IOCDMedia::readTOC(buffer, 0x05, ...) → parsePacks()
  │  ├─ pack data present → best-effort fill (album, titles, performer,
  │  │                       composer if 0x83 pack present) → tag → DONE
  │  │                       (confidence=Medium, source="cd-text")
  │  └─ empty / no packs   → Stage 3
  │
  ├─ Stage 3: Chromaprint + AcoustID
  │  fingerprint(track1_PCM) → POST /v2/lookup
  │  ├─ result score >= 0.5 → walk to releases → score → second-hop →
  │  │   DONE (confidence=Medium, source="acoustid")
  │  └─ nothing / low score  → Stage 4
  │
  └─ Stage 4: Stub
     ALBUM="Unknown Album (Disc ID: <id>)", ARTIST="Unknown",
     TRACKNUMBER=N → tag → append (discId, toc) to
     pending-submissions.jsonl → DONE (confidence=None, source="stub")
```

The cache stores every stage's outcome and every candidate's scoring
breakdown — that's the dev's debugging surface.

---

## Scoring function (Stage 1 + AcoustID-walks-to-release)

Pure function over a MusicBrainz release JSON. Higher is better.
Deterministic — same input yields same score on every machine.

```python
def score_release(release, our_toc):
    s = 0
    # Classical-completeness (proxied by artist-credit shape at disc-ID
    # stage; full work-rels arrive on the second-hop fetch)
    if any(r.has_composer_relation()      for r in release.recordings): s += 50
    if any(r.has_conductor_or_orchestra() for r in release.recordings): s += 20
    # Curation proxies
    if release.barcode:                                      s += 30
    if release.country in {"US","GB","DE","JP","FR"}:        s += 15
    s += 10 * sum(1 for ac in release.artist_credit if ac.artist_id)
    if release.release_group.primary_type == "Album":        s += 5
    # Duration similarity (ambiguous-disc-ID defence)
    dev = sum(abs(our_toc.track_len_sec(i) - release.track_len_sec(i))
              for i in range(release.track_count))
    s += max(0, 30 - dev)
    # Penalties
    if release.status in {"Bootleg","Pseudo-Release"}:       s -= 20
    if not release.has_cover_art():                          s -= 10
    return s

def pick(candidates, our_toc):
    return sorted(candidates, key=lambda c: (
        -score_release(c, our_toc),                            # desc score
        c.release_group.first_release_date or "9999-99-99",    # earliest wins
        c.id,                                                  # lex MBID deterministic tiebreak
    ))[0]
```

The "lowest MBID lex order" final tiebreak guarantees identical picks
across machines. If a wrong pick becomes a pattern, the dev adjusts
weights; the next run flips.

Honest accuracy estimate (see METADATA_PLAN.md §3.3.3 for reasoning):

- ~55–70% single unambiguous MB match
- ~15–20% multi-match with ~80% correct picks
- ~5–10% rescue by CD-TEXT (very degraded — title-only)
- ~5–10% rescue by AcoustID (~70% correct release choice)
- ~2–5% stubs

Net: ~85% correct composer+performer; ~10% wrong-but-plausible labels;
~3% stubs.

---

## CD-TEXT via MMC (clean-room, no libcdio)

Spec: MMC-5 §6.26.3.7.1 (READ TOC/PMA/ATIP cmd 0x43 format 0101b) + Red
Book / Sony CD-TEXT Annex J for the 18-byte pack layout. macOS path:
`IOCDMedia::readTOC(buffer, 0x05, formatAsTime=0, trackOrSession=0,
&actualByteCount)`. Apple's IOKit wraps the SCSI command for us; we
parse the result.

### The 18-byte pack

```
Byte 0    Pack type (0x80–0x8F)
Byte 1    Track number (0 = disc-level; 1..99 = per-track)
Byte 2    Sequence counter
Byte 3    BNCPI: bits 0..3 char-pos (15=new text), bits 4..6 block
                 number (0..7, one per language), bit 7 double-byte
Bytes 4..15  12 payload bytes (zero-terminated strings; multi-pack runs)
Bytes 16..17 CRC-16 (poly 0x11021, inverted, BE)
```

Pack types:

```
0x80 TITLE          0x86 DISC_ID (binary)     0x8D CLOSED_INFO
0x81 PERFORMER      0x87 GENRE   (binary)     0x8E UPC_EAN / ISRC
0x82 SONGWRITER     0x88 TOC_INFO (binary)    0x8F SIZE_INFO (charset)
0x83 COMPOSER       0x89 TOC_INFO2 (binary)
0x84 ARRANGER
0x85 MESSAGE
```

Charset byte (in 0x8F SIZE_INFO payload byte 0):
`0x00 ASCII / 0x01 ISO-8859-1 / 0x80 MS-JIS / 0x81 Korean / 0x82 Mandarin`.

The parser is a ~40-line state machine: iterate 18-byte packs, CRC-check,
peel SIZE_INFO to learn charset per block, accumulate payloads per
(type, track, block), split on 0x00 to recover strings, decode via the
block's charset + double-byte flag. Mapping: `0x80/track=0 → album`,
`0x80/track=N → trackTitles[N]`, `0x81/track=N → trackPerformers[N]`,
`0x83/track=N → trackComposers[N]`, `0x8E/track=N → trackIsrc[N]`. Full
code skeleton in METADATA_PLAN.md §1.3.4.

The IOKit bridge (macOS only) is ~25 lines of Objective-C++ wrapping
`IOServiceOpen(cdMedia, ...)` + the user-client call that lands on
`readTOC`. Apple's `IOCDTypes.h` is APSL 2.0; we redeclare equivalent
structs in our own header so the source tree stays permissive-only.

---

## AcoustID call shape

The application API key is registered once at acoustid.org/new-application
(free, no review, no commercial restrictions) and embedded in the binary
as `kAcoustIdAppKey`. User keys are NOT involved (those are only required
for fingerprint submission, which we don't do).

```
GET https://api.acoustid.org/v2/lookup
  ?client=<kAcoustIdAppKey>
  &meta=recordings+releases+releasegroups+sources
  &duration=<int seconds>
  &fingerprint=<chromaprint base64>
  &format=json
```

Response:

```jsonc
{
  "status": "ok",
  "results": [
    { "id": "<acoustid uuid>",
      "score": 0.987,                          // fingerprint match confidence
      "recordings": [
        { "id": "<mb-recording-mbid>",
          "duration": 481,
          "title": "...",
          "artists": [...],
          "releasegroups": [
            { "id": "<mb-rg-mbid>", "title": "...",
              "releases": [{ "id": "<mb-release-mbid>" }, ...] } ] } ] } ]
}
```

Pick `results[0]` (highest score). If `score < 0.5` → Stage 4. Otherwise
take the top `recording.id`, query
`https://musicbrainz.org/ws/2/recording/<rec-mbid>?inc=releases` to get
candidate releases, apply the same scoring function from above, second-hop
the winner. Done.

Fingerprinting is Chromaprint built against vDSP (Accelerate.framework)
— MIT-licensed binary, no FFmpeg/LGPL surface. Track 1 PCM is already in
memory from the rip; we feed it directly into Chromaprint's `feed()`.

---

## Apple Music — Stage 1.5 enrichment (UPDATED 2026-05-16 — Apple Music)

Re-added 2026-05-16 after previously being DROPPED. The dev has an
active Apple Developer Program membership ($99/year, paid by the dev,
not the user) and Concerto is closed-source, so the embedded
developer-token model is acceptable. Apple Music has first-class
`composerName`, `workName`, `movementName`, `movementNumber`,
`movementCount` on each Song — exactly the gap MB leaves on
classical-sparse releases. **No disc-ID endpoint**, so it never
originates a release; it enriches the Stage 1 winner.

### Auth — ES256 JWT, no OpenSSL

JWT header `{"alg":"ES256","kid":<keyID>,"typ":"JWT"}`, payload
`{"iss":<teamID>,"iat":<now>,"exp":<now+3600>}`. Lifetime **1 hour**
(Apple max is 6mo; we limit exposure); cached in process; regenerated
lazily when within 5 min of expiry. One signer shared across the
session.

Signing uses macOS `Security.framework`, no external crypto:

1. `:/keys/applemusic.p8` → strip PEM armor → base64-decode → PKCS#8 DER
2. `SecItemImport` with `kSecFormatWrappedPKCS8` → `SecKeyRef` (P-256)
3. Build signing input `<b64u(headerJSON)>.<b64u(payloadJSON)>`
4. `SecKeyCreateSignature` with
   `kSecKeyAlgorithmECDSASignatureMessageX962SHA256` → ASN.1-DER sig
5. DER → JWT raw R||S: parse `30 LL 02 rL R 02 sL S`, strip leading
   `0x00` sign-padding, **left-pad each to exactly 32 bytes for
   P-256**, concatenate → 64 bytes. (Skip step 5 = ~50% server rejects.)
6. Base64url-encode the 64-byte sig, append `.<sig>`

Qt has no JWT primitive (QtNetworkAuth = OAuth only; QSslKey loads EC
keys but doesn't expose raw signing). `Security.framework` is already
linked by every Cocoa app — no link-time cost. Apple docs:
[`SecKeyCreateSignature`](https://developer.apple.com/documentation/security/1643916-seckeycreatesignature),
[`kSecKeyAlgorithmECDSASignatureMessageX962SHA256`](https://developer.apple.com/documentation/security/kseckeyalgorithmecdsasignaturemessagex962sha256).

### Embedding the .p8

```xml
<!-- applemusic.qrc -->
<RCC><qresource prefix="/keys"><file>applemusic.p8</file></qresource></RCC>
```

Load: `QFile(":/keys/applemusic.p8").readAll()`. The .p8 is gitignored;
the dev drops their own copy into `src/keys/` at build time. **Trivially
extractable from the shipped binary** (`strings` reveals the PEM) —
accepted threat model. Mitigation: rotate keyID + ship a new build if
abuse appears. Light obfuscation (XOR-with-derived-key) **not
recommended**: adds complexity, no real durability against a debugger.

### Endpoints (default storefront `us`)

(a) **Album by UPC** — primary Stage 1.5 path:
`GET /v1/catalog/us/albums?filter[upc]=<upc>&include=tracks` →
album + nested songs, each with `composerName`/`workName`/`movementName`/
`movementNumber`/`movementCount` + album `genreNames`.

(b) **Song by ISRC** — per-track secondary, only for composer-less
tracks: `GET /v1/catalog/us/songs?filter[isrc]=<12-char-ISRC>`. Multiple
results: pick the one whose `albumName` is closest to the MB-resolved
title (case-folded Levenshtein < 5), else `data[0]`.

(c) **Fuzzy text search** (`/v1/catalog/us/search?term=...&types=albums`)
— **not in v1**: too prone to wrong-album-right-work auto-commit.

### When Stage 1.5 fires

```
if mb.barcode and (>=30% tracks lack work-rels OR any track lacks composer):
    fire (a) album-by-UPC
elif any composer-less track has a readable ISRC:
    fire (b) per-track ISRC for just those tracks
else:
    skip — MB nailed it
```

Expected fire rate on classical CDs: **~30–50%** (MB's long-tail
classical work-rel coverage is incomplete). On pop, near 0%.

### Merge rules (MB ↔ Apple, only the load-bearing entries)

| Field                                 | Prefer            | Reason                                                                |
| ------------------------------------- | ----------------- | --------------------------------------------------------------------- |
| `composerName`                        | MB if Work-rel present; Apple otherwise | MB's structured composer wins when present; Apple fills the gap       |
| `workName`                            | Apple             | Apple's classical curation > MB's community parsing                   |
| `movementName` / number / count       | Apple             | First-class on Apple; MB needs fragile title-splitting                |
| `conductor` / orchestra / soloist     | MB                | Typed MBID relations on MB; Apple flattens into `artistName`          |
| `albumName` / `date` / pressing data  | MB                | MB is authoritative for the *specific* pressing (matched on disc-ID)  |
| `genreNames`                          | Apple-only        | MB tags are CC-BY-NC-SA (not requested); Apple has a clean array      |
| `coverArtUrl`                         | CAA → Apple fallback | CAA when HEAD 200/307; Apple `artwork.url` w/ `{w}x{h}`→`1200x1200`   |
| `MUSICBRAINZ_*` IDs                   | MB-only           | Apple has no MBIDs                                                    |

Merge is a pure function — replayable offline against `rawMb` +
`rawAppleMusic` in the cache file.

### Failure mode — silent degradation

Any Apple failure (4xx, 5xx, timeout >5s, network error) is logged to
`pipelineLog` and falls through to "commit the bare MB result." Stage
1.5 cannot block a rip; the source badge just reads `MusicBrainz`
instead of `MusicBrainz + Apple Music`. Persistent 401s → rotate the
embedded `.p8`.

---

## What was deleted / re-added (UPDATED 2026-05-16 — Apple Music)

- **Discogs** — PAT required since 2023; per-user; per-token rate limit.
  Stays out.
- **Manual-entry UI** — replaced by Stage 4 stub. Stays out.
- **Release-chooser UI** — replaced by the scoring function. Stays out.
- **Confirmation panel + auto-accept threshold** — replaced by
  always-commit + score logging to the cache. Stays out.
- **"Submit to MB?" deeplink prompt** — replaced by silent append to
  `pending-submissions.jsonl`. Stays out.
- **"User supplies barcode" branch** — there's no UI surface to collect
  one anymore. Stays out.
- **Apple Music** — previously dropped (recurring $99/year). **Re-added
  2026-05-16 as Stage 1.5 enrichment.** Dev funds the developer-side
  membership; closed-source distribution accepts the embedded-key risk.

---

## Stage outcomes the cache records (UPDATED 2026-05-16 — Apple Music)

Per disc-ID, `<AppDataLocation>/metadata-cache/<discid>.json` contains:

- `albumMeta` — the parsed AlbumMeta the runtime reads on cache hit;
  now carries `genreNames` and `appleMusicAlbumId` when Stage 1.5 fired
- `rawMb` — raw MB JSON (for re-flattening if we change the parser)
- `rawAppleMusic` — raw Apple Music JSON when Stage 1.5 fired; lets
  the dev re-run the merge logic without re-paying the API call
- `scoringLog` — array of `{releaseId, totalScore, components: {composer:
  +50, conductor: +20, barcode: +30, country: +15, ...}}` for every
  candidate the scoring function ranked, plus a `stage15_decision:
  {path, reason, tracksTargeted}` row when Apple Music was consulted
- `pipelineLog` — array of `{stage, outcome, durationMs}` rows;
  `stage` is now `musicbrainz | applemusic | cd-text | acoustid |
  stub`; `outcome` is `hit | miss | fail | skip`

Plus a side log:
`<AppDataLocation>/pending-submissions.jsonl` — append-only JSONL of
`{timestamp, discId, toc, ripDir, stageFailures}` rows for stub-tagged
discs that the dev manually submits to MB later under their own account.

That's the entire pipeline.
