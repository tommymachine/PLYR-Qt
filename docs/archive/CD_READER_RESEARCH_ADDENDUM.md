# CD-DA Reader: Research Addendum

Follow-up pass over the same problem space as `CD_READER_RESEARCH.md`, with the goal of pinning down the spots that prior pass had to describe second-hand. Anchored in direct reads of cdparanoia, libcdio, libcdio-paranoia, whipper, and redumper source, plus Apple's `IOCDTypes.h` / `IOCDMedia.h` / `IOCDMediaBSDClient.h` headers, plus Apple's archived "Accessing SCSI Architecture Model Devices" guide.

Clean-room rule: GPL source is fair game for reading and understanding ideas. Nothing here transcribes or closely paraphrases source. Algorithms below are described as ideas; constants are factual. If a future implementer can write fresh code purely from this addendum + the prior report, the constraint is met.

Sections numbered to match the gap list (1-8).

---

## 1. cdparanoia's jitter correction algorithm — full technique

The prior report's section F.2 sketched the technique from external write-ups. Here is the same algorithm derived from a direct read of `paranoia.c` (~2750 lines in original cdparanoia; near-identical in libcdio-paranoia). I describe ideas and constants. No code transcribed.

### Data-structure model

Paranoia maintains three populations of buffered samples, layered:

- **c_block** — a "candidate block." One per low-level read attempt. Holds raw samples, a flag-byte parallel array (per-sample flags: `EDGE` for samples near a read boundary, `UNREAD` for missing/zero-padded, `VERIFIED` for matched), and the LBA the drive said it returned. Many c_blocks coexist in memory; a list maintained in time order.
- **v_fragment** — a "verified fragment." A contiguous subspan of one c_block where the verification pass already proved a match against some other c_block. Possibly jittered — i.e. the LBA the drive claimed may not be the actual LBA on disc.
- **root_block** — a single "verified root." The contiguous, position-accurate output buffer. New audio is appended only after stage-2 merge proves the fragment fits onto the end of root without disagreement.

Read flow at one return-a-sector level: caller asks for sector N → if root already covers it, return slice from root. Otherwise loop: read another c_block from the drive, run stage 1 (match new c_block against earlier c_blocks → create v_fragments), run stage 2 (merge v_fragments into root). Repeat until root covers N or retry budget exhausts.

### Read sizing and jiggling

Each low-level read pulls a multi-sector chunk: `p->cdcache_size` sectors per top-level call, broken into sub-reads of `p->d->nsectors` sectors each. Default `cdcache_size = 1200` (the `CACHEMODEL_SECTORS` constant), broken into sub-reads of typically 27 sectors (the SCSI path caps at 64 KB per ioctl on Linux's SG_IO; macOS/libcdio's analogous limit is 25 sectors). On each sub-read boundary, the parallel flag array marks the `MIN_WORDS_OVERLAP/2 = 32`-word region around the boundary as `EDGE`. The intent: samples right at a sub-read boundary are *less* trustworthy than samples in the middle, because drives sometimes drop or duplicate samples at internal seek boundaries.

The starting LBA of each top-level read is "jiggled" — not simply `cursor`, but `(target & ~(JIGGLE_MODULO-1)) + p->jitter`, where `p->jitter` cycles through 0..14 (`JIGGLE_MODULO=15`). Effect: two successive reads of nominally-the-same sector are aligned to *different* sub-read boundaries. If the drive has a consistent boundary-glitch (drops sample X every time it reads the boundary at LBA Y), the second read's boundary is at a different LBA, so the glitch position changes — and the matched span between the two reads correctly excludes the glitched region.

Drift compensation: `p->dyndrift` adjusts the LBA target by an empirically-tracked offset that accumulates from stage-1 jitter measurements (see below).

### Stage-1 matching (within c_blocks, jitter-tolerant)

For a new c_block, paranoia walks through every prior c_block and looks for matching runs. Walk granularity: stride of `23` words (a prime to avoid resonances with sub-read boundaries) through the overlap region of the two c_blocks' nominal LBA ranges.

At each stride position, a "sort-based" index of one c_block is searched for any sample with the same 16-bit value as the corresponding sample in the other c_block, allowing the search to look up to `p->dynoverlap` words away. (`dynoverlap` is initialized to `MAX_SECTOR_OVERLAP × CD_FRAMEWORDS = 32 × 1176 = 37632 words`, i.e. ~32 sectors — actually the maximum from the start. It only narrows if a user sets it lower; on retry it grows back toward this ceiling.)

When the value matches at some `(posA, posB)`: extend the matched run as far backward and forward as both buffers agree, stopping at any `UNREAD` flag, at any aligned `EDGE` flag where both buffers were at the *same* low-level read boundary (a coincidence the drive may have reproduced), or at the first disagreeing sample. If the extended run is at least `MIN_WORDS_SEARCH = 64` words long (i.e. ~16 stereo samples — short enough to find short distinct snippets, long enough to make coincidental matches astronomically unlikely), record the match.

Match outcome: the relative offset between the two c_blocks at this match is `(posA - cb(A)) - (posB - ib(B))`. That offset is fed into the dyndrift statistics. The matched samples in both c_blocks get the `VERIFIED` flag set in their parallel flag array, but trimmed by `OVERLAP_ADJ = MIN_WORDS_OVERLAP/2 - 1 = 31` words at each end. The trim exists so that two adjacent matched runs only become one continuous verified region if a *third* read confirms the join — never just two.

Once the new c_block has been compared against all prior c_blocks, the contiguous spans of `VERIFIED` samples in it become new v_fragments.

### Stage-2 merging (v_fragments to root, position-determining)

For each new v_fragment, decide if it fits onto the end of root. Search: the fragment's leading non-silent samples are compared to a 256-sample window around the end of root (extended ±`dynoverlap` past the boundary, but capped at 256 — stage 2 is narrowly local). If a long-enough matching run is found, the v_fragment is "anchored" to root: the offset between the v_fragment's claimed LBA and root's actual LBA becomes known. New samples beyond root's current end are appended.

A separate path handles silence. Silence (consecutive samples of value 0, longer than `MIN_SILENCE_BOUNDARY = 1024` words) is treated as "wet" — non-distinctive, can't be position-anchored by content. Stage 2 finds the non-silent island closest to a silent gap, anchors that island, and fills the intervening silent region of root with zeros. The comments in the source describe this in vivid terms ("continents floating on a mantle of molten silence") — non-silent audio is solid and anchors fragments; silence is fluid and fills gaps.

### Boundary cases — first and last samples of a track

