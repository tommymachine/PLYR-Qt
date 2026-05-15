# SCANCERTO — Concerto's CD Rip UI

The GUI surface for the CD-rip pipeline. Everything described below is
**built and visually working against stub data**; what's left is wiring
the stub `Ripper` to the real `runRip` machinery (already validated end-
to-end in `cdrip_cli`). Read this top to bottom once before continuing
the work — the architectural decisions are deliberate and several have
already been re-litigated and resettled.

If you're picking this up cold:
1. `CDRIP_STRATEGY.md` — the project-wide non-negotiables (no GPL, no
   `cdparanoia` subprocess, etc.). Don't reopen those.
2. `CD_READER_PLAN.md` — how the native macOS CD reader was built.
3. `src/cdrip_main.cpp::runRip` — the *reference rip flow* that this
   UI is going to drive. Every signal the QML side renders has a source
   in that function.

## Visual & flow summary

Click the 💿 button in the header → opens a non-modal `RipView` popup
(880×600) over the main player.

States, driven by `plyr::cd::Ripper::State`:

| State          | What's on screen                                     |
|----------------|------------------------------------------------------|
| Idle           | (rip view closed)                                    |
| WaitingForDisc | CD with no tracks; big right-side "Insert a CD to rip" |
| Identifying    | "Identifying disc…" while TOC + MB + offset resolve  |
| Reading        | CD fills inside→out with blue overlay over iridescent silver; ETA in top-right |
| Encoding       | Per-track ring lights up briefly as each FLAC is written |
| Verifying      | Per-track ring goes green / amber / red as AR + CTDB report |
| SavePending    | Save picker auto-opens; cancelling prompts "Delete this rip?" |
| Saving         | "Saving…" while the temp dir is moved into place     |
| Done           | "Tracks saved and playing now."                      |
| Cancelling     | "Discarding the in-progress disc"                    |
| Failed         | error message                                        |

The header pill in `Main.qml` replaces the 💿 button while a rip is
active and the popup is dismissed — clicking the pill re-opens the view.

Keyboard nav for stub review (will come out with the stub):
**← / →** step through demo stages, **Space** pauses / resumes
auto-advance.

## Settled design decisions

These came out of the design sessions; **don't relitigate** without a
new conversation.

1. **Save-at-end, not save-at-start.** Rip writes to
   `~/Library/Application Support/Concerto/rip_in_progress/<uuid>/`,
   then prompts for destination after verify completes. The save folder
   is auto-named from the MusicBrainz match
   (`AlbumTitle_Disc_NN`) and moved into the user's chosen parent.
2. **Auto-play after save.** On disc save → `playlist.openFolder(savedPath)`
   + `setCurrentIndex(0)` + `audio.play()`. For multi-disc sets the user
   listens to disc 1 while disc 2 reads. This is already wired in
   `src/main.cpp` via the `Ripper::discSaved` signal.
3. **Non-modal popup.** RipView is `modal: false` so playback continues
   under it.
4. **Save dialog auto-opens.** No "Save to…" button; entering
   SavePending immediately opens the FolderPicker. Cancelling it
   triggers a "Delete this rip?" confirmation (Choose folder reopens
   the picker, Delete rip discards and closes).
5. **Verify is part of the rip flow.** Not a separate user action.
   Per-track AR + CTDB results render as colored arc rings on the disc
   and `✓ / ⚠ / ✕` glyphs in the track list.
6. **AccurateRip + CTDB results don't gate save.** A warn or fail
   result is informational; the rip still proceeds to SavePending.
7. **Multi-disc batches.** Persistent `RipBatch` state in
   `~/Library/Application Support/Concerto/rip_batches/<batch_id>.json`.
   One file per batch. Keyed by MusicBrainz `release_group_id`.
   Resume-later is real (between-discs); mid-disc resume is **out of v1**.
8. **Disc-matching strictness: loose.** Match on release_group, trust
   MB's `disc.position`. Different individual disc IDs across pressings
   are normal in box sets.
9. **No MB match → standalone rip.** Online MB is the golden path;
   no manual track-entry fallback. Without MB no batch is created.
10. **No batch auto-cleanup.** Old resumable batches sit on disk until
    explicitly deleted by the user.
11. **Cancel mid-rip discards.** The in-progress disc's PCM buffer is
    dropped; the batch (if any) stays resumable with that disc marked
    pending again.
