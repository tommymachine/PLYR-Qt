# Concerto — LibrarySweeper Design

How Concerto turns "here is my music folder" into "every album is in the
library DB, fully enriched." Companion to `LIBRARY_METADATA_PLAN.md`
(specifically Step 4 of §10) and `METADATA_PLAN.md` (rip-time context).
The Sweeper is the bulk-orchestration layer that sits on top of the
foundation that has already shipped:

- `LibraryDatabase` (SQLite, `src/LibraryDatabase.{h,cpp}`)
- `AudioTagIo` (TagLib-backed multiformat tag reader,
  `src/AudioTagIo.{h,cpp}`)
- `AudioFrameHash` (decoded-PCM SHA-256, `src/AudioFrameHash.{h,cpp}`)
- `FolderIdentifier` (Stages Z/A/B live; C/D stubbed,
  `src/FolderIdentifier.{h,cpp}`)
- `library_cli` test harness (`src/library_cli_main.cpp`)

Status: planning only. No code in this pass. The recommended
implementation order is in §17; the one-paragraph architecture
recommendation is in §16. Read those first if you want the answer.

The Sweeper has **one job**: walk a root, find album folders, hand each
to `FolderIdentifier`, persist results, do it in the background, do it
at the MB rate-cap, survive app restart, and resume cleanly. Everything
else is policy on top of that.

---

## 0. TL;DR — Sweeper in one screen

```
LibrarySweeper (lives on a QThread, owned by main.cpp)
    │
    ├── sweep_queue table (SQLite, in the same library.db)
    │     status ∈ { queued | in_progress | done | failed | skipped | locked }
    │     atomically transitioned via UPDATE…WHERE status=?
    │
    ├── FolderEnumerator (run on the sweeper thread)
    │     recursive walk of the root
    │     groups files into "logical albums" using the rules in §2
    │     enqueues into sweep_queue (idempotent INSERT OR IGNORE)
    │
    ├── MbRateGate (shared service object, source-aware)
    │     strict 1 req/sec/IP for "musicbrainz" source
    │     QTimer-driven leaky-bucket; emits ready() when budget is
    │     available; other sources have their own gates
    │
    └── per-job pipeline (one folder at a time, fully async):
          1. read tags for every file via AudioTagIo                ← thread-local
          2. compute AudioFrameHash for any file the DB doesn't       ← thread-local
             already know (lazy; path+size+mtime probe first)
          3. FolderIdentifier::identify(req) — Stage Z/A/B/C/D       ← async
             (Stage E AcoustID is NOT auto-run, ever)
          4. on identified: LibraryDatabase::upsertAlbumMeta + per-     ← thread-local
             file rows in one transaction
          5. update sweep_queue → done (or failed / unidentified)      ← thread-local
          6. emit folderCompleted / folderFailed
```

Defaults:

- **Triggering**: explicit. The user picks a library root via
  `Settings → Library → Library folder`. First-launch onboarding leads
  them through this. Auto-watching is opt-in (off by default in v1).
  Post-rip never enqueues — the rip path writes the DB row directly
  because it already has the metadata.
- **Folder discovery**: directory-grouped (every directory that holds
  ≥1 audio file is one logical album); a hint-based pre-pass groups
  per-disc subfolders under one parent release-group when name regex
  matches (`Disc 1` / `CD 02` / `Disc.03` / `disk-1`).