Paranoia's domain is the disc-as-stream. Track boundaries are not part of the algorithm — `paranoia_read` returns one sector of verified PCM at a time; the caller (cdparanoia's `main.c`) tracks track boundaries separately. For sample-offset application, `main.c` shifts the TOC's start sectors by `(sample_offset / 588)` whole sectors and skips the fractional `sample_offset % 588` samples on output. The leadout is extended by one sector (`d->disc_toc[d->tracks].dwStartSector++`) when `sample_offset` is non-zero so the libs are willing to read one past the natural last LBA. Lead-in reads are simply attempted; the libs report whatever the drive returns.

### Retry escalation when correlation fails

The retry path is in `paranoia_read_limited`. Default `max_retries = 20` (from `paranoia_read`). Each top-level iteration:

1. Trim root, check fragment cache for what we already have.
2. If root still doesn't cover the requested sector, issue a fresh c_block read.
3. Run stage 1 and stage 2 over what's in memory.
4. Check whether root grew by at least half a sector (588 samples). If yes, the retry counter resets to zero — progress.
5. If root did *not* grow: bump retry counter. Every 5 retries, grow `dynoverlap` by 1.5x (capped at `MAX_SECTOR_OVERLAP × CD_FRAMEWORDS`). If `dynoverlap` is already maxed or retry counter hits `max_retries`, call `verify_skip_case` — which appends one sector of *unverified* data from the c_block cache (preferring the most-recently-seen verified-flagged span if any exists, else zero-fill 2352 bytes) and resets the retry counter to continue.

Final fallback: silent zero-fill of one sector, advance the cursor by one sector, continue. The user-visible callback fires `PARANOIA_CB_SKIP` so cdparanoia's CLI can log "unrecoverable skip at LBA X."

### Notable: cdparanoia does NOT slow the drive on retries

`scsi_set_speed` exists in the SCSI interface but is wired only to the user-facing `-S/--force-cdrom-speed` CLI argument. The retry path in `paranoia.c` does not call it. **Drive speed is whatever the user set, fixed for the whole rip; the retry strategy is purely "read more, jiggle starting LBA, grow correlation window" — not slow-down.** This contradicts the "speed-down ladder" that the prior report listed as cdparanoia behavior; that pattern is EAC-style, not cdparanoia.

### Recommendation

For v1 we don't need any of the above. The two-stage verify-then-merge architecture is a major engineering investment justified by old, unstable drives where jitter is real. Modern Accurate-Stream drives don't jitter, and AccurateRip/CTDB catches the rare cases that slip through. Confirming the prior report's deferral: v1 ships plain sector reads + simple re-read; clean-room reimplementation of the verify/merge stages can come in a v2 if and only if real-world drives in the field show jitter that re-read alone can't catch.

### Caveats

- Future v2 implementer: the `JIGGLE_MODULO=15` value and the prime-23 stride aren't magic — they're empirical anti-resonance constants for *those specific era drives*. Tuning may differ on modern hardware.
- The `MIN_WORDS_SEARCH=64` floor is calibrated so coincidental 16-stereo-sample matches are below noise; under serious noise (heavy scratch with content variation), false matches become more likely. AR/CTDB cross-verify is the ultimate safety net.

---

## 2. cdparanoia's actual retry/error-handling ladder

The prior report's "recommended ladder" (3 retries at full speed, drop to 4x, 5 more retries, zero-fill) was a synthesis. Here's what cdparanoia actually does:

### Trigger conditions

A retry triggers (i.e. the dynoverlap loop iterates again) when root doesn't grow by ≥ 588 samples (half a sector) over an iteration. Specifically:

- **ioctl-level failure**: `cdda_read` returns negative. Treated like a zero-length read except `ENOMEDIUM` (no media) which is fatal-bail. The zero-length region is marked with `FLAGS_UNREAD` so stage 1 won't try to match through it.
- **Short read**: returns fewer sectors than asked. Same handling — zero-pad and mark `UNREAD`. Notable: cdparanoia comments explicitly mention some firmware always short-reads at boundaries (NEC MultiSpeed 4x is cited); cdparanoia keeps the partial data and proceeds rather than retrying the same exact read.
- **Stage 1 found no match**: returned c_block didn't correlate with any prior c_block. Treated as "no progress this iteration."
- **Stage 2 didn't merge**: v_fragments exist but couldn't anchor to root.

The trigger is uniformly "root didn't advance." Errno values aren't dispatched into specific paths.

### Retry budget per "speed level," total cap

There is no per-speed-level subdivision. There is one global retry counter that resets to zero on any forward progress (root advances), and the cap is the `max_retries` argument to `paranoia_read_limited` — defaulting to **20**.

Every 5 retries without progress, `dynoverlap` grows by 1.5x. The "every 5" structure is the closest thing to a tier. Effective behavior on a sector the drive simply can't read cleanly: try 5 reads at initial dynoverlap (already at the 32-sector ceiling), no progress → bump (already capped → no change); try 5 more, no progress → same; on the 20th retry, give up and let `verify_skip_case` fill the sector from whatever is least-bad in the cache.

### Speed-down behavior

**None.** As noted in section 1, cdparanoia does not change drive speed on retry. The prior report's "drop to 4x at retry 4" was made up.

### Final fallback when retries exhausted

`verify_skip_case` does:
1. Walks the cache of c_blocks looking for one that spans the requested LBA.
2. Prefers a span flagged `VERIFIED` (even though not enough other reads confirmed it for root-merge). Otherwise picks the smallest unverified span (least-bad).
3. Appends that span's data to root, OR if no candidate found, appends 2352 bytes of zeros.
4. Fires `PARANOIA_CB_SKIP` callback so cdparanoia's CLI can mark the skip in its rip log.

### "Atomic read" / jitter overlap numbers

There's no explicit "atomic read" concept in the code. The thing that resembles it: `MIN_WORDS_OVERLAP=64`, `MIN_WORDS_SEARCH=64`, `MIN_WORDS_RIFT=16`, `MAX_SECTOR_OVERLAP=32 sectors`. `dynoverlap` is in 16-bit words; converting: initial = 32 sectors × 588 stereo samples × 2 words/sample = 37632 words. That's the sample-position uncertainty window paranoia is willing to consider.

### Recommendation

Adopt a much simpler v1 ladder than the prior report sketched:

1. Issue read CD.
2. On ioctl-level failure: retry the same read up to **N=2** times immediately (small N — most transient errors clear in one retry).
3. On persistent failure: log the LBA range, zero-fill, continue.
4. No speed-down. (If telemetry from real rips later shows speed-down would help, add it then.)
5. No multi-read correlation (jitter correction): deferred per project non-negotiables.