12. **WaitingForDisc shows no track lines.** Tracks appear after the
    TOC has been read in Identifying. Once "resume known batch with
    cached TOC" lands, that path will pre-fill the tracks before
    insertion — there's a note in `src/Ripper.cpp::applyDemoStep`.

## Architecture map

```
┌──────────────────────────────┐
│ qml/Main.qml                 │   header 💿 / pill, ripper context
└──────────────┬───────────────┘
               │
               ▼
┌──────────────────────────────┐
│ qml/RipView.qml              │   non-modal popup, state-driven layout
│                              │
│  ┌────────────────────────┐  │
│  │ qml/CdDiscCanvas.qml   │  │   ShaderEffect + 2 Canvas overlays
│  │  ┌──────────────────┐  │  │
│  │  │ shaders/cd_disc  │  │  │   diffraction shader + matrix-text mask
│  │  └──────────────────┘  │  │
│  └────────────────────────┘  │
└──────────────┬───────────────┘
               │ Q_INVOKABLE / signals
               ▼
┌──────────────────────────────┐
│ src/Ripper.{h,cpp}           │   QObject state machine + stub pipeline
│   (currently stubbed —       │
│    plug RipWorker here)      │
└──────────────────────────────┘
```

### `plyr::cd::Ripper` (`src/Ripper.{h,cpp}`)

`QObject`, lives on the GUI thread, exposed to QML as `ripper`.

- **State enum** (`Q_ENUM`): Idle / WaitingForDisc / Identifying /
  Reading / Encoding / Verifying / SavePending / Saving / Done /
  Cancelling / Failed. QML switches layout on this.
- **Q_PROPERTYs** mirror everything `runRip` produces: drive info,
  MusicBrainz match, disc IDs, track list, per-phase progress, ETA,
  zero-filled ranges, batch context, resumable-batches list.
- **Q_INVOKABLEs**: `startSession`, `endSession`, `stopRip(deleteBatch)`,
  `saveTo(parentUrl, folderNameOverride)`, `ejectDisc`,
  `deleteResumableBatch(id)`.
- **Stub-only invokables**: `demoStep`, `demoToggleAutoAdvance`,
  `demoStepIndex`, `demoStepCount`. Remove with the stub.
- **Signal `discSaved(savedPath)`**: wired in `src/main.cpp` to open the
  saved folder in the playlist and start playback.

The stub pipeline lives in `Ripper::stubAdvance` + `Ripper::applyDemoStep`.
Drives a `QTimer` at 80 ms; walks WaitingForDisc → Identifying →
Reading → Encoding → Verifying → SavePending → Done in ~12 s.

### CD canvas (`qml/CdDiscCanvas.qml` + `shaders/cd_disc.{vert,frag}`)

Three composited layers on top of the popup background:

1. **ShaderEffect (bottom)** — `cd_disc.frag` renders the disc surface:
   silver base, HDR diffraction-grating iridescence (Alan Zucconi /
   `spectral_zucconi6`, n=1..8 with `sinc²/n²` envelope), per-pixel
   wave physics (Zucconi `u = |L·T − V·T|`), micro-jitter for
   shimmer, anisotropic specular streak, ACES tonemap. Two animated
   uniforms drive the morph: `axisAngle` (slow disc rotation) and a
   2-D `viewTilt` vector that rotates at a different rate (the
   morphing comes from V·T variation, not from the light moving).
2. **`matrixCanvas` (middle)** — Canvas2D, painted white-on-transparent.
   Renders the curved matrix text (`CONCERTO v. <ver> · CD-DA · IFPI
   L3777 · Distributed by Three Seven Studios © <year> · ⋆ ⋆ ⋆`) with
   per-character horizontal-strip warping for the trapezoidal "etched
   along an arc" shape. Wrapped in a `ShaderEffectSource` and fed to
   the shader as a `sampler2D matrixMask` — the shader runs its
   diffraction physics through the alpha mask in the mirror band, so
   the etched text shimmers with the same physics as the data area.
3. **Active overlay Canvas (top)** — Canvas2D for sparse vector ops:
   per-track verify rings (annular arcs colored by status), track
   boundary circles (area-correct `r = √(r_inner² + frac·(r_outer² −
   r_inner²))`), the rotating "drive head" sweep line, and the hub
   trim. Repaints on every sweep tick.

### CD anatomy (per ECMA-130 §8 + IFPI SID Code Guide v2.4)

Normalised to disc radius = 1.0 ≡ 60 mm. From the centre outward:

| Region                | Radius range | Real CD (mm) | Render                                          |
|-----------------------|--------------|--------------|-------------------------------------------------|
| Centre hole           | 0 → 0.125    | 0 → 7.5      | flat black                                      |
| Clear polycarbonate   | 0.125 → 0.275| 7.5 → 16.5   | dark plastic + stacking-ring highlight          |
| Mirror band           | 0.275 → 0.382| 16.5 → 22.9  | silver; iridescent only where matrix mask = 1   |
| Pit region            | 0.382 → 0.967| 22.9 → 58.0  | silver + full iridescence                       |
| Program area subset   | 0.417 → 0.967| 25.0 → 58.0  | where the blue read-fill overlay maps           |
| Outer buffer + rim    | 0.967 → 1.0  | 58.0 → 60.0  | clear plastic (matches inner)                   |

## What's left — the wiring session

Track this list — these are all already wired into the Ripper's signal
shape. Each one is "delete the stub branch, wire the real signal":

1. **Port `runRip` into a `RipWorker` on a `QThread`.** The body of
   `src/cdrip_main.cpp::runRip` is the reference. The `Ripper` becomes
   a thin facade that proxies the worker's queued signals.
   - emit `discChanged()` once TOC + drive info are read
   - emit `discChanged()` once MB lookup resolves
   - emit `progressChanged()` from the read loop with LBA, speed/multiplier, ETA
   - emit `warning()` and update `m_zeroFilledRanges` when a sector is zero-filled
   - emit `tracksChanged()` as encode progresses
2. **AR + CTDB verify** — call into `src/ArVerify.{h,cpp}` after encode
   completes. Update each track's status (`ok` / `warn` / `fail` /
   `unknown`) in `m_tracks`; emit `tracksChanged()`. Build the verify
   summary line and store in `m_verifySummary`.
3. **`RipBatchStore`** — JSON-backed batch persistence at
   `~/Library/Application Support/Concerto/rip_batches/<batch_id>.json`.
   Schema is in `SCANCERTO_PLAN.md` (this file, below).
4. **Save-at-end → playback handoff.** `saveTo(parentUrl, ...)` already
   emits `discSaved(savedPath)` from the stub; the real version
   actually moves the temp dir, writes the batch's
   `saved_path` for this disc, and updates batch state.
5. **Inter-disc + resumable-batch picker UX.** Three branches in
   `startSession(resumeBatchId)`: no disc + no batches → straight to
   WaitingForDisc; no disc + 1 batch → "Resume *X*?" prompt; no disc +
   ≥1 batches → batch picker.
6. **CdShield disc-event hook.** Currently CdShield silently claims
   discs. Add a callback that wakes the Ripper when an audio CD
   appears in any drive — this is what flips WaitingForDisc →
   Identifying for real (rather than the stub's 2-second timer).
7. **Remove the demo-step stub.** All `demoStep` / `demoToggleAutoAdvance`
   plumbing + the `kDemoSteps` table + the Shortcut bindings in
   `qml/RipView.qml`.

### RipBatch JSON schema

```json
{
  "batch_id": "01J...",
  "album_title": "Ravel — Complete Edition",
  "artist": "Maurice Ravel",
  "mb_release_group_id": "abc-...",
  "total_discs": 14,
  "parent_folder": "/Users/thompson/Music/Ravel_Edition",
  "discs": [
    { "position": 1, "mb_disc_id": "...", "status": "done",
      "saved_path": "/Users/thompson/Music/Ravel_Edition/Disc_01" },
    { "position": 2, "mb_disc_id": "...", "status": "done",
      "saved_path": "..." },
    { "position": 3, "status": "pending" }
  ],
  "created_at": "2026-05-12T...",
  "updated_at": "2026-05-15T..."
}
```

Keyed for resume by `mb_release_group_id`. When a disc is identified
during a rip, look up its release_group across `rip_batches/` — if a
batch exists, the disc is a resume candidate; otherwise it's standalone
or the start of a new batch.

## Reference files

- `src/Ripper.h`, `src/Ripper.cpp` — the QObject
- `qml/RipView.qml` — popup layout
- `qml/CdDiscCanvas.qml` — disc graphic composition
- `shaders/cd_disc.frag` — diffraction shader (matches Alan Zucconi
  technique with concerto-specific lighting model)
- `shaders/cd_disc.vert` — passthrough vertex shader; mirror UBO
- `src/main.cpp` — wires `Ripper::discSaved` → `playlist.openFolder` +
  `audio.play`
- `src/cdrip_main.cpp::runRip` — the reference rip flow to port