- **Rate limiter**: leaky bucket at 1 req/s/IP for MusicBrainz, owned
  by a singleton `RateGate` keyed by source. Cover Art Archive gets
  its own gate (shares MB's IP-based ceiling per MB docs). AcoustID
  (later) and Apple Music v2 each get their own gate. No bursting
  against MB — the published policy is hard 1/s with IP-block teeth
  (§3 citations).
- **Concurrency**: one MB-dispatching worker thread + a small CPU
  pool (`QThreadPool`, 2–4 threads) for `AudioFrameHash`. Folder-level
  work is fully serialized through the worker; file-level hashing
  fans out within a single folder. Throughput math in §4.
- **Persistence**: `sweep_queue` and `sweep_runs` tables in the
  existing `library.db`. Atomic status transitions, generation
  counter for re-rooted libraries, in-flight rows are re-queued at
  startup. Schema in §5.
- **Re-sweep policy**: only on user action ("Re-scan library") or
  pipeline-version bump (`kConcertoPipelineVersion` bumps trigger
  re-fetch on folders whose `releases.pipeline_version` is lower).
  Folder mtime changes trigger a *cheap* re-enumerate (look for new
  files) but not a full re-identify. Failed folders are re-tried
  only at the user's request or on a network-came-back signal.
- **UI**: persistent but unobtrusive sweep status chip in the library
  view's header; full-screen progress on first sweep; per-folder
  badge on the album card. Signal vocabulary in §7.
- **Cancellation**: drain-current-folder semantics for pause and
  cancel. The folder in flight finishes (or rolls back its DB writes
  if it crashed). The user-driven "open this folder now" path
  jumps the queue.
- **Errors**: 5xx + 503 → exponential backoff (1s / 5s / 30s, three
  tries) then back of the queue. Network down → pause MB-dependent
  stages, continue Stage Z folders at full speed. File I/O fail →
  mark file as skipped, continue. Corrupt audio → skip file, don't
  skip folder.
- **Perf tuning (the three things worth doing in v1)**: (a) **path+
  size+mtime probe before hashing** — most files are unchanged
  across re-sweeps and don't need to be re-hashed; (b) **batch DB
  writes per folder** — one `BEGIN…COMMIT` per folder, not per
  track; (c) **defer Cover Art Archive fetches** to a low-priority
  job kind that only runs when no `ResolveFolder` jobs are pending.
- **Out of scope for v1**: Stage E (AcoustID), Apple Music gate,
  tag writeback, cover-art fetch (the *scheduling* lives in the
  sweeper but the *fetch logic* is a separate concern), multi-root
  libraries, network playback sources.

---

## 1. Triggering — when does the sweeper run?

Three triggers, ordered by user-visibility.

### 1.1 First-launch onboarding (the primary case)

Onboarding card: "Welcome to Concerto. Pick your music folder."
`QFileDialog::getExistingDirectory` rooted at
`QStandardPaths::MusicLocation` (= `~/Music/` on macOS). The chosen
path lands in `QSettings` under `library/rootPath`; immediately calls
`Sweep::start(root, force=false)`. The library view swaps to
"Sweeping… 0 of N albums" with progressive enrichment as folders
complete.

One root only in v1. Multi-root (laptop with music on `~/Music`, an
external SSD, and a NAS) is v1.1 — adds a `roots` table and a column
to `sweep_queue`. Not v1.

### 1.2 Explicit "add another folder" or "re-scan"

`Settings → Library` surfaces two controls:

- **Library folder**: changing this kicks a *new generation* (§5.3).
  Old rows aren't deleted; they belong to the previous generation
  and are filtered out of queries. v1 keeps them; a "cleanup
  orphans" action is v1.1.
- **Re-scan library**: re-enqueues everything under the current
  root with `force=true`. Most folders hit Stage Z (the in-file
  marker or DB-as-authority is already there) → seconds-not-hours
  for a fully-enriched library.

### 1.3 Post-rip: rip writes directly, sweeper never touches

When `RipWorker` finishes saving FLACs, **the rip path writes the
library DB row directly**, not via the sweep queue. The data is
already in hand: `MetadataResolver::resolved(album, sourceTag)` fired
at the end of the rip; the FLACs carry the `CONCERTO_*` provenance
markers (per §A.1 of `LIBRARY_METADATA_PLAN.md`).

Hook: in `main.cpp` where `Ripper::discSaved` is observed, also call
`libraryDb.upsertAlbumMeta(album)` and `upsertFile()` for each
encoded FLAC. The sweeper is **not** notified.

If the user later runs Re-scan, Stage Z trust-passes the same folder
without a web call. Belt + suspenders for free.

### 1.4 Auto-watch — opt-in v1.1

`QFileSystemWatcher` on macOS hits `kqueue` file-descriptor limits
fast; FSEvents (the right way) is a non-trivial wrapper. v1 ships
without watch; v1.1 adds a `Settings → Library → Watch for new files`
toggle backed by an FSEvents stream.

### 1.5 Decision

| Trigger | v1 behavior |
| --- | --- |
| First launch | Onboarding card → folder picker → kicks `Sweep::start(root, fullScan=true)`. |
| User adds folder | Re-points the root; new generation; previous-gen rows kept for history. |
| User clicks Re-scan | Re-enqueues every folder under current root with `force=true`; cheap in steady state because of Stage Z. |
| Rip completed | Rip path writes DB row directly. **Sweeper is not notified.** |
| Files appear on disk | Detected only on next user-initiated sweep. v1.1 adds FSEvents-based watch. |

---

## 2. Folder discovery

The sweeper recursively walks the root and decides which directories
are "logical album folders." This is the hairy part of the design
because real-world libraries lie all over the spectrum.

### 2.1 What counts as an album folder

Three observed layouts: (1) one folder per album (the 90% Picard
case), (2) multi-disc with per-disc subfolders
(`Box/CD01/`, `Box/CD02/`, …), (3) flat dump (everything in one
directory, tags carry the structure). All three need to work.

Discovery algorithm:

```
enumerate(root):
  for each directory D under root (recursive, breadth-first):
    if D matches a skip pattern: continue
    audio_files = files in D matching audioExtensions()
    if audio_files.empty(): continue
    if D's name matches a disc-suffix regex:
       record (parent=D.parent, disc=D, files=audio_files)
    else:
       record (parent=D, disc=null, files=audio_files)
```

One queue entry per `(parent, disc)`. `FolderIdentifier::Request`
carries `folderPath = disc or parent` plus the file list.

The `audioExtensions()` set already exists in `library_cli_main.cpp`
(lines 42–55: `flac`, `mp3`, `m4a`, `aac`, `ogg`, `opus`, `wav`,
`aiff`, `aif`); migrate it to a shared header — `AudioTagIo` is the
natural home.

### 2.2 Disc-suffix regex

The wild has many spellings. A union regex that covers the common
ones:

```
^(?:disc|cd|disk)\s*[-_.]?\s*\d{1,2}(?:\s*[-_.\s].*)?$
```

Matches (case-insensitive, full-string against the directory's name):

- `Disc 1`, `Disc 01`, `Disc.01`, `disc-1`, `disc_1`
- `CD 1`, `CD01`, `CD-01`, `cd_1`
- `Disk 1`, `disk 02`
- `Disc 1 - Bonus material` (the trailing suffix is allowed)

Does **not** match:

- `DiscoAlbum/` (no separator between `Disc` and `o`)
- `1of3/`, `Vol 1/` (intentionally — these may or may not be a disc)

Multi-disc detection has a fallback: if a folder has files all sharing
the same `DISCNUMBER` tag value AND that value is not `1`, the folder
is a disc whose subfolder name didn't match the regex. The sweeper
notes this via a `disc_position` column on `sweep_queue` and joins it
to the parent folder by walking up one path level.

The "all discs concatenated in one folder" case (rare): handled by
`FolderIdentifier` — if the same album-MBID quorum lands on N tracks
across multiple `DISCNUMBER` values, `FolderIdentifier` records all
mediums. The sweeper passes the whole folder; identification is what
notices.

### 2.3 Enumerator sketch

`QDirIterator` with `QDirIterator::Subdirectories` builds a flat
hash of `dir → audio files`, then emits one `SweepJob` per directory
(applying the disc-suffix regex on each). Flat-hash form avoids
unbounded recursion (some users have ~20-deep paths) and the skip
pattern can abort whole subtrees cheaply.

### 2.4 Skip patterns

Hardcoded set (matched against any path segment):

- Dot-prefixed: `.git`, `.DS_Store`, `.Trashes`, `.fseventsd`, `.Spotlight-V100`
- Concerto's own dirs: `rip_in_progress`, `metadata-cache`, `library`,
  `tag-backups`, `pending-submissions.jsonl` (in case the user
  somehow puts the AppSupport dir under their music root)
- iTunes/Music.app artifacts: `iTunes Library Files`, `Album Artwork`,
  `Genius Information.gnp`, `Previous iTunes Libraries`
- Common archive/temp suffixes for directories: `.app`, `.bundle`
  (treat as opaque)

The skip patterns are loaded from a hardcoded list in
`src/SystemPaths.{h,cpp}` (the natural home — it already centralizes
path conventions). A `Settings → Library → Exclude patterns` text
field can append user patterns; v1.1.

### 2.5 What's *not* an album folder

- Directories with `cover.jpg` / `folder.png` only — skip.
- Directories with only `.cue` and `.log` files (EAC artifact-only
  folders before the FLAC lives there) — skip; rare in practice.
- Bundle / package directories (`.app`, `.bundle`) — skip entirely.

These are handled by the "≥1 audio file" gate. The enumerator
silently produces no job for them.

---

## 3. Rate limiting

The single biggest design constraint on the Sweeper. The MB published
policy (verified at <https://musicbrainz.org/doc/MusicBrainz_API> and
<https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting>):

> **All users of the API must ensure that each of their client
> applications never make more than ONE call per second.** If you
> impact the server by making more than one call per second, your IP
> address may be blocked preventing all further access to MusicBrainz.

No averaging window. No published burst tolerance. The penalty is
IP-blocking. **The Sweeper must be conservative.**

### 3.1 The pattern: leaky bucket, 1 req/s, no burst

A `QTimer`-driven leaky bucket. Tokens regenerate at the configured
rate; consumers `acquireBlocking()` on the sweeper thread (off the
GUI thread). No bursting against MB — the brief asked about "5 reqs
in 5s" as a sanity-check, but **the published policy is unambiguous
hard 1/s with IP-block teeth and no documented burst tolerance**.
Penalty is asymmetric: we'd lose all MB access for an unknown duration
with no support contact. Always pick the conservative number for MB.

### 3.2 Source-aware, not single-global

Make the gate per-source from day one, even though only MB matters in
v1:

```
RateGateRegistry
  ├─ "musicbrainz"      → RateGate(1.0 token/s,  backlog=∞)
  ├─ "coverartarchive"  → RateGate(1.0 token/s,  backlog=∞)  // shares MB IP ceiling
  ├─ "acoustid"         → RateGate(3.0 token/s,  backlog=128)  // v1.5
  └─ "applemusic"       → RateGate(100 token/s,  backlog=512)  // v2
```

Adding a source = registering a new gate. The sweeper resolves by
source string before each call.

### 3.3 Where the gate lives

**Shared service object, not inside `musicbrainz::Client`.** The
client is reused by the rip flow (which does ~2 calls per rip); a
shared external gate lets sweep and rip cooperate naturally. The
registry is owned by `main.cpp` and passed by pointer to
`LibrarySweeper`, `MetadataResolver`, and (later) `CoverArtFetcher` /
`AppleMusicGate`. Existing `MetadataResolver` MB calls switch to go
through the gate as a same-PR change — `m_mb.fetchDiscId(id)` becomes
`gate.acquireBlocking("musicbrainz"); m_mb.fetchDiscId(id);`.

### 3.4 503 / 429 handling

- 503 (rate exceeded — implies *someone else on our IP* is consuming
  budget): requeue at back, wait 30 s before next MB call.
- 429 (HTTP-level rate limit): same as 503.
- ≥3 consecutive: pause MB-dependent jobs for 60 s, emit
  `rateLimited(60)` for the UI ("MusicBrainz throttling us; resuming
  in 60s"), then resume.

### 3.5 Burst budget for the sweep itself

Cold-cache MB-call counts per folder:

| Stage | MB calls |
| --- | --- |
| Z (trust marker / DB-as-authority) | 0 |
| A (MBID quorum) | 1 (second-hop release) |
| B (disc-ID, cache hit) | 0 |
| B (disc-ID, cache miss) | 2 (disc-ID + release) |
| C (search) | 1–2 |
| D (TOC fingerprint) | 1–2 |

Mean ≈ 1 call per uncached folder. 700-album library = ~12 minutes
of MB time (concrete numbers in §4.4).

### 3.6 Lifting the cap?

Per MB docs: no authenticated tier raises the per-IP limit. The
"meaningful User-Agent" requirement is about *accountability*, not
quota. Theoretically: self-host a mirror (out of scope) or commercial
license (overkill for one user). The Sweeper is permanently
constrained by 1/s, and every downstream design decision optimizes
for "60 MB calls per minute, that's all we ever get."

---

## 4. Concurrency

What's CPU vs I/O vs network in the per-folder work:

| Operation | Per file | Cost class |
| --- | --- | --- |
| Tag read (`AudioTagIo::read`) | ~5 ms | I/O + light CPU |
| Audio-frame hash (`AudioFrameHash::compute`) | ~150–500 ms (30s decode + SHA-256) | CPU-bound |
| DB upsert | ~0.5 ms per row | I/O (SQLite WAL) |
| Stage A MB fetch | ~300–800 ms | Network |
| Stage C MB search | ~300–800 ms | Network |
| FolderIdentifier orchestration | negligible | CPU |

The MB rate cap means **at most 1 folder per second can do
network-bound work**. Hashing is parallelizable but bounded by CPU
cores. Tag reads + DB writes are tiny.

### 4.1 The recommendation: 1 worker thread + small CPU pool

```
LibrarySweeper thread (one)
  drives the per-folder pipeline; owns the SQLite write handle;
  owns the QNAM that issues MB / CAA requests; processes one folder
  at a time; talks to RateGate via Qt::QueuedConnection::ready().

QThreadPool ("hash pool")
  size = max(2, QThread::idealThreadCount() / 2), cap at 4.
  consumed only by AudioFrameHash::compute. Fanout-within-folder:
  the sweeper submits all unhashed files of a folder, then awaits
  via QFuture::waitForFinished() (this *does* spin nested events,
  but only on the sweeper thread — it's safe; the GUI is on
  another thread).
```

Why not multiple sweeper threads:

- The MB rate gate already serializes the most expensive step. Adding
  more workers wouldn't go faster; they'd all queue on the gate.
- SQLite writes serialize naturally (WAL mode permits concurrent
  reads + one writer); a single writer thread avoids `SQLITE_BUSY`
  retry overhead.
- The mental model is simpler. One thread = one place to reason about
  invariants. The persisted queue (§5) is the parallelism boundary,
  not in-memory thread pools.

Why a small CPU pool for hashing:

- First-sweep hashing is the only "many seconds per folder" CPU work.
  Without parallelism, hashing a 13-file folder is ~3–6 seconds.
  With 4-way parallelism, ~1 second. That's worth it for first-sweep
  wall-clock (§4.4).
- The pool is bounded so the rest of the app stays responsive on
  laptops; the dev's machine is presumably reasonably-spec'd but
  Concerto's audio path is real-time and CPU-sensitive.

### 4.2 The "open this folder now" path

User-driven open-folder (the lazy-eager hybrid from
`LIBRARY_METADATA_PLAN.md` §7.2) jumps the queue via
`identifyNow(folderPath)`:

- If `queued` or absent → insert at `PRIORITY_USER` (= 5, ahead of
  the default 100).
- If `in_progress` → no-op; the caller subscribes to the next
  `folderCompleted` for that path.
- If `done` → re-read from DB and emit `folderCompleted` immediately.

Pull loop is `WHERE status = 'queued' ORDER BY priority ASC, queued_at
ASC LIMIT 1`, so the user row is next. Click-to-result latency ≤1 s.

### 4.3 No fan-out for tag reads

Tag reads are ~5 ms. A 13-file folder = ~65 ms serialized; fan-out
saves ~50 ms — not worth the complexity. Serialize on the sweeper
thread.

### 4.4 Quantified wall-clock for a 10k-track library

10k tracks / ~700 albums / 4-core SSD machine / cold MB cache:

| Phase | Per-folder cost | Bottleneck | Throughput |
| --- | --- | --- | --- |
| Stage A (~50% of first sweep) | tag-read 75 ms + hash 1100 ms + MB-gate ≥1000 ms + DB 15 ms | gate-bound | ~1100 ms/folder |
| Stage B/C/D (~50%) | similar | gate-bound | ~1100 ms/folder |
| Stage Z re-sweep (steady state) | tag-read + probe-skip + DB | CPU-light | ~50 ms/folder |

Totals: **first sweep ~13 minutes**, **re-sweep ~35 seconds**.
Matches `LIBRARY_METADATA_PLAN.md` §7.4. 50k tracks: ~55 min first
sweep, ~3 min re-sweep. 1k tracks: ~70 s first sweep.

### 4.5 Slow-machine sensitivity

Hashing can be 3× slower on old hardware, but per-folder time stays
gate-dominated: hashing finishes during the MB gate's 1-second wait.
Wall-clock is robustly bounded by MB rate cap; no need to optimize
CPU paths aggressively for v1.

---

## 5. Persistence and resumption

The sweeper persists its queue and run history into the same
`library.db`. Atomic transitions ensure that an app crash never leaves
a folder stuck in `in_progress`.

### 5.1 Schema additions

```sql
-- One row per folder the sweeper plans to (or has) processed. Same
-- DB as the existing `folders` table, but separate concern: `folders`
-- is the post-identification truth table; `sweep_queue` is the
-- mid-flight bookkeeping.
CREATE TABLE sweep_queue (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  folder_path     TEXT NOT NULL,             -- absolute path
  parent_release  TEXT,                      -- parent path if disc subfolder
  is_disc_sub     INTEGER NOT NULL DEFAULT 0,
  generation      INTEGER NOT NULL,          -- sweep generation, §5.3
  priority        INTEGER NOT NULL DEFAULT 100,  -- lower = sooner
  status          TEXT NOT NULL,             -- queued | in_progress | done | failed | skipped | locked
  attempts        INTEGER NOT NULL DEFAULT 0,
  last_error      TEXT,
  queued_at       INTEGER NOT NULL,
  started_at      INTEGER,                   -- null until status=in_progress
  finished_at     INTEGER,                   -- null until terminal status
  UNIQUE (folder_path, generation)
);
CREATE INDEX idx_sweep_queue_status_priority
  ON sweep_queue(status, priority, queued_at);
CREATE INDEX idx_sweep_queue_generation ON sweep_queue(generation);

-- One row per "Sweep::start" invocation. Summary stats + the root.
CREATE TABLE sweep_runs (
  generation        INTEGER PRIMARY KEY,
  root_path         TEXT NOT NULL,
  started_at        INTEGER NOT NULL,
  finished_at       INTEGER,                 -- null while in flight
  folders_total     INTEGER NOT NULL DEFAULT 0,
  folders_done      INTEGER NOT NULL DEFAULT 0,
  folders_failed    INTEGER NOT NULL DEFAULT 0,
  folders_skipped   INTEGER NOT NULL DEFAULT 0,
  -- per-stage hits the sweep produced (Stage Z/A/B/C/D)
  hits_z            INTEGER NOT NULL DEFAULT 0,
  hits_a            INTEGER NOT NULL DEFAULT 0,
  hits_b            INTEGER NOT NULL DEFAULT 0,
  hits_c            INTEGER NOT NULL DEFAULT 0,
  hits_d            INTEGER NOT NULL DEFAULT 0,
  hits_unidentified INTEGER NOT NULL DEFAULT 0,
  mb_requests       INTEGER NOT NULL DEFAULT 0,
  bytes_hashed      INTEGER NOT NULL DEFAULT 0
);

-- Add to the existing `folders` table:
ALTER TABLE folders ADD COLUMN diagnostic       TEXT;
ALTER TABLE folders ADD COLUMN sweep_generation INTEGER;
```

These are v2 of the library schema (`kLibrarySchemaVersion = 2`). The
existing `applyV1Schema` stays as-is for backwards compat; a new
`applyV2Schema` adds these tables and ALTERs.

The `folders.diagnostic` column holds the multi-line text from
`FolderIdentifier::Result::diagnostic` — the per-stage trace ("Stage Z
miss: no marker; Stage A miss: no quorum on MBID; Stage B miss: no
disc-id tag; Stage C miss: no candidate"). This is what the UI shows
when the user right-clicks an unidentified folder.

### 5.2 Atomic transitions

The status state machine:

```
       queued                     (initial)
         │
         ▼
    in_progress
       /    \
      ▼      ▼
    done   failed
      │       │
      ▼       ▼
    (terminal — no further transitions until generation bumps)

  skipped   (terminal — set on file I/O failure, locked folder, etc.)
  locked    (terminal — set when folders.is_locked=1)
```

Transitions are single UPDATE statements with status WHERE clauses, so
they're atomic against concurrent reads:

```sql
-- "Pick the next folder to work on, atomically claim it."
UPDATE sweep_queue
   SET status = 'in_progress', started_at = strftime('%s','now')
 WHERE id = (
   SELECT id FROM sweep_queue
    WHERE status = 'queued'
      AND generation = ?
    ORDER BY priority ASC, queued_at ASC
    LIMIT 1)
 RETURNING id, folder_path, ...;
```

(SQLite 3.35+ supports `UPDATE … RETURNING`; macOS ships ≥3.39 on
Sonoma. If we have to target older SQLite we do `SELECT id …` then
`UPDATE … WHERE id = ?` in a single transaction. Either way: atomic.)

### 5.3 Sweep generation

When the user changes the library root (or clicks Re-scan with
`force=true`), increment `generation` (`MAX(generation)+1` from
`sweep_runs`). New `sweep_queue` rows are inserted with the new
generation. Old-generation rows are kept for history but ignored by
the pull loop's `WHERE generation = current_gen` filter.

A "drop history older than N generations" cleanup job is a v1.1
nicety, not v1.

### 5.4 On-startup recovery

At app boot, the sweeper re-queues any in-flight rows:

```sql
UPDATE sweep_queue
   SET status = 'queued',
       started_at = NULL,
       attempts = attempts + 1
 WHERE status = 'in_progress';
```

Then finds the open run (`SELECT MAX(generation) FROM sweep_runs
WHERE finished_at IS NULL`) and resumes pump. `attempts` caps at 3:
on the third re-queue, the folder is marked `failed` permanently with
`last_error = "exceeded retry budget"`. User can right-click "Try
again."

### 5.5 SQLite + WAL config

WAL handles one writer (the sweeper) + many readers (the QML library
view). At open: `PRAGMA journal_mode=WAL`, `PRAGMA synchronous=NORMAL`,
periodic `PRAGMA wal_checkpoint(TRUNCATE)` on idle. The DB lives at
`~/Library/Application Support/Concerto/library/library.db`, which is
*not* in iCloud by default (per `LIBRARY_METADATA_PLAN.md` §6.4).
Users who move it there are on their own.

---

## 6. Re-sweep policy

When does the sweeper revisit a `done` folder?

### 6.1 The three triggers

| Trigger | Action |
| --- | --- |
| User clicks **Re-scan library** | All folders → re-enqueue (force). Stage Z fast-paths 99% of them; only newly-added folders cost MB calls. |
| `kConcertoPipelineVersion` is bumped | Folders whose `releases.pipeline_version < kConcertoPipelineVersion` re-enqueue at low priority. Stage Z's version check (§A.6 of the metadata plan) gates the re-fetch. Auto-triggered on app boot after a binary upgrade. |
| User clicks **Re-identify this folder** | One specific folder → enqueue at user-priority. |

### 6.2 What about file changes?

The default is: **don't auto-re-sweep on file mtime changes**. The user
adds a track to an existing folder; until they click Re-scan, the
sweeper doesn't know.

The dev's intuition might be different. Two reasons to keep this
conservative in v1:

- File-system change detection on macOS is not trivial (FSEvents
  wrapper).
- The marginal value is low: a Picard-tagged file added to an
  existing folder will Stage-Z trust-pass on the next manual scan in
  <1 second.

In v1.1, add FSEvents → mark the folder's `sweep_queue.status` as
`queued` so the next sweep run picks it up. Cheap to add later.

### 6.3 Failed-stage retry policy

A folder that fell to Stage F (unidentified, all stages tried) gets:

- One try per sweep generation.
- Sticky status: `done` with `release_mbid IS NULL` and a non-empty
  `diagnostic`. The UI shows the "Unidentified" badge.
- Re-tried only on user action ("Try again" right-click) or on the
  next user-initiated Re-scan.

This avoids hammering MB with the same hopeless folders on every
boot.

### 6.4 Lock semantics

`folders.is_locked = 1` (from `LIBRARY_METADATA_PLAN.md` §8.6) freezes
the folder. The sweeper:

- Skips it on enumeration (the row is inserted with status=`locked`
  to record the skip).
- Honors the lock across re-scans.
- Lifts the lock only on explicit user action.

---

## 7. UI surface — signal vocabulary

The Sweeper is a non-UI object; QML binds to its signals and
properties. The signal vocabulary:

```cpp
class LibrarySweeper : public QObject {
  Q_OBJECT
public:
  Q_PROPERTY(SweeperState state READ state NOTIFY stateChanged)
  Q_PROPERTY(int foldersTotal READ foldersTotal NOTIFY progress)
  Q_PROPERTY(int foldersDone  READ foldersDone  NOTIFY progress)
  Q_PROPERTY(int foldersFailed READ foldersFailed NOTIFY progress)
  Q_PROPERTY(QString currentFolderPath READ currentFolderPath NOTIFY folderStarted)
  enum class SweeperState { Idle, Enumerating, Running, Paused, RateLimited, Finished };

signals:
  void started(qint64 generation, QString rootPath);
  void enumerated(int totalFolders);              // after FolderEnumerator finishes
  void folderStarted(QString folderPath, int index, int total);
  void folderCompleted(QString folderPath, QString sourceTag, QString releaseMbid);
  void folderFailed(QString folderPath, QString reason);
  void folderSkipped(QString folderPath, QString reason);
  void progress(int done, int failed, int total);  // emitted every ~500 ms
  void paused(QString reason);                     // "user", "network-down", "rate-limited"
  void resumed();
  void finished(qint64 generation, int totalDone, int totalFailed);
  void diagnostic(QString line);                   // free-form text for debug pane
};
```

### 7.1 What the user sees

Three layers, increasing intrusiveness:

1. **Library-header chip** — `Sweeping… 124 / 700  [Pause] [Cancel]`
   above the album grid. Auto-hides on `Idle | Finished`. Cancel is
   confirmation-gated if any folders have completed.
2. **Per-folder badges in the album grid** — spinner
   (`in_progress`), green check (`done` w/ MBID), yellow `?` (`done`
   unidentified), red `x` (`failed`), lock (`is_locked`).
3. **Full-screen first-sweep modal** — only on the *very first*
   sweep: ETA, current folder, live mini-log of recent completions
   ("Just identified: Brahms Symphony No. 4 — Carlos Kleiber").
   Subsequent sweeps use only the chip.

### 7.2 Notifications

In-app toast on: sweep finish ("698 identified, 2 unidentified"),
network-down auto-pause, rate-limit auto-pause, >5% folder-failure
rate. Suppress everything else as noise. System notifications via
`QSystemTrayIcon` are v1.1.

### 7.3 Pause / resume / cancel

Pause: drain in-flight folder, sleep on `QWaitCondition`. Resume:
signal the condition. Cancel: drain in-flight folder (commit on
success / rollback on mid-way crash), mark remaining `queued` rows
as `skipped` for this generation, emit `finished`. Cancel sits
behind a confirmation modal.

### 7.4 Settings → Library fields

- **Library folder** (current root; change kicks new generation)
- **Re-scan library** (button; re-enqueue all)
- **Exclude patterns** (text area) — v1.1
- **Auto-watch** (toggle) — v1.1
- **Force re-fetch** (button + confirm; bumps a force-gen counter
  Stage Z honors)

---

## 8. Cancellation, pause, atomic per-folder writes

### 8.1 The unit of atomicity is the folder

One DB transaction per folder wraps every related write:

```cpp
db.beginTransaction();
db.upsertAlbumMeta(album);
for (const auto& fr : fileRows) db.upsertFile(fr);
db.upsertFolder(folderRow);
db.updateSweepQueue(job.id, status=done, diagnostic=...);
db.bumpSweepRun(generation, hits, ...);
db.commitTransaction();
```

App killed mid-transaction → SQLite rolls back, `sweep_queue.status`
stays at `in_progress`, startup recovery (§5.4) re-queues.

### 8.2 Mid-folder cancel = drain, not abort

Cancel does **not** short-circuit a mid-stage HTTP request. The
folder is ≤1 s of work after acquiring the gate token; aborting
mid-HTTP risks a stuck token. Cancel sets a flag, the in-flight
folder completes its current MB request and commits-or-rolls-back,
then the worker exits the loop.

### 8.3 Pause = drain-current-folder

Same semantics as cancel for the in-flight folder; sweeper sleeps on
`QWaitCondition`. `resume()` signals; `cancel()` wakes-then-exits.

### 8.4 Queue-jumping for user-driven open

Covered in §4.2: `identifyNow(folderPath)` promotes the row to
`PRIORITY_USER` (= 5). ≤1 s latency from click to result.

### 8.5 Force-quit recovery

WAL recovers transactions; startup re-queue (§5.4) re-feeds
`in_progress` rows. UI shows "Resuming sweep" on next launch.

### 8.6 Rip path is orthogonal

Rip-time writes directly to the DB (§1.3); it doesn't touch the
sweep queue. Pausing the sweeper does *not* pause rip-time
persistence.

---

## 9. Errors, retries, network outage

### 9.1 Failure-mode catalog

| Failure | Folder result | Sweep result |
| --- | --- | --- |
| MB returns 5xx / 503 / 429 | Retry up to 3× with backoff (1s, 5s, 30s); on persistent failure, requeue at the back, status stays `queued`, `attempts++` | Continues |
| MB returns 4xx (404, 401, 400) | Stage failed; falls to next stage in `FolderIdentifier`. If chain exhausts → `done` with `release_mbid IS NULL`. | Continues |
| Network completely down (DNS fails / connection refused) | Pause MB-dependent jobs for 60 s, continue Stage Z folders. Emit `paused("network-down")`. Periodic re-check every 60 s. On reconnect: emit `resumed()` and continue. | Continues |
| File I/O fail (permission, unmounted volume) | Mark folder as `skipped` with `last_error = "I/O: <details>"`. Skip the folder, don't retry until user re-mounts and re-scans. | Continues |
| TagLib parse error on one file | Skip the file; log; continue with the rest of the folder. The folder still has N-1 files for the FolderIdentifier quorum check. | Continues |
| `AudioFrameHash::compute` returns empty | Skip the hash for that file; tag-based identification still works. The file is recorded in `files` table with `content_hash = ""`. | Continues |
| SQLite write fail | Roll back transaction; mark folder `failed` with the SQLite error; emit warning. If repeated 3+ times (DB locked, disk full): emit `paused("db-error")` and stop. | Pauses |

### 9.2 Network-down detection

`QNetworkAccessManager::networkAccessibleChanged()` is deprecated in
Qt 6 and useless. Substitute: ≥3 consecutive MB failures with
`QNetworkReply::HostNotFoundError` or `RemoteHostClosedError` → treat
as offline. Re-poll every 60 s with a HEAD on `https://musicbrainz.org/`;
on success, resume. While offline-paused, Stage Z folders continue at
full speed; MB-dependent folders skip. The user perceives slow
progress, not a stall.

### 9.3 Exponential backoff

`backoff_seconds[attempt] = { 1, 5, 30 }`. After 3 failures the
folder is requeued to the back with `attempts++`. The 7th total
attempt marks it `failed` permanently for that generation. User can
right-click "Try again."

### 9.4 Corrupt audio file isolation

`AudioTagIo::read()` returns `nullopt` on parse fail; `AudioFrameHash::
compute()` returns empty on decode fail. Either case: record the
file with `format = '<unknown>'`, `duration_sec = 0`, `content_hash
= ''`, **drop it from the quorum calculation**, continue with the
rest of the folder. If every file is corrupt: folder is `skipped`
with `last_error = "no readable audio files"`.

### 9.5 TagLib fail-soft

TagLib has known crashers on malformed MP4 atoms. The existing API
returns `nullopt` rather than throwing — handled. If TagLib actually
crashes, the worker thread dies, the GUI stays up, next-launch
recovery marks the run failed. Long-term mitigation (child-process
isolation) is overkill for v1.

---

## 10. Performance tuning — the four we recommend

From the brief's longer list, pick the four with the best
return-on-effort:

### 10.1 Path + size + mtime probe before hashing — HIGH VALUE

`AudioFrameHash::compute` is 150–500 ms per file. On re-sweeps,
hashing unchanged files is pure waste. Probe before:

```cpp
auto known = db.findFileByPath(absPath);
if (known
    && known->sizeBytes == QFileInfo(absPath).size()
    && known->lastSeenTs > QFileInfo(absPath).lastModified().toSecsSinceEpoch()) {
  contentHash = known->contentHash;        // reuse — byte-identical to last hash
} else {
  contentHash = AudioFrameHash::compute(absPath);
  db.upsertFile({contentHash, ...});
}
```

Re-sweep of a 10k-track library skips ~50 minutes of CPU.
Safe-always: a false-positive on the probe just skips a re-hash, it
never assigns the wrong hash. Edge case (tag-edit preserves size +
mtime) is harmless because the audio-frame hash is *over the audio
frames*, which don't change.

### 10.2 Batch DB writes per folder — MEDIUM VALUE

One `BEGIN…COMMIT` per folder, not per track. Saves ~10× on WAL
fsyncs (already noted in §8.1).

### 10.3 Skip MB second-hop for releases already cached — HIGH VALUE

Extend the `MetadataCache::getByDiscId()` pattern (live in Stage B)
to Stage A — release-JSON cache keyed by MBID via the
`releaseAliasPath` shim (`LIBRARY_METADATA_PLAN.md` §3.2). Zero MB
calls for repeat hits. Compounds with §10.1 to make re-sweep near-
instant.

### 10.4 Defer Cover Art Archive — MEDIUM VALUE

CAA fetches use rate-gate tokens that identification needs.
`CoverArtFetch(release_mbid)` jobs enqueue at `PRIORITY_LOW` (800);
pull loop runs them only when no `ResolveFolder` jobs are queued.
UI shows placeholder covers during identification; covers fill in
afterward. CAA has its own gate (§3.2) sharing the MB IP ceiling.

### 10.5 Skipped optimizations (not v1)

- Skip re-reading tags on unchanged files — transitively covered by
  §10.1.
- Concurrent MB fetches across sources — only MB matters in v1.
- HTTP/2 multiplexing — gated by 1/s anyway.
- Pre-aggregated quorum table — micro-opt.

---

## 11. Telemetry / diagnostics

The dev's "right-click the folder, see why" debug surface.

- **`sweep_runs` aggregate** (§5.1): one row per `start()` invocation
  with per-stage hit counts, MB request count, bytes hashed. Powers
  the post-sweep summary toast ("Identified 698, unknown 2, took
  12m37s") and long-term tracking ("Stage Z hit rate climbed from
  0% on first sweep to 95% by 5th").
- **`folders.diagnostic` per row** — `FolderIdentifier`'s existing
  `Result::diagnostic` string plumbed straight in on upsert.
  Multi-line trace like `[Stage Z] no marker in 0/13 files: miss`
  / `[Stage A] no MBID quorum (best: 4/13 = 31%): miss` / etc.
  Right-click → "Show identification log" → dialog renders it.
- **Per-MB-request log** behind the `Settings → Library → Show
  identification debug pane` toggle (mirrors
  `LIBRARY_METADATA_PLAN.md` §9.5). Rolling
  `~/Library/Logs/Concerto/sweeper.log`, capped at 10 MB.
- **`diagnostic(QString)` signal** streams human-readable lines to
  the in-app debug pane (`[12:34:01] folder 'Mahler Symphony No. 9'
  → MB call 1247: 200 OK 4.2 KB`). QML appends to a ring-buffered
  `ListView`.

---

## 12. Class skeleton

```cpp
// src/LibrarySweeper.h
#pragma once
#include "FolderIdentifier.h"
#include "LibraryDatabase.h"
#include "MusicBrainz.h"
#include "RateGate.h"
#include <QObject>
#include <QString>
#include <QThread>

namespace concerto::library {

class FolderEnumerator;

class LibrarySweeper : public QObject {
  Q_OBJECT
public:
  enum class State { Idle, Enumerating, Running, Paused, RateLimited, Finished };
  Q_ENUM(State)
  Q_PROPERTY(State state             READ state             NOTIFY stateChanged)
  Q_PROPERTY(int foldersTotal        READ foldersTotal      NOTIFY progress)
  Q_PROPERTY(int foldersDone         READ foldersDone       NOTIFY progress)
  Q_PROPERTY(int foldersFailed       READ foldersFailed     NOTIFY progress)
  Q_PROPERTY(QString currentFolderPath READ currentFolderPath NOTIFY folderStarted)
  Q_PROPERTY(qint64 generation       READ generation        NOTIFY stateChanged)

  // Non-owning deps; main.cpp owns the lifetimes.
  explicit LibrarySweeper(LibraryDatabase* db,
                          musicbrainz::Client* mb,
                          concerto::metadata::MetadataCache* cache,
                          RateGateRegistry* gates,
                          QObject* parent = nullptr);
  ~LibrarySweeper() override;

  // accessor getters elided …

public slots:
  void start(const QString& root, bool force = false);
  void pause(const QString& reason = "user");
  void resume();
  void cancel();
  void identifyNow(const QString& folderPath);
  void retryFolder(const QString& folderPath, bool pipelineForce = false);

signals:
  void started(qint64 generation, QString rootPath);
  void enumerated(int totalFolders);
  void folderStarted(QString folderPath, int index, int total);
  void folderCompleted(QString folderPath, QString sourceTag, QString releaseMbid);
  void folderFailed(QString folderPath, QString reason);
  void folderSkipped(QString folderPath, QString reason);
  void progress(int done, int failed, int total);
  void paused(QString reason);
  void resumed();
  void rateLimited(int retryAfterSec);
  void finished(qint64 generation, int totalDone, int totalFailed);
  void stateChanged();
  void diagnostic(QString line);

private:
  class Worker;  // moved to its own QThread; slots wired via queued connection
  QThread* m_thread = nullptr;
  Worker*  m_worker = nullptr;
  // ... non-owning dep pointers + state fields ...
};

}  // namespace concerto::library
Q_DECLARE_METATYPE(concerto::library::LibrarySweeper::State)
```

Companion rate-gate types:

```cpp
// src/RateGate.h — token bucket per source.
class RateGate : public QObject {
  Q_OBJECT
public:
  RateGate(QString source, double tokensPerSec, int maxBacklog, QObject* parent = nullptr);
  bool tryAcquire();
  bool acquireBlocking(int timeoutMs = 60000);   // spins QEventLoop on caller thread
  int  available() const;
signals: void ready();
};

class RateGateRegistry : public QObject {
  Q_OBJECT
public:
  explicit RateGateRegistry(QObject* parent = nullptr);
  RateGate* gate(const QString& source);   // lazy; policy table per source
};
```

### 12.1 Ownership tree

```
main.cpp owns:
  LibraryDatabase db, MetadataCache cache, QNetworkAccessManager nam,
  musicbrainz::Client mb(&nam), RateGateRegistry gates,
  LibrarySweeper sweeper(&db, &mb, &cache, &gates)
      └── owns its own QThread + a private QNAM (separate from `nam`
          so sweep network I/O doesn't compete with rip-time I/O).
```

Singleton-ish (one instance, not statically allocated) — same shape as
`Ripper` / `MetadataResolver`. Exposed to QML via
`qmlRegisterUncreatableType<LibrarySweeper>` + a context property
named `librarySweeper`; QML calls become
`librarySweeper.start("/Users/...")`.

---

## 13. The rip-just-finished path

Restated for emphasis: **rip path writes directly to
`LibraryDatabase`. The Sweeper is not notified.**

Implementation in `main.cpp` where the existing `Ripper::discSaved`
signal is wired:

```cpp
connect(&ripper, &Ripper::discSaved, [&](QString fromTempDir, QString finalPath) {
  // Existing playlist remap stuff stays.

  // NEW: write to library DB. Resolver result is cached on the Ripper.
  AlbumMeta album = ripper.lastResolvedAlbum();
  libraryDb.upsertAlbumMeta(album);
  QDirIterator it(finalPath, {"*.flac"}, QDir::Files);
  while (it.hasNext()) {
    QString flacPath = it.next();
    FileRow fr;
    fr.path = flacPath;
    fr.format = "flac";
    fr.releaseMbid = album.releaseMbid;
    fr.trackPosition = parseTrackPositionFromFilename(flacPath);
    fr.mediumPosition = album.discPosition;
    fr.writebackAt = QDateTime::currentSecsSinceEpoch();  // markers were just written
    fr.contentHash = AudioFrameHash::compute(flacPath);
    libraryDb.upsertFile(fr);
  }
});
```

The Sweeper is genuinely redundant for ripped folders — every file
already carries `CONCERTO_*` markers, so Stage Z would trust-pass on
any future scan. But the redundancy costs nothing in steady state:
the user can re-scan, the sweeper hits Stage Z in <50 ms per folder,
no MB call. The DB row is already there.

There is one subtle benefit of the direct-write path: the user sees
the album in the library view *immediately* after rip finish, with
zero sweep delay. With the sweeper-notification approach there'd be
a 1-second-or-so window. Direct write wins on UX too.

---

## 14. Edge cases worth a paragraph each

### 14.1 50,000-track library

~3,300 albums. First-sweep ~55 minutes (MB-rate bound). Queue is on
disk; in-memory state is the single in-flight job (~few KB). DB at
~30 MB per 700 releases (`LIBRARY_METADATA_PLAN.md` §6.4) → ~140 MB
for 3,300 releases. Per-folder transaction is ~15 inserts; SQLite WAL
handles this fine. QML side: virtualize the album grid (standard
`ListView` recycling) so only on-screen cards hold model rows.
Nothing breaks.

### 14.2 Library on a slow NAS

Read latency 50–200 ms per file vs ~1 ms on SSD. Per-folder tag-read
+ hash phase balloons to ~10 s; a 700-album library → ~2 hours
first-sweep, NAS-dominated. Mitigations: the §10.1 probe still
applies on re-sweep (mtime check is ~50 ms even on NAS → re-sweep
~30 s); cache the directory listing per folder (one `QDirIterator`,
no repeated stats); detect via `QStorageInfo::fileSystemType() ∈
{smbfs, afpfs, nfs}` and surface a warning. Don't optimize harder —
user picked a NAS, user pays the I/O cost.

### 14.3 iCloud Drive eviction

If the user puts `~/Music` under iCloud Drive, files can be evicted
to `.icloud` stubs. Implications: filter `*.icloud` out of the
audio-extension list and check `QFileInfo::size() > 0` before reading;
each access can trigger seconds of re-download (a sweep effectively
asks iCloud to re-fetch the entire library); MB rate is no longer the
bottleneck. Detect via `*.icloud` presence in the root; surface a
warning ("Files will be downloaded as they're scanned"). No
pre-prefetch — Apple's API for that is private.

### 14.4 Multi-disc box layouts

Confirmed handled by `FolderIdentifier` per `LIBRARY_METADATA_PLAN.md`
§7.5. Per-disc-folder layout (`Box/CD01/`, …): each subfolder is one
sweep job; releases joined in the DB via `release_group_mbid`.
All-in-one layout (50 tracks in one dir, multiple `DISCNUMBER` values):
`MUSICBRAINZ_ALBUMID` quorum identifies the release; `DISCNUMBER`
tags populate `files.medium_position`. Sweeper just passes the folder.

### 14.5 Mixed-content folder (5 classical FLACs + 8 pop MP3s)

`FolderIdentifier::Stage A` quorum is per-MBID. 5/13 = 38% is below
the 60% threshold → Stage A misses; chain exhausts → `done` with
`release_mbid IS NULL`. Suboptimal: we *could* identify the 5
classical files individually. Per-file identification is v1.1 work
(`LIBRARY_METADATA_PLAN.md` §8.2). v1 mitigation: when ≥2 distinct
MBIDs each have ≥30% quorum, emit `sourceTag = "mixed-content"` so
the UI can flag the folder. User can manually re-organize.

### 14.6 iTunes-exported MP3s

The flagship onboarding case. iTunes/Music.app MP3s carry `TCON` /
`TIT2` / `TPE1` / `TALB` / `TPOS` but rarely `MUSICBRAINZ_*`. Flow:
Stage Z miss, Stage A miss, Stage B miss, **Stage C (search)** lights
up — `ARTIST` + `ALBUM` text against MB. Most iTunes albums find a
match; classical sometimes misses because iTunes' "Beethoven:
Symphony No. 9" differs from MB's "Symphony No. 9 in D minor, Op.
125". Stage D's track-count + duration fingerprint catches most of
those. On hit, MB enrichment lives in the sidecar; the user's tags
are preserved per `LIBRARY_METADATA_PLAN.md` §8.5
(augment-not-overwrite). iTunes MP3s often carry embedded album art —
`AudioTagIo::extractArtwork()` (later, separate concern) means the
library view can show covers without a CAA fetch.

### 14.7 User moves their music root

Sweeper bumps generation, re-enumerates from the new root. Old-gen
rows kept; queries filter on `generation = current_gen`. v1.1 adds
a "cleanup orphans" action that drops rows whose path no longer
exists.

### 14.8 Two Concerto instances open simultaneously

OS advisory lock on `library.db.lock` at sweeper start; if held,
emit `paused("library-locked")` and don't sweep. Read-only library
view stays usable. Cross-machine sync via iCloud is explicitly out
of scope (`LIBRARY_METADATA_PLAN.md` §8.9).

### 14.9 USB drive unplugged mid-sweep

Periodic `QStorageInfo(rootPath).isReady()` check every 60 s. On
false → `paused("volume-disconnected")`. Folders processed mid-pull
get `skipped` with `last_error = "volume disconnected"` and re-tried
automatically on reconnect (60 s poll).

---

## 15. Out of scope for v1

Explicitly not in the v1 Sweeper:

- **Stage E (AcoustID / Chromaprint)**. The fingerprinter is its own
  workstream per `LIBRARY_METADATA_PLAN.md` §10 Step 5. The Sweeper
  v1 reaches Stage D and stops; unidentified folders are surfaced
  via the badge but not auto-fingerprinted. v1.5 adds the
  `Fingerprint` job kind.
- **Apple Music Stage 1.5 enrichment** of identified folders. v2 per
  `LIBRARY_METADATA_PLAN.md` §5.4.
- **Tag writeback** (Step 7 of the metadata plan). The Sweeper
  writes to the DB only; the writeback toggle and its execution are
  a separate workstream.
- **Cover Art fetch** (Sweeper schedules low-priority CAA jobs but
  the actual CAA client + UI display are a separate concern).
- **Multi-root libraries**. v1 ships with one root.
- **FSEvents-based auto-watch**. v1.1.
- **Cross-machine library sync** (the user with laptop + desktop +
  iCloud). Out of v1 per `LIBRARY_METADATA_PLAN.md` §8.9.
- **Streaming sources (Tidal, Spotify, etc.)**. Not Concerto's
  product.
- **A "merge two albums" / "split this album into compilations"
  editing UI**. Not the Sweeper's job; would be a v1.1+ library
  curation feature.
- **A "fix my whole library" automated cleanup of duplicates,
  bad tags, etc.** Out of v1.

---

## 16. Recommended architecture (one paragraph)

The `LibrarySweeper` is a singleton-ish background-worker QObject
owned by `main.cpp`, running its enumeration + identification +
persistence on a **dedicated QThread** with help from a small
**QThreadPool** (2–4 workers) for CPU-bound `AudioFrameHash`. It pulls
work from a **persistent `sweep_queue` table** in the existing
`library.db`, with atomic `status` transitions so an app crash never
leaves a folder stuck — startup recovery re-queues `in_progress`
rows. All MusicBrainz traffic flows through a **shared
`RateGateRegistry`** (source-keyed; the `"musicbrainz"` gate is a
hard 1 req/sec leaky bucket per the published MB policy, no burst;
other sources get their own gates as they land). The pipeline per
folder is: enumerate (`QDirIterator` over the user's library root,
disc-suffix-regex hint for multi-disc layouts) → tag-read via
`AudioTagIo` → lazy hash via `AudioFrameHash` (skipped on re-sweep
when path+size+mtime match the DB) → `FolderIdentifier::identify()`
(Stages Z+A+B+C+D) → one DB transaction per folder writing
`releases`/`tracks`/`files`/`folders`/`sweep_queue` updates atomically.
Triggering is explicit: first-launch onboarding picks the root, the
"Re-scan library" button re-enqueues with `force=true`,
post-rip writes the library DB row directly (the Sweeper is **not**
notified for rip outputs because they're already trusted). User-driven
"open this folder now" jumps the queue at `PRIORITY_USER`. Stage E
AcoustID, tag writeback, Cover Art Archive fetch logic, and Apple
Music v2 enrichment are all **deferred past v1**.

The three load-bearing decisions: **(a) a persisted SQLite queue with
atomic state transitions** (not an in-memory `QQueue` — the user will
quit the app mid-sweep; this is non-negotiable); **(b) a shared,
source-keyed `RateGateRegistry`** (rip-time and sweep-time both go
through the same MB gate; Apple Music v2 lands as a new gate without
sweeper changes); **(c) lazy hashing via path+size+mtime probe**
(re-sweep wall-clock drops from minutes to seconds, which is the
single biggest UX win on a real classical library).

---

## 17. Prioritized implementation order

Seven steps. Each lands one self-contained piece with tests before
the next starts. Engineer-day each except as noted.

1. **`RateGate` + `RateGateRegistry`** (`src/RateGate.{h,cpp}` +
   `src/RateGateRegistry.{h,cpp}`). Per-source token bucket. ~150
   LoC. Wire into `MetadataResolver` first so the rip flow
   exercises it before the Sweeper depends on it.
2. **v2 schema migration** in `LibraryDatabase`: add `sweep_queue` +
   `sweep_runs` tables, ALTER `folders` for `diagnostic` +
   `sweep_generation`. Typed access methods (`claimNextQueued`,
   `markSweepJob`, `startSweepRun`, `finishSweepRun`). ~250 LoC.
3. **`FolderEnumerator`** (`src/FolderEnumerator.{h,cpp}`). Recursive
   walk + disc-suffix regex + skip-pattern logic; pure (no DB
   writes); outputs `QVector<SweepJob>`. ~200 LoC. Most easily
   unit-tested piece — temp-dir fixture covers single-folder,
   multi-disc, skip patterns.
4. **`LibrarySweeper` worker thread** (`src/LibrarySweeper.{h,cpp}`
   + private `Worker`). Pull-pipeline, rate gate, backoff,
   network-down detection, `identifyNow` queue-jumping, the §10.1
   probe and §10.2 batched DB writes. ~700 LoC; biggest step.
   Integration test: synthetic library through full pipeline with
   mocked MB client.
5. **Rip-just-finished direct-write hook** in `main.cpp`. ~50 LoC.
   Add `Ripper::lastResolvedAlbum()` getter if missing.
6. **`main.cpp` wiring + QML exposure**. Boot the registry, the
   sweeper, startup recovery, the first-launch onboarding card.
   `qmlRegisterUncreatableType<LibrarySweeper>`; Settings → Library
   fields. ~150 LoC C++ + ~100 LoC QML.
7. **UI surface** in `qml/LibraryView.qml` (created by
   `LIBRARY_METADATA_PLAN.md` Step 6 anyway): header chip,
   per-folder badges, first-sweep modal. ~300 LoC QML.

After Step 7 the v1 Sweeper ships. Step 10.4 CAA deferral folds into
Step 6 (job priority on the pull loop). v1.5 adds Stage E +
`Fingerprint` job kind; v1.1 adds multi-root + auto-watch; v2 adds
Apple Music for the library flow.

---

## 18. Open questions for the dev

Five things I can't decide alone:

1. **Default library root**: open `QFileDialog` rooted at `~/Music`
   (my recommendation — simple, predictable), or heuristically
   auto-detect by scanning `~/Music` / `~/Documents/Music` /
   mounted volumes for `.flac` / `.mp3` densities and pre-selecting
   the most likely? The dev knows their setup; flagging in case
   "they keep music on `/Volumes/RAID/Music`" is the common case.

2. **First-sweep UX**: full-screen modal during the very first sweep
   (my recommendation — onboarding-friendly), then a discreet header
   chip for re-sweeps; or header chip always (less intrusive but
   less clear)?

3. **Generation history retention**: keep old-generation rows
   indefinitely (my v1 recommendation — the DB stays small), or
   auto-prune after N generations? Affects the "I changed my root,
   what happens to my old data" story.

4. **`kConcertoPipelineVersion` bump UX**: silent re-fetch on next
   app boot (my recommendation), or surface a "new metadata fields
   available — re-fetch your library?" modal? Silent is friendlier;
   modal is more transparent.

5. **Auto-watch in v1 vs v1.1**: I recommend v1.1 (FSEvents wrapping
   is a real subproject). If auto-watch is table-stakes, it lands as
   Step 7.5 (~1.5 engineer-days).

---

## Sources cited

- `docs/LIBRARY_METADATA_PLAN.md` — §2 (the identification chain),
  §4 (`AudioTagIo` / TagLib), §6.3 (the existing schema), §7
  (lookup-timing), §A (provenance markers), §10 (implementation
  order), §12 (revised implementation order from 2026-05-16).
- `docs/METADATA_PLAN.md` — §1 (MB User-Agent + rate-limit policy
  context), §6 (resolver / scoring threading model).
- `docs/METADATA_PIPELINE_AUTOMATED.md` — rip-pipeline distillation.
- `src/FolderIdentifier.{h,cpp}` — the per-folder strategy chain
  the Sweeper invokes.
- `src/LibraryDatabase.{h,cpp}` — schema + typed access patterns
  (current v1 schema; v2 adds `sweep_queue` / `sweep_runs`).
- `src/AudioTagIo.{h,cpp}` — multiformat tag reader the Sweeper
  uses (lines 29–72 for the API surface).
- `src/AudioFrameHash.{h,cpp}` — content-hash routine; static
  method `compute(path, firstSeconds=30)`.
- `src/MetadataResolver.{h,cpp}` — async resolver pattern the
  Sweeper mirrors (`Request`/`Result`/signal vocabulary).
- `src/MusicBrainz.{h,cpp}` — async client; methods
  `Client::fetchDiscId(id)` and `Client::fetchRelease(mbid)`. No
  rate limiter at this layer; the Sweeper plus rip-flow both go
  through the new `RateGateRegistry`.
- `src/RipWorker.{h,cpp}` — reference for "long-running worker
  thread that emits progress signals." The Sweeper's signal shape
  mirrors `discIdentified` / `readProgress` / `failed` etc.
- `src/PendingSubmissions.{h,cpp}` — append-only persisted queue
  pattern; the Sweeper's `sweep_queue` is similar in spirit
  (persisted, append-only conceptually, atomic transitions) but
  lives in SQLite rather than JSONL because it has rich query
  needs (status filtering, generation isolation, priority sort).
- `src/library_cli_main.cpp` — test harness shape; `audioExtensions()`
  set lives here today and should migrate to a shared header.

External:

- [MusicBrainz API](https://musicbrainz.org/doc/MusicBrainz_API) —
  the canonical 1-req/sec/IP policy. **All users of the API must
  ensure that each of their client applications never make more
  than ONE call per second.** Penalty: IP block. No burst window
  documented. Verified 2026-05-18.
- [MusicBrainz API Rate Limiting](https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting) —
  distinguishes "anonymous User-Agent" (50 req/sec averaged, 503s
  on excess) from "IP-based" (1 req/sec). The Sweeper sets a
  meaningful `User-Agent: Concerto/<ver> ( <contact> )` per
  `METADATA_PLAN.md` §0 and is therefore subject to the IP-based
  ceiling. Verified 2026-05-18.

End of document.