Combined with C2-flag-triggered re-read on a *successful* ioctl that nevertheless reports per-byte errors (see section 5), this gives us a workable error-handling baseline without any of cdparanoia's complexity.

### Caveats

- Some drives stall in firmware after a sense error and need an FD close/reopen to recover. cdparanoia handles this implicitly by issuing many overlapping reads from different starting LBAs; we don't. If we see persistent post-error stalls in testing, add an "if 2 consecutive ioctls fail with EIO, close and reopen the FD before the next read" workaround.

---

## 3. Drive offset application — boundary edge cases

The prior report's section E.3 sketched the math. Whipper delegates entirely to cdparanoia, so I read cdparanoia/libcdio-paranoia's `main.c` for the actual edge-case logic.

### Negative offsets like -670 — does it actually read lead-in?

Yes, with no special-casing. The flow is:

1. Compute `toc_offset = sample_offset / 588` (whole sectors, integer division), `sample_offset %= 588` (the fractional remainder).
2. If `sample_offset < 0`, normalize: `sample_offset += 588; toc_offset -= 1`.
3. Add `toc_offset` to every track's start LBA in the in-memory TOC, *including track 1 and the leadout*.
4. Pass the new LBA to `paranoia_seek`, which calls down to `cdda_read`, which calls down to the low-level platform read function.

For offset = -670 on track 1 (start LBA 0): step 1-2 gives `toc_offset = -2, sample_offset = 506` (since -670 = -2*588 + 506). The TOC's track-1 start moves from 0 to -2. `paranoia_read` cursors from -2 and reads forward; the actual MMC READ CD CDB has LBA = -2 (encoded as 0xFFFFFFFE in unsigned bytes 2-5).

**Drive refusal handling**: there is none explicit. The drive's response shapes the outcome:

- Drive returns success with real data → great, normal flow.
- Drive returns sense error → cdparanoia/libcdio-paranoia treats it as a failed read; the upper layer marks those samples `UNREAD` and continues.
- Drive silently returns zeros → cdparanoia accepts the zero data; if it happens to be in the AR skip region (first 5 sectors / last 5 sectors per track), the AR checksum doesn't care.
- Drive returns cached data from a different LBA → silent corruption. No defense.

The cdparanoia source explicitly does *not* defend against drives that lie about successful reads at negative LBA. The comment chain notes this is a hardware quality issue you can't fix in software.

Redumper *does* model this — its per-drive DB has a `pregap_start` field with the empirically-known lowest LBA each drive can actually read. Modern good drives:
- Most LG/ASUS/LITE-ON: `pregap_start = -135` (can read 135 of the 150 lead-in sectors).
- Most Plextor with D8 command: `-75` (can read 75 sectors of lead-in).
- Many DVD-R/RW: `-135` or `0` depending on model. 
- Cheap slim USB drives: `0` (can't read lead-in at all).
- A few drives: large negative numbers like `-1164` for the AccurateRip-published lowest LBA on a Lite-On LTN483S.

### Positive offsets past leadout — read past, zero-pad, or other?

libcdio-paranoia's `cd-paranoia.c` has explicit branches:

- **Without `--force-overread`** (default): when the rip range would extend past the last track's actual last sector, the rip range is clipped at the last track's natural end, AND a final pass appends `toc_offset × CD_FRAMESIZE_RAW` bytes of silence (calloc'd zeros) to compensate for the missing samples. The user-visible track is the correct length; the trailing samples are just digital silence.
- **With `--force-overread`**: the rip range is extended past the last track's natural end (TOC's leadout is bumped by 1 sector earlier in the flow to allow this). cdparanoia issues a read at the post-leadout LBA. If the drive cooperates (good Plextor reads ~100 lead-out sectors; good MTK-based drives can read ~75; many cheap drives refuse) you get real data; otherwise the drive returns silence or sense-errors and the rip-flow fallback kicks in.

Most modern audio rippers default to the zero-pad path because (a) it's safe, (b) the AR/CTDB skip-the-last-5-sectors region absorbs the difference, and (c) the user's drive may not support overread anyway.

### The "5-sector skip" in AR's checksum — designed for this boundary or different?

This was a design question — the answer is yes, exactly for this. The AccurateRip v1/v2 checksum explicitly skips the first 5 sectors of track 1 and the last 5 sectors of the last track (2940 stereo samples each side). Hydrogenaudio's AR docs are consistent on this. The skip range is sized so that whether your drive can or can't read lead-in/lead-out, and whether your offset shift is up to ±2940 samples (about ±5 sectors), the checksum-computed region is the same across all drives that get the canonical region right. So the AR-DB matches against the user's rip even when the user's drive zero-padded the boundary regions. **This is exactly why the rip can be "AR-accurate" without successfully reading lead-in/lead-out.**

### Non-sector-aligned offsets — confirmation of canonical approach

Read whole sectors, slice samples afterward. cdparanoia and every other ripper do this because MMC READ CD's transfer unit IS the sector; there's no way to ask for a sub-sector range. The math:

- For canonical track region `[start_sample, end_sample)` and drive offset `+O` (positive means drive reads "O samples earlier than asked"):
  - To get sample N, ask the drive for the sector containing sample N+O.
  - Read sectors covering `[ceil((start_sample + O)/588) - 1, ceil((end_sample + O)/588))` (with -1 on the start for the partial-sector boundary on the leading edge if `O` isn't a multiple of 588).
  - In the returned buffer, output starts at byte `((start_sample + O) % 588) × 4`.

Cdparanoia's expression of this: keep an `offset_buffer[1176]` (one sector's worth of words = 4×588 = 2352 bytes), an `offset_buffer_used` byte count, and an `offset_skip` byte count. The first sector you write to the output, skip `offset_skip` bytes off the front. For each subsequent sector boundary, save the trailing `sample_offset × 4` bytes in offset_buffer for the next batch (`offset_buffer_used = sample_offset × 4`), prepend that into the output before the next normal read. At the end of the rip, flush whatever's still in `offset_buffer`.

The Concerto implementation can be simpler since we control the whole pipeline: read the whole continuous range as one big PCM buffer (with one extra sector front and back if offset is non-zero), then slice each track's canonical region from the appropriate sample offset into that buffer.

### Recommendation for v1

1. Apply offset at read time, as the prior plan said. Use the formula above; read whole sectors and slice.
2. For negative offsets shifting track 1 below LBA 0: try the read, and on sense-error fall back to zero-pad. Don't treat the failure as fatal; AR skip absorbs it.
3. For positive offsets that would shift the last track past natural leadout: zero-pad by default. Don't issue overread reads unless we add a drive-quirks DB later that says "this drive can overread N sectors."
4. Don't add `force-overread`-equivalent in v1.

### Caveats

- A drive that silently returns cached/garbage data on negative-LBA reads is undetectable from software at the read level. Defense is AR/CTDB verification post-rip — if the rip can't verify at any offset, the rip is broken.
- Hidden Track One Audio (HTOA) — audio in the lead-in pregap before "track 1 sample 0" — exists on some discs. Reading it requires the drive to be able to read negative LBAs. We don't support HTOA in v1; out of scope.

---

## 4. Drive-quirk DB

The prior report's reference here was vague. Here's the actual landscape across cdparanoia / libcdio / redumper.

### cdparanoia's quirks DB

Three small tables in `interface/drive_exceptions.h`, ~15 entries total:

- `atapi_list[]` — drives that need their ATAPI/SCSI detection forced. Four entries (Samsung SCR-830, Memorex CR-622, Sony CDU-561, Chinon CDS-525).
- `mmc_list[]` — drives that need a non-default MMC read variant. Five entries, mostly overlapping the ATAPI list, plus Kenwood UCR (needs the `D8` non-standard read command).
- `scsi_list[]` — drives that need pre-MMC SCSI read commands. ~15 entries covering Toshiba, IBM, DEC, IMS, Kodak, Ricoh, HP, Philips, Plasmon, Grundig, Mitsumi, Kenwood, Yamaha, Plextor, Sony, NEC, Matsushita.

Each entry can override: ATAPI/SCSI mode, mode-sense density byte, an "enable CDDA" hook, a low-level read function pointer, and a big-endian flag. **All these quirks address "which SCSI/MMC opcode to use to read CDDA at all" — they're for drives so old (1990s) they don't speak modern MMC.** Modern USB drives don't need entries here.

The "lies about caching" quirk is handled empirically by `cachetest.c`, not via a static list — every drive gets measured. cdparanoia comments that there's no MMC way to ask whether a drive caches CDDA reads, so the only option is to detect it at runtime.

### redumper's quirks DB

Much richer. Each entry has: vendor, product, revision, vendor_specific, reserved5, plus `read_offset` (sample-level), `c2_shift` (how many sample positions the C2 byte stream lags the data — typically 0 or 294 depending on drive), `pregap_start` (lowest LBA readable, see section 3), `read_method` (`BE`=READ_CD 0xBE vs `D8`=non-standard Plextor command), `sector_order` (`DATA_C2_SUB`/`DATA_SUB_C2`/`DATA_SUB`/`DATA_C2` — order of the response payload), and `type` (chipset family — PLEXTOR, MTK8A/8B/8C, MTK3, MTK2/2B). The DB has ~70 entries, partitioned into recommended (above a sentinel row) and generic.

### Categories cdparanoia cares about (not relevant to v1):
- Pre-MMC SCSI READ variants (D8, D4_10, D4_12, etc.).
- Endianness of returned data (some old SCSI drives returned big-endian).
- Lies-about-caching (handled empirically not statically).

### Categories redumper cares about:
- Read method (BE vs D8) — only matters if you want to access lead-in via D8.
- Sector order in response — only matters if you're requesting C2 + subcode alongside user data.
- C2 shift — only matters if you're using C2 *and* the drive's C2 is misaligned.
- Pregap_start — empirical lowest LBA the drive can read.
- Read_offset — sample-level offset. Same data as AR's drive offset DB.

### What `CdDevice` should care about for v1

Almost none of it. For v1:
- **Read offset only**: bundle the AR drive offset DB as `drive_offsets.json`. Look up by (vendor, product); on hit, apply offset at read time. On miss, log warning and default to 0 (the AR/CTDB offset scan handles post-hoc).

For v2+:
- **Sector order and C2 shift**: only relevant if we add C2-triggered re-read AND we hit drives where C2 is misordered. The macOS BSD ioctl path returns a fixed layout (UserData then ErrorFlags in the per-sector area encoding defined by sectorArea bitmask) so the C2 shift question is moot on macOS — Apple's kernel side handles the per-drive translation. Linux/Windows raw SG_IO/SPTI would need to care.
- **Pregap_start**: defer. The naive "try and zero-pad on failure" approach in section 3 is fine; we don't need to predict whether the drive can read lead-in.

### Comparison to Aaru's drive DB

Aaru (the .NET re-imagining of redumper-adjacent ideas) has a similarly rich DB but is GPL-3. Don't link or transcribe; the conceptual schema is the same as redumper's.

### Recommendation

For v1, ship only `drive_offsets.json` (sample offset only). Don't ship a quirks DB. Adding more drive-specific data later is straightforward — the AR offset DB sits in one CSV table; adding a sibling `drive_quirks.json` keyed the same way is a future task.

### Caveats

- USB bridge chipsets mask the underlying drive's INQUIRY response on some external drives. In that case the AR DB lookup matches "JMicron" (or whichever bridge) instead of "PIONEER" (or whichever real drive). The result is no DB hit and a default offset of 0. Subsequent AR offset-scan rescues the rip but the user-visible offset is wrong on first pass — they'd need to re-rip with the corrected value. This is unavoidable without parsing through USB bridges' MODE SENSE responses (out of scope).

---

## 5. MMC READ CD CDB layout — verified against production code

Section D.1 of the prior report gave the CDB layout. Verified against libcdio's `mmc_read_cd` function and cdparanoia's `i_read_mmc*` variants. The byte-by-byte interpretation is correct, with one important nuance.

### The "byte 1" interpretation (most important nuance)

Per the MMC-5 spec, byte 1 has these fields (LSB first):

- bit 0: RELADR — obsolete in MMC-3+; relative-addressing flag from SCSI history.
- bit 1: **DAP** ("Digital Audio Play") — *not* "reserved." When set, the drive applies its analog volume/mute settings to the audio data being returned. For read-to-host operations most drives ignore it; for read-to-DAC playback it matters. Apple's IOKit framework's `kCDSectorTypeCDDA` request, libcdio's `b_digital_audio_play` parameter, and cdparanoia's `i_read_mmc3` family all set this bit.
- bits 2:4: Expected Sector Type — 0 (Any), 1 (CDDA), 2 (Mode1), 3 (Mode2), 4 (Mode2Form1), 5 (Mode2Form2).
- bits 5:7: LUN — usually 0.

So:
- `Cdb[1] = 0x04` = sector_type=1 (CDDA), DAP=0, RELADR=0. **This is the "strict, spec-clean" value the prior research suggested.**
- `Cdb[1] = 0x06` = sector_type=1 (CDDA), DAP=1, RELADR=0. **This is what cdparanoia's `i_read_mmc3` and libcdio's `mmc_read_cd` with `b_digital_audio_play=true` actually send.** Empirically known to work better on a wider range of drives — some drives need DAP set to honor the audio-mute-defaults-off state.
- `Cdb[1] = 0x02` = sector_type=0 (Any), DAP=1, RELADR=0. cdparanoia's `i_read_mmc` (default for many drive families). Drives that can't enforce "this is CDDA" on data tracks (mixed-mode handling weaker than spec) still return 2352-byte CDDA-style payloads with this.

**The prior report's bit 1 = "RELADR" annotation was a misread of the MMC spec.** Bit 0 is RELADR; bit 1 is DAP. Functionally for our case, both bits set or unset is fine on modern drives; the safe default is `0x04` (CDDA, DAP=0).

### CDB[9] = 0x12 for "UserData + ErrorFlags" — confirm

Per MMC-5 byte 9:
- bit 7: Sync (12 bytes; CDDA returns 0)
- bits 6:5: Header Code (00=none for CDDA)
- bit 4: User Data (1 = include user data; the 2352 CDDA bytes)
- bit 3: EDC/ECC (data sectors only; CDDA ignores)
- bit 2: reserved
- **bit 1: Error Flags (1 = include the 294-byte C2 mask after user data)**
- bit 0: reserved

So `Cdb[9] = 0x10` = UserData only (no C2). `Cdb[9] = 0x12` = UserData + ErrorFlags. **Confirmed against libcdio's `mmc_read_cd`:**

```
if (b_user_data) cdb9 |=  16;        # 0x10 → bit 4
cdb9 |= (c2_error_information & 3) << 1;   # 0x02 (C2=1) or 0x04 (C2=2 with BEB)
```

So `0x12 = 0x10 | 0x02` is correct. The MMC error-flags field is actually 2 bits wide:
- `00` = no C2.
- `01` = C2 only (294 bytes).
- `10` = C2 + Block Error Byte (296 bytes — adds 2 bytes of summary flags).
- `11` = reserved.

The prior report's "0x12" is C2-only. For v1 use that. C2_BEB (`0x14`) gives 2 extra summary bytes that aren't needed.

### CDB[10] = subchannel selection

Per MMC-5:
- `0x00` = no subchannel.
- `0x01` = raw P-W subchannel (96 bytes).
- `0x02` = formatted Q-subchannel (16 bytes — useful for ISRC/index tracking).
- `0x04` = corrected/de-interleaved R-W subchannel (96 bytes).
- Others reserved.

For v1 (no ISRC/index reads): `Cdb[10] = 0x00`. Libcdio's `mmc_read_cd` parameter `subchannel_selection` exposes this; the default is 0.

### Production-grade tweaks that work better than spec-minimum

From comparing cdparanoia and libcdio:

- **DAP=1 (set bit 1 in `Cdb[1]`)** — empirically more compatible. Libcdio's `mmc_read_cd` exposes this as a parameter; cdparanoia's mmc3 variant sets it.
- **Use `0xBE` (READ CD), not `0x28` (READ 10)** — READ 10 returns 2048-byte ROM blocks on some drives even when the track is CDDA. Spec-confused-firmware. cdparanoia tries `0xBE` first.
- **Use LBA addressing, not MSF** — `0xBE` (LBA) is preferred over `0xB9` (READ CD MSF). MSF only useful if you're addressing in MSF coordinates anyway.

### macOS BSD ioctl mapping

`DKIOCCDREAD` with `sectorArea = kCDSectorAreaUser | kCDSectorAreaErrorFlags = 0x12`, `sectorType = kCDSectorTypeCDDA = 0x01`. The kernel translates to MMC `READ CD (0xBE)` with `Cdb[1]` set to the appropriate CDDA bits (Apple's kernel decides whether to set DAP; we don't have to). The `offset` field of `dk_cd_read_t` is in **bytes from disc start**, computed as `lba × per_sector_total_size` — i.e. `lba × 2646` when sectorArea=0x12. Apple's `IOCDMedia.h` doc string is explicit: "The sum of each area's size defines the natural block size of the media for the call. This should be taken into account when computing the address of byteStart."

### Recommendation

For v1 macOS:
- `cd_read.sectorArea = kCDSectorAreaUser` only (= 0x10, 2352 bytes/sector). Don't request C2 yet.
- `cd_read.sectorType = kCDSectorTypeCDDA`.
- `cd_read.offset = lba × 2352`.
- `cd_read.bufferLength = count × 2352`.

For v1.1 (add C2-triggered re-read once basic flow proven):
- `cd_read.sectorArea = kCDSectorAreaUser | kCDSectorAreaErrorFlags` (= 0x12, 2646 bytes/sector).
- `cd_read.offset = lba × 2646`.
- After read: scan the 294-byte C2 block per sector; on any non-zero byte, re-read once at same speed.

The change between v1 and v1.1 is a 4-line edit in `CdDevice::readSectors`. No reason to require both at once.

### Caveats

- C2 reporting accuracy varies wildly: some drives never report C2 (firmware bug). Some report C2 on every read regardless of disc condition (firmware bug, the other way). Don't treat "C2 clean" as proof of correctness; treat "C2 dirty" as a strong re-read trigger.
- The 294-byte C2 layout: bit `(7 - n%8)` of byte `(n/8)` flags user-data byte `n`. So all-zero C2 means no flagged errors; any non-zero byte means somewhere in this sector at least one byte was uncorrectable.

---

## 6. Cross-platform abstraction shape — what libcdio actually does

The prior report's `CdDevice` sketch (section H) is a clean shape. Comparing to libcdio reveals what to keep and what to skip.

### libcdio's `cdio_funcs_t` shape

A C struct of ~30 function pointers, defined in `lib/driver/cdio_private.h`. Each driver (macOS, Linux, Windows ASPI, Windows ioctl, FreeBSD, NetBSD, Solaris, AIX, plus image-file drivers for ISO, BIN/CUE, NRG) implements the relevant subset. The interface includes:

- Discovery: `get_devices`, `get_default_device`.
- Open/close: `set_arg("source", ...)`, `free`.
- Disc-level: `get_discmode`, `get_drive_cap`, `get_media_changed`, `get_mcn`, `get_first_track_num`, `get_num_tracks`, `read_toc`, `set_blocksize`, `set_speed`.
- Track-level: `get_track_lba`, `get_track_msf`, `get_track_pregap_lba`, `get_track_isrc`, `get_track_format`, `get_track_green`, `get_track_copy_permit`, `get_track_preemphasis`, `get_track_channels`.
- Reads: `read` (lseek-style stream read), `read_audio_sectors`, `read_data_sectors`, `read_mode1_sector(s)`, `read_mode2_sector(s)`.
- Audio control (legacy, for CD-as-analog-playback): `audio_get_volume`, `audio_set_volume`, `audio_pause`, `audio_play_msf`, `audio_play_track_index`, `audio_resume`, `audio_stop`, `audio_read_subchannel`.
- Misc: `eject_media`, `get_cdtext`, `get_cdtext_raw`, `lseek`, `run_mmc_cmd` (escape hatch for arbitrary MMC).

### What's good

- One function pointer per platform-distinct operation. Trivial to add a platform by implementing only the relevant pointers.
- `run_mmc_cmd` escape hatch — drivers that can pass arbitrary MMC commands expose it; drivers that can't return `DRIVER_OP_UNSUPPORTED`.
- Read functions are split by sector type (Mode 1, Mode 2, audio). That maps cleanly to the underlying MMC `READ CD` `sectorType` field.
- Driver-agnostic dispatch — the public `cdio_*` functions just look up the pointer and call it. Almost zero glue code.

### What's bad

- The audio-control surface is dead weight. CD-as-analog-output is a 1990s mode. Modern apps decode audio data to PCM and play it through OS audio. We won't ship a `play_track` button that streams audio analog-out from the drive.
- ~30 pointers means ~30 stubs to implement for each new platform, even if half return "unsupported."
- `read_data_sectors` / `read_mode2_sectors` / etc. are needed by libcdio's broader scope (ISO9660 reading, mixed-mode discs) but not by us.
- Subchannel data (`get_track_isrc`, `audio_read_subchannel`, `get_mcn`) — out of scope for v1, would be on the v2 list.
- `lseek` / `read` stream interface is for filesystem-shaped consumers; we don't need it.
- `set_blocksize` is an MMC primitive we don't need — we always read whole sectors.

### Where libcdio over-generalizes

- All "raw data sector" read variants for getting at Mode 1/2/2-Form-1/2 with different combinations of sync header + ECC bytes. We only care about CDDA.
- All disc-write paths (`writeCD`). Read-only library would have a tighter footprint.

### Where it under-specifies

- No retry semantics. Each driver returns success/failure per call; orchestration (re-read, log, give up) is the caller's job. This is consistent with what we want — keep `CdDevice` as a stateless transactional layer and put retry orchestration in `Ripper`.
- No C2 hint propagation. libcdio's audio read on macOS doesn't request C2 even though it could; the C2 bits are silently dropped. To use C2 we'd need to extend the function signature.
- No "what's the read offset" hook. libcdio doesn't know about per-drive read offsets; that's the consumer's problem.

### Proposed updates to the `CdDevice` sketch in the prior report

Keep:
- The 5-function shape (enumerate / open / readToc / readSectors / set speed / read isrc).
- `int32_t lba` (signed) for negative LBAs.
- `wantC2` flag and the `c2` byte array in `ReadResult`.
- `ReadStatus` enum for `Ok/TransientBusy/MediumError/OutOfRange/Aborted/FatalDeviceError`.

Add:
- `optional<DriveQuirks> quirks()` returning vendor-derived hints (initially: only the AR-DB offset; later: pregap_start if we add it).
- `lastDeviceError() -> std::string` for diagnostic logging — translated platform-specific (errno on macOS BSD ioctl, sense-key+ASC+ASCQ on Linux SG_IO / Windows SPTI).
- A `cancellation_token` on `readSectors` (or a sibling `cancel()` method that closes the FD from another thread) — long rips need cancellation. The BSD ioctl is blocking; closing the FD from another thread interrupts.

Remove:
- The per-call `audio` parameter as `std::span<uint8_t>`. Switch to `std::vector<uint8_t>` for ownership clarity; or keep span but document that the caller owns the buffer. Either works.

Skip (defer):
- ISRC, MCN, subchannel reads.
- CD-TEXT.
- Mode 1/2 data sector reads.
- Multi-session disc info.
- `audio_*` analog playback API.
- `eject` (we can call `drutil eject` as a subprocess from the GUI; ejecting via SCSI-passthrough is overkill).
- `lseek`/`read` stream API.

### Recommendation

Adopt the prior report's API shape with the small additions above. Keep the platform-private implementation in `src/CdDevice_macos.cpp` initially; add `src/CdDevice_linux.cpp`, `src/CdDevice_windows.cpp` later. The interface header (~50 lines) stays platform-agnostic.

### Caveats

- libcdio's history shows the API outlasted use cases by 25 years. Aim for a small, focused API and accept that we might add 1-2 more functions later. Don't try to predict every future use case.

---

## 7. Out-of-range LBA behavior

What drives actually do on out-of-range reads, per cdparanoia comments and redumper README:

### Fail with sense data

The "well-behaved" response. Sense key 0x03 (Medium Error) or 0x05 (Illegal Request, ASC 0x21 = LBA out of range). Software detects, marks the sector unreadable, moves on.

### Zero-pad

Drive returns success with all-zero data. The drive's firmware decided this was easier than reporting an error. Software can't distinguish from "this sector actually was silence" without external verification.

### Return actual lead-in/lead-out data

Plextor drives via the `0xD8` command read lead-in cleanly. Some good Plextor drives via `0xBE` also expose ~75 sectors of lead-in. Most modern LG/ASUS/LITE-ON drives read ~135 sectors of pregap via `0xBE` (sense field reports success). Very few cheap drives.

### Return cached/stale data

The bad case. Drive caches the most-recent successful read and returns it for any subsequent request. cdparanoia's `cachetest.c` exists specifically because no MMC command lets you ask whether a drive caches. The empirical detection: read sector A, then read sector A+10000 (long seek that should bust any cache), then re-read sector A, time the second read. If it's fast (cached), the drive caches; if slow (re-read from disc), it doesn't.

### Hang the drive

A few drives lock up entirely on negative-LBA reads. Power cycle required. Redumper documents this for specific Mediatek-based firmware in its drive-test guide.

### Concrete examples in source

cdparanoia source comments (`paranoia.c` near line 2426): "There are drives on which you will never get a full read in some positions. They always abort out early due to firmware boundary cases. Reread will cause exactly the same thing to happen again. NEC MultiSpeed 4x is one such drive. In these cases, you take what part of the read you know is good, and you get substantially better performance. --Monty"

redumper drive DB:
- `LITE-ON LTN483S 48x Max` rev `PD03` — read offset `-1164`, *zero* pregap. The drive can't read negative LBAs at all but its offset is so negative the rip would always need lead-in. Comment in `drive.ixx` lists this as a "bad" drive for that reason.
- `PIONEER BD-RW BDR-209D` rev `1.10` — read offset `+667`, zero pregap. Large positive offset means it needs lead-out overread to get final samples; doesn't have it.
- `PLEXTOR DVDR PX-740A` rev `1.02` — comment: "doesn't stop on lead-out but always returns same sector." That's the "cached/stale data" case explicit.

### Recommendation for v1

- Treat any successful but suspicious read (negative LBA, post-leadout LBA) as data we have to trust the AR/CTDB cross-check to validate.
- Don't try to detect cached-data drives at runtime. cdparanoia spends ~600 lines of `cachetest.c` on this; we don't need to.
- On sense-error or short-read at boundary LBAs, fall back to zero-pad without alarm.

### Caveats

- The "cached data" case is undetectable without something cachetest-like. For v1 we accept this risk — if the user's drive has this bug, AR verification will catch it (the rip won't match anywhere) and they can try another drive.

---

## 8. SCSITaskUserClient — second look

The prior report concluded "stay off SCSITaskUserClient for v1." That's the right *practical* conclusion, but the *reasoning* was over-cautious.

### What the BSD ioctl path can't do (in theory)

- Send arbitrary MMC commands (vendor-specific opcodes like `0xD8` Plextor read, mode-sense pages beyond what the kernel exposes, mode-select to disable read-ahead).
- Issue raw SCSI INQUIRY (we read the parsed-INQUIRY result through IORegistry `Device Characteristics` dict instead).
- Send `SET STREAMING` (`0xB6`) for explicit read-cache control.

### What SCSITaskUserClient gives you that BSD ioctls don't

The full set above. Plus: arbitrary CDB construction with full sense-data return. Plus: drive-specific command support (Plextor `0xD8` for clean lead-in, drive-test commands, firmware revision via `INQUIRY` VPD pages, etc.).

### What it actually costs to use

Apple's archived "Accessing SCSI Architecture Model Devices" doc is explicit: storage devices (peripheral types 0x00, 0x07, 0x0E) are gated to in-kernel drivers only. **Optical drives (peripheral type 0x05) are explicitly exempted** — Apple's own design intends for user-space apps to access them via `MMCDeviceInterface → SCSITaskDeviceInterface`. The rendezvous mechanism: `IOServiceMatching` with `kIOPropertySCSITaskDeviceCategory == kIOPropertySCSITaskAuthoringDevice` (or `kIOPropertySCSITaskUserClientDevice` for non-authoring).

### Does it work on signed/notarized apps in 2026?

libcdio's macOS driver still has working SCSITaskUserClient code (under `GET_SCSI_FIXED` ifdef; the default for fresh builds keeps it disabled, but the codepath compiles and works). Apps that have shipped using it in modern macOS (Disco / Burn / various legacy CD authoring tools) suggest it works with normal Developer ID signing. No DriverKit entitlement is required for the optical-drive case — that's only for *new* drivers (kernel drivers that talk to *new* devices in the SCSI family). User-space apps consuming the pre-existing IOSCSIArchitectureModelFamily interfaces have not been gated.

Apple Developer Forums thread 650611 (June 2020): an Apple Systems Engineer confirmed "There is no support for IOSCSIArchitectureModelFamily classes like IOSCSIProtocolInterface in DriverKit yet. Support will be added in a future release." That's about *driver development*, not user-space access. The user-space `SCSITaskDeviceInterface` path is still alive.

### Updated conclusion

The prior report's "skip SCSITaskUserClient" stands, but for a different reason: **we don't need any of the capabilities it adds.** BSD ioctls cover:
- `DKIOCCDREAD` → MMC `READ CD` (0xBE) — does CDDA reads with optional C2 and subchannel.
- `DKIOCCDREADTOC` → MMC `READ TOC` (0x43).
- `DKIOCCDSETSPEED` → MMC `SET CD SPEED` (0xBB).
- `DKIOCCDREADISRC` / `DKIOCCDREADMCN` — track ISRC and disc MCN (we don't need for v1).
- `DKIOCCDREADDISCINFO` / `DKIOCCDREADTRACKINFO` — disc and track info (we don't need; the TOC gives everything for audio CD).

Vendor/product → IORegistry `Device Characteristics` dict (parsed-INQUIRY result, no raw SCSI needed).

We never need Plextor `0xD8` (deferred lead-in reads), MODE SENSE beyond defaults (deferred read-cache disabling), or any vendor-specific commands. So **SCSITaskUserClient is correctly not in v1's path**, but the prior report's "Apple gates it on entitlements" was the wrong reason — it's actually accessible without entitlements for optical drives. The right reason is "BSD ioctls cover every command we need, so the simpler path wins."

### Recommendation

Stick with BSD ioctls for v1. Update the prior report's caveat: if a future feature needs vendor-specific MMC (e.g. clean lead-in reads via Plextor D8 for HTOA support), SCSITaskUserClient is available without entitlements; budget it for ~1 week of plumbing (the `MMCDeviceInterface → SCSITaskDeviceInterface` rendezvous + `ExecuteTaskSync` shape is well-documented).

### Caveats

- Apple may eventually deprecate user-space `SCSITaskUserClient` in favor of a DriverKit replacement. No evidence of this happening yet as of 2026. If/when it does, we'd need a kernel-bridge component. The BSD ioctl path is the safest long-term bet because it's the documented public API.
- Empirically: libcdio has had this code path for ~20 years and it still works on Apple Silicon Macs running macOS 14.x. Not gathering dust.

---

## Delta summary

Punch list of what's now known more precisely than `CD_READER_RESEARCH.md` said:

- **cdparanoia retry budget**: default is **20 retries total per sector**, not the "3 at full speed, 5 at 4x" the prior report listed. Source: `paranoia_read` default `max_retries=20`.
- **cdparanoia speed-down on retries**: **doesn't happen**. The retry path grows correlation window and jiggles read alignment; it doesn't call `set_speed`. The prior report's speed-down ladder was extrapolation from EAC, not cdparanoia.
- **Jitter overlap initial value**: `dynoverlap` starts at the maximum `MAX_SECTOR_OVERLAP × CD_FRAMEWORDS = 32 sectors × 1176 words = 37632 words`. The 1.5x growth on retry has no effect at start (already capped) — it only matters if a user manually lowered it via `paranoia_overlapset`.
- **cdparanoia read sizing**: default `cdcache_size = 1200 sectors` per top-level call, split into `nsectors = 27` sectors per sub-read (Linux 64KB cap → 27 sectors at 2352 bytes; macOS libcdio caps at 25 sectors).
- **The "every 5 retries" pattern**: dynoverlap grows by 1.5x every 5 retries, not every retry.
- **Sample-domain matching constants**: `MIN_WORDS_SEARCH=64` (minimum match length), `MIN_WORDS_OVERLAP=64`, `MIN_WORDS_RIFT=16`, `MIN_SILENCE_BOUNDARY=1024`, `JIGGLE_MODULO=15`, prime-23 stride.
- **MMC CDB[1] bit 1**: it's **DAP (Digital Audio Play)**, not RELADR. RELADR is bit 0. Functionally `0x04` (CDDA, DAP=0) and `0x06` (CDDA, DAP=1) both work; libcdio defaults to DAP=1 in `mmc_read_cd` when `b_digital_audio_play` is set.
- **MMC CDB[9] for User+C2**: confirmed `0x12 = bit 4 (UserData) | bit 1 (C2 single-byte mask)`. Bits are 2-wide for C2: `00`=none, `01`=294-byte C2, `10`=296-byte C2+BEB. We want `01`.
- **macOS `dk_cd_read_t.offset`**: in **bytes from disc start**, computed as `lba × per_sector_total_size`. When `sectorArea = 0x12` (UserData + ErrorFlags), per-sector size is 2646 bytes, so `offset = lba × 2646`. Confirmed against Apple's `IOCDMedia.h` doc string and libcdio's audio read.
- **IORegistry property keys** for drive vendor/product: confirmed as `"Vendor Name"` / `"Product Name"` in the `"Device Characteristics"` dictionary on a parent IORegistry entry. Constants `kIOPropertyVendorNameKey` / `kIOPropertyProductNameKey` / `kIOPropertyDeviceCharacteristicsKey`.
- **macOS drive walk**: libcdio uses `IOServiceMatching("IOCDBlockStorageDevice")` to enumerate drive nubs, then path-prefix-match against the IOCDMedia path to find the right one. Different from "walk up parents recursively" the prior report suggested. Either works; the libcdio approach is more direct.
- **Lead-in/lead-out range per drive type**: pregap_start values from redumper's DB — modern LG/ASUS/LITE-ON drives `pregap_start = -135` (can read 135 of 150 lead-in sectors); Plextor with D8 `-75`; many slim USB drives `0` (no lead-in). Lead-out overread: good Plextor ~100 sectors; good MTK ~75; most drives 0. Prior report's "0-150 sectors" range too vague.
- **cdparanoia drive-quirks DB**: only ~15 entries, all about which SCSI/MMC opcode the drive accepts (D4/D8/A8/28/etc). The "lies about caching" quirk is **empirical, not statically listed** — handled by runtime `cachetest`. Modern USB drives don't appear in cdparanoia's quirks DB.
- **redumper's drive quirks DB**: ~70 entries with `read_offset`, `c2_shift`, `pregap_start`, `read_method` (BE vs D8), `sector_order` (DATA_C2_SUB vs DATA_SUB_C2 vs DATA_SUB vs DATA_C2), and `type` (chipset family). Much richer than cdparanoia's; for v1 we still only need read_offset.
- **whipper's offset handling**: it delegates entirely to cdparanoia (`--sample-offset=N --force-overread`). No additional logic.
- **lead-out zero-pad behavior**: libcdio-paranoia's CLI explicitly zero-fills `toc_offset × CD_FRAMESIZE_RAW` trailing bytes when ripping without `--force-overread`. The AR skip region (last 5 sectors) absorbs this so AR verification doesn't care.
- **lead-in handling**: there's no special handling. cdparanoia just tries the negative-LBA read; outcome is drive-dependent. Recommended pattern: zero-pad on sense-error, trust AR skip region to absorb.
- **Apple's SCSITaskUserClient stance**: explicitly *permits* user-space access for optical drives (peripheral type 0x05) without entitlements via the `MMCDeviceInterface → SCSITaskDeviceInterface` rendezvous. The prior report's "skip it because entitlements are gated" reasoning was inaccurate; the correct reason for skipping is "BSD ioctls cover what we need."
- **cdparanoia DOES NOT request C2 by default**: cdparanoia's READ CD CDBs use `CDB[9] = 0x10` (UserData only). C2 use is a separate codepath. So cdparanoia-the-original is a worse reference for C2 handling than libcdio's `mmc_read_cd` (which parameterizes C2 cleanly).
- **macOS BSD ioctls returning C2**: the headers expose `kCDSectorAreaErrorFlags`, but neither libcdio nor any other GPL macOS code in the survey actually exercises this path on macOS via BSD ioctl. So while structurally correct, we don't have a production-code datapoint that it works end-to-end on macOS via DKIOCCDREAD. Worth testing on real hardware before relying on it.
- **`/dev/rdisk` rationale**: confirmed by libcdio's comment ("Below, by appending 'r' to the BSD node name, we indicate a raw disk. Raw disks receive I/O requests directly and don't go through a buffer cache.").
- **libcdio audio sectors per read**: cap of **25 sectors** per low-level call on Linux MMC path. cdparanoia caps at 27 sectors (64KB/2352 ≈ 27.2). The relevant constraint is the drive's max-transfer length, typically 64KB on USB.

---

## Updated implementer note

The findings here are **additive only — the original `CD_READER_RESEARCH.md` should NOT be rewritten.** The prior report's recommendations (BSD ioctls, IORegistry walk for vendor/product, deferred SCSITaskUserClient, drive-offset DB bundled as JSON, no jitter correction in v1) all remain correct. The corrections above sharpen specifics — corrected `Cdb[1]` bit interpretation, replaced the made-up retry ladder with cdparanoia's actual 20-retry-cap behavior, replaced the made-up speed-down with "no speed-down," clarified lead-in/lead-out boundary handling — but none of them flip a v1 decision.

The right way to consume this addendum: read the prior report for the architecture decisions, read this addendum for the implementation details when writing `CdDevice` and the retry path. If conflicts appear, this addendum's values are sourced from actual production code and supersede.

One specific edit that would be useful in the prior report's "Concrete proposed edits" section: change the proposed v1 retry ladder from "3 retries at full speed, 5 retries at 4x" to "2 retries at full speed, log+zero-fill on persistent failure; no speed-down in v1; add speed-down as a knob in v1.1 if telemetry shows it helps." But this is a refinement, not a contradiction — the prior report was already explicit that the ladder was a recommendation, not derived from cdparanoia.
