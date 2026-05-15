# PLYR-Qt Native CD Reader (Task 3)

The plan for the only remaining piece of the CD-rip stack: actually
reading audio off a CD via native macOS APIs. Everything else around it
already works (see CDRIP_STRATEGY.md), so this doc is scoped just to the
reader and the bits that depend on a real drive being present.

If you're picking this up cold: read this doc top to bottom once, then
treat the "Non-negotiables" section as settled. Don't relitigate
licensing or the cdparanoia decision — both already happened.

---

## North star

Insert a CD, run one binary, get out a folder of correctly-tagged,
AccurateRip + CTDB verified FLAC files. No Homebrew, no Python, no
`cdparanoia` subprocess — everything inside the Qt app bundle.

## What you can lean on

The rest of the pipeline is built and validated against the
Rachmaninoff 10-disc set:

- **TOC + disc-ID math** — `src/ArVerify.{h,cpp}`. Reconstructs a TOC
  from per-track sample counts; produces AR id1/id2, CDDB id,
  MusicBrainz disc id, CTDB TOC string. Pure computation.
- **FLAC decode/encode** — `src/FlacDecode.{h,cpp}`,
  `src/FlacEncode.{h,cpp}`. CD-DA only (44.1k/16/stereo). Encoder
  writes Vorbis comments. Round-trip verified bit-identical across
  202 files via `flacrt_cli`.
- **MusicBrainz lookup** — `src/MusicBrainz.{h,cpp}`. discId →
  release + per-track titles, with the same medium-matching
  precedence rip_cd.sh used.
- **AR + CTDB verify** — `src/arverify_main.cpp`. Independent offset
  scans for each pool; reports per-track accuracy with confidence.
- **Vendored libFLAC** in `third_party/flac/`; system zlib (for fast
  CRC32). No Homebrew dependency in the binary.

Existing CLI targets you can compose against or model on:
`arverify_cli`, `flacrt_cli`, `mbquery_cli`.

## Non-negotiables (do not relitigate)

These are settled in CDRIP_STRATEGY.md and not reopen-able here:

- **Permissive license only.** No libcdio, libcdio-paranoia, or
  cdparanoia in any form — those are GPL and would force PLYR-Qt to
  GPL.
- **Native OS APIs, not subprocess wrappers.** Specifically: no
  shelling out to `cdparanoia` "as a temporary measure" — that's the
  GPL-via-the-back-door route. The point of vendoring libFLAC was
  the same: keep the binary self-contained AND license-clean.
- **No jitter correction in v1.** Plain sector reads with basic
  re-read on error is the scope. If/when jitter correction is added,
  it must be a clean-room implementation of the *technique*, not a
  port of cdparanoia's code.
- **CDDA reads happen downstream of all error correction.** The
  drive's silicon does Viterbi + CIRC Reed-Solomon. The host
  interface only exposes post-correction PCM. The only software
  levers are re-read, C2 pointers if the drive exposes them, and
  AR/CTDB cross-verification.

---

## The actual job

Deliver a `CdDevice` that can:

1. Enumerate optical drives on the system.
2. Open a chosen drive.
3. Report the drive's SCSI vendor + product strings (for offset lookup).
4. Read the disc's TOC.
5. Read N CD-DA sectors starting at an LBA into a caller buffer.
6. Surface read errors so a caller can re-read.

And a `Ripper` that glues `CdDevice` to the existing pipeline:
TOC → MB lookup → per-track read+encode+tag → AR/CTDB verify.

That's it. No subchannel data, no C2 pointer interpretation, no
multi-session, no mixed-mode CDs in v1.

---

## Cross-platform shape

The same MMC `READ CD` (`0xBE`) command underlies every platform we'll
ever target. macOS wraps it in `DKIOCCDREAD` (the kernel builds the CDB
for us); Linux sends it via SG_IO + a raw 12-byte CDB; Windows via SPTI
+ a raw 12-byte CDB. The shared per-sector payload for v1 is
`User Data + Error Flags` = 2646 bytes when C2 is enabled, 2352 bytes
when not. One mental model, three platform glues.

The canonical CDB for CDDA reads:

```
Cdb[0]  = 0xBE                                   // READ CD
Cdb[1]  = (1 << 2) | (0 << 1)                    // CDDA expected type, DAP=0
Cdb[2..5] = big-endian LBA (int32, signed-treated)
Cdb[6..8] = big-endian transfer-length (sectors, uint24)
Cdb[9]  = (1 << 4) | (wantC2 ? (1 << 1) : 0)     // User + optional C2
Cdb[10] = 0x00                                   // no sub-channel
Cdb[11] = 0x00                                   // control
```

DAP=0 is deliberate. DAP=1 enables the drive's firmware-level audio
concealment (interpolation/muting on uncorrectable errors), which is
right for analog playback and wrong for ripping. We want raw decoded
bytes so AR/CTDB checksums are meaningful. cdparanoia's `0x06` (DAP=1)
choice is a historical playback-oriented default; we deviate.

Per-platform mapping (also see §3.B of `CD_READER_RESEARCH_FINAL.md`):

| Concern              | macOS                                                    | Linux                                  | Windows                                |
|----------------------|----------------------------------------------------------|----------------------------------------|----------------------------------------|
| Enumerate            | `IOServiceMatching(kIOMediaClass)` + ejectable + whole   | `libudev` / `/sys/class/block/sr*`     | `SetupDiGetClassDevs(GUID_DEVINTERFACE_CDROM)` |
| Open                 | `open("/dev/rdiskN", O_RDONLY \| O_NONBLOCK \| O_EXLOCK)` | `open("/dev/sr0", O_RDWR \| O_NONBLOCK)` | `CreateFileW(...)` w/ `FILE_SHARE_*`   |
| Exclusivity          | `DASession` + `DADiskClaim` + `O_EXLOCK`                 | `O_RDWR` + cdrom group                 | `FILE_SHARE_*` flags                   |
| Read CDDA (no C2)    | `DKIOCCDREAD`, `sectorArea=0x10`, 2352/sector            | SG_IO + READ CD, `Cdb[9]=0x10`         | SPTI + same CDB                        |
| Read CDDA + C2       | `sectorArea=0x12`, 2646/sector                           | `Cdb[9]=0x12`, 2646/sector             | Same SPTI CDB                          |
| Read TOC             | `DKIOCCDREADTOC` (format 0x02)                            | SG_IO + READ TOC (0x43)                | SPTI + 0x43                            |
| Speed control        | `DKIOCCDSETSPEED`                                          | `CDROM_SELECT_SPEED` or SG_IO + 0xBB   | SPTI + 0xBB                            |
| Vendor / product     | IORegistry `Device Characteristics` → `Vendor Name`, `Product Name` | SG_IO INQUIRY or `/sys/block/sr0/device/{vendor,model}` | SPTI INQUIRY or `SPDRP_FRIENDLYNAME`   |
| Cancel in-flight     | close FD from another thread                              | close FD from another thread           | `CancelIoEx`                            |

---

## macOS approach: which APIs

Two layers:

### Device discovery — DiskArbitration framework

`<DiskArbitration/DiskArbitration.h>`. Create a `DASession`, register
a callback or iterate disks, filter for media kind = audio CD
(`kDADiskDescriptionMediaKindKey == "IOCDMedia"`, or check the
`kDADiskDescriptionMediaContentKey` value). Pull the BSD name
(`/dev/disk2` etc.) from the disk description.

Audio CDs do NOT auto-mount as filesystems (they have no FS), so DA
won't fight you the way it would with a data CD.

### Sector + TOC reads — IOCDMediaBSDClient ioctls

`<IOKit/storage/IOCDMediaBSDClient.h>` — public, entitlement-free
ioctls on the raw device (`/dev/rdiskN`):

- `DKIOCCDREAD` — read CD sectors. Struct `dk_cd_read_t`:
  - `offset` is **bytes from disc start**, computed as
    `lba * per_sector_total_size`. **NOT LBA in frames** — getting this
    wrong is silent data corruption. The per-sector total is the sum of
    the requested area sizes: 2352 for CDDA-only
    (`kCDSectorAreaUser`), 2646 for CDDA + C2
    (`kCDSectorAreaUser | kCDSectorAreaErrorFlags`). See `IOCDMedia.h`
    `readCD()` doc comment.
  - `bufferLength` is also bytes, = `sector_count * per_sector_total_size`.
  - `sectorArea` is a bitmask. `kCDSectorAreaUser` (0x10) for the
    2352-byte audio payload alone; OR with `kCDSectorAreaErrorFlags`
    (0x02) for the 294-byte C2 error-pointer mask (per-sector size
    becomes 2646).
  - `sectorType` = `kCDSectorTypeCDDA` for audio reads.
- `DKIOCCDREADTOC` — read the disc TOC. Struct `dk_cd_read_toc_t`
  yields the MMC-format TOC buffer; parse into our `DiscToc`.
- `DKIOCCDSETSPEED` / `DKIOCCDGETSPEED` — set/get drive transfer speed
  in kB/s (uint16 argument; `kCDSpeedMin = 0xB0`, `kCDSpeedMax =
  0xFFFF`). Used by the v1 retry path to drop to ~4× on persistent
  failure. Public, entitlement-free.
- `DKIOCCDREADDISCINFO`, `DKIOCCDREADTRACKINFO` — optional richer
  metadata if needed later.
- `DKIOCCDREADISRC`, `DKIOCCDREADMCN` — per-track ISRC and disc MCN.
  Out of v1 scope; listed for future v2.

Why this path vs. **SCSITaskUserClient**: the generic SCSITaskUserClient
explicitly *excludes* peripheral type 0x05 (multimedia/optical) in
Apple's own `SCSITaskUserClientIniter.cpp:117-122`, but a separate MMC
user client (`kIOMMCDeviceUserClientTypeID`, declared in
`SCSITaskLib.h:66-69`) IS open for optical drives without any
entitlement — redumper's macOS build uses this path. We still skip
the MMC user client for v1 because the BSD client ioctls cover every
read, TOC, and speed-control operation we need with strictly less code.
The earlier `com.apple.developer.driverkit.*` framing was incorrect
reasoning; the conclusion (use BSD ioctls) is right anyway.

### Drive vendor/product — IORegistry (no SCSI needed)

You don't need SCSI INQUIRY to learn the drive's vendor/product
strings. They're stored as IORegistry properties.

Starting from the IOCDMedia node DA hands you, walk parents with
`IORegistryEntryCreateIterator(... kIORegistryIterateParents |
kIORegistryIterateRecursively ...)` until you find an entry that
carries a `Device Characteristics` dictionary property; read
**`Vendor Name`** (`kIOPropertyVendorNameKey`,
`IOStorageDeviceCharacteristics.h:92`) and **`Product Name`**
(`kIOPropertyProductNameKey`, line 119) out of that dict.

Trim whitespace and uppercase before doing the AR DB lookup — both
are convention.

(Earlier drafts had the keys as `Vendor Identification` /
`Product Identification`; those are the raw INQUIRY field labels in
the SCSI spec, not the IOKit property keys. The keys above are the
correct ones.)

---

## Drive offset DB

Source: <https://www.accuraterip.com/driveoffsets.htm> — HTML table of
(vendor, product, offset) tuples for thousands of drives.

**Bundle, don't fetch.** One-time scrape, hand-clean (the upstream has
duplicates and ambiguous entries), commit as `resources/drive_offsets.json`
under a Qt resource (qrc). Embedded in the binary, no runtime fetch.

Schema:
```json
[
  {"vendor": "PIONEER", "product": "BD-RW BDR-XD05", "offset": 6},
  ...
]
```

Match logic (in `src/DriveOffsetDb.{h,cpp}` or similar):
1. Normalize both sides: uppercase, collapse whitespace, strip
   trailing version suffixes the AR DB sometimes encodes.
2. Exact match on (vendor, product).
3. If no exact match, return `nullopt` and let the caller decide
   (offer manual entry, or rip at offset 0 and let the AR offset
   scan figure it out post-hoc — the verifier can do that).

Don't get clever with substring matching: false matches across drive
families would silently misalign rips.

---

## Proposed module layout

New files (add alongside the existing ones in `src/`):

- **`src/CdDevice.h` / `.cpp`** — DA + IOKit wrapper on macOS; SG_IO
  on Linux; SPTI on Windows. Pure C++ + platform frameworks. No Qt
  dependency. Sketch:
  ```cpp
  namespace plyr::cd {

  struct DriveInfo {
      std::string id;        // platform handle: BSD name / "/dev/sr0" / `\\?\...`
      std::string vendor;    // trimmed, AR-DB matching form
      std::string product;
      std::string revision;
      bool hasMedia;
  };

  struct TocEntry { uint8_t trackNumber; uint32_t startLba;
                    bool isData; bool preEmphasis; };
  struct Toc      { std::vector<TocEntry> tracks; uint32_t leadOutLba; };

  enum class ReadStatus { Ok, TransientBusy, MediumError,
                          OutOfRange, Aborted, FatalDeviceError };

  struct ReadResult {
      ReadStatus status;
      uint32_t   sectorsRead;     // may be < requested
      std::vector<uint8_t> c2;    // sectorsRead × 294 bytes when wantC2
  };

  class CdDevice {
  public:
      static std::vector<DriveInfo>      enumerate();
      static std::unique_ptr<CdDevice>   open(const std::string& id);
      virtual ~CdDevice() = default;

      virtual const DriveInfo&        info() const = 0;
      virtual std::optional<Toc>      readToc() = 0;
      virtual ReadResult              readSectors(int32_t lba,
                                                  uint32_t count,
                                                  std::span<uint8_t> audio,
                                                  bool wantC2) = 0;
      virtual bool                    setReadSpeed(uint16_t kBps) = 0;
      virtual std::string             lastDeviceError() const = 0;
      virtual void                    cancel() = 0;
      virtual std::optional<std::string> readIsrc(uint8_t track) = 0; // v2
  };

  } // namespace plyr::cd
  ```

  Key shape decisions:
  - `int32_t lba` is **signed** so negative values can express
    lead-in reads (required for some drive offsets).
  - `wantC2` is a per-call toggle, not a device-wide mode. v1.0 ships
    callers passing `false`; v1.1 flips to `true` on retries.
  - `setReadSpeed(kBps)` wraps `DKIOCCDSETSPEED` / Linux
    `CDROM_SELECT_SPEED` / SPTI 0xBB. Returns true if the platform
    reported success; many drives silently ignore the request.
  - `cancel()` is the only method safe to call from another thread —
    it closes the FD/handle to unblock an in-flight blocking call.
  - **Not thread-safe** otherwise. Serialize at the caller (`Ripper`).
  - Drive-offset lookup is **not** `CdDevice`'s job — `Ripper` calls
    `DriveOffsetDb::lookup(vendor, product)` after `open()` and
    applies the offset at read time.
- **`src/DriveOffsetDb.h` / `.cpp`** — bundled JSON lookup.
- **`src/Ripper.h` / `.cpp`** — high-level orchestrator. Pulls in
  CdDevice, MusicBrainz, FlacEncode, ArVerify.
- **`src/ripper_main.cpp`** — new `ripper_cli` target (or, later, the
  GUI surface). `ripper_cli <output-dir>` rips the inserted disc.

CMake additions:
- Link `-framework IOKit -framework DiskArbitration -framework CoreFoundation`.
- Embed `resources/drive_offsets.json` as a qrc.

---

## MVP path (recommended order)

Land these in this order; each step has a meaningful "is it working"
check before moving on.

1. **Enumerate drives.** `CdDevice::enumerateDrives()` returns at
   least one entry when an external optical drive is connected.
   Check: print the BSD name and verify against `diskutil list`.

2. **Read the TOC.** `CdDevice::readToc()` produces a `DiscToc`
   matching `drutil discinfo` or `cdrecord -toc`'s output.
   Check: compute `DiscIds` from it and confirm the AR/CDDB/MB ids
   match what `arverify_cli` reports on the corresponding existing
   FLAC rip of the same disc.

3. **Read all sectors.** Loop `readSectors` from the first track's
   LBA through the leadout. Concatenate into a single packed-PCM
   buffer (use `arverify::packStereo` to convert in place).
   Check: total samples = (leadout - firstTrackLBA) × 588.

4. **Encode without tags or offset.** Slice the buffer by TOC track
   ranges; encode each track with `flacencode::encodeCdAudioToFile`.
   No tags, offset = 0.
   Check: `arverify_cli <output-dir>` runs and reports per-track
   offsets in the same range AR knows for that disc — i.e. the
   verifier's offset scan finds *some* offset where the audio is
   bit-accurate. If it can't find any, the rip is broken; debug
   before moving on.

5. **Drive INQUIRY → offset lookup.** Read vendor/product from the
   IORegistry path, look up `DriveOffsetDb`.
   Check: known drive returns its known offset.

6. **Apply offset at rip time.** Read `+offset` samples extra at each
   track boundary, slice the canonical range. Re-encode.
   Check: `arverify_cli` now reports `AR offset 0` (or very close)
   and full bit-accuracy on tracks that have AR DB coverage.

7. **MB lookup + tagging.** Plug `musicbrainz::lookupByDiscId` into
   the rip flow; pass `tagsForTrack`-style tags into the encoder.
   Check: `metaflac --list` shows correct Vorbis comments.

8. **Basic re-read on error.** Detect `ioctl` failure or short reads;
   retry the affected sector range up to 2 times at current speed. If
   still failing, zero-fill those sectors and log the LBA range +
   count in the rip log. (When C2 lands in v1.1: also trigger re-read
   on any non-zero byte in the C2 mask; same retry budget; same
   zero-fill-and-log fallback.) No drive-speed-down in v1; AR/CTDB
   cross-verification catches what re-read can't.

That's a working v1 ripper. Everything from step 4 onward is just
plumbing into pipeline pieces that already work in isolation.

---

## Known gotchas

- **Sector size is 2352 bytes (588 stereo frames).** Buffer math: a
  74-minute disc is ~333,000 sectors. CDDA reads are always
  sector-aligned.
- **`/dev/rdisk` not `/dev/disk`.** The "raw" character device skips
  the kernel's buffer cache — important for CD-DA reads since the
  cached path doesn't necessarily understand CDDA framing.
- **Open the device exclusive.** Other processes (Music.app, Finder,
  the disk arbitration daemon) can claim the disc; use `DAClaim` to
  ensure exclusive access, or open with `O_EXLOCK`. Otherwise
  background tools can interleave reads and corrupt yours.
- **Drive offset is applied at READ time, not VERIFY time.** Read
  `drive_offset` samples beyond the natural track end (and before
  the start), then slice. The verifier's offset scan exists to
  rescue *historical* rips that weren't offset-corrected; new rips
  should land at offset 0.
- **CDDA host interface only exposes post-CIRC PCM.** No raw channel
  bits, no Reed-Solomon parity. CIRC and any PRML detection happen
  inside the drive. Re-read is the only software lever. (Setting
  `DAP=0` in the CDB doesn't change this — it just stops the drive
  from substituting interpolated samples *after* CIRC has run.)
- **C2 error pointers** are requestable via `kCDSectorAreaErrorFlags`
  (per-sector size becomes 2646). v1.0 ships without C2 (matches
  libcdio's macOS default — known-good baseline). v1.1 enables C2
  and uses any non-zero byte in the 294-byte mask as a re-read
  trigger. Drive C2 accuracy varies — treat non-zero as "definitely
  re-read," treat zero as "probably OK, not guaranteed." AR/CTDB
  cross-verification remains the actual safety net.
- **Drive speed control via `DKIOCCDSETSPEED`.** Public, entitlement-
  free. v1.0 does not exercise it; v1.1+ may add speed-down on retry
  as a knob once real-rip telemetry shows it helps. Note this is an
  EAC-style strategy; cdparanoia does NOT slow the drive on retries
  (it just retries 20× at the current speed).
- **`dk_cd_read_t.offset` is BYTES, not LBA.** `offset =
  lba * per_sector_total_size`. Per-sector total = 2352 for CDDA
  alone, 2646 for CDDA + C2. Getting this wrong is silent corruption
  — verified against `IOCDMedia.h` `readCD()` doc comment and
  libcdio's `osx.c:1077-1082`. `bufferLength` is also bytes, =
  `sector_count * per_sector_total_size`.
- **MMC byte 1 `DAP` bit — set to 0 for ripping.** DAP=1 enables the
  drive's firmware-level audio concealment (interpolation/muting on
  uncorrectable errors). That's right for analog playback and wrong
  for ripping — we want raw decoded bytes so AR/CTDB checksums are
  meaningful. cdparanoia/libcdio set DAP=1 for legacy playback
  compatibility; we deviate. If a specific drive returns garbage at
  DAP=0 that DAP=1 silently "fixes," that drive's firmware is hiding
  errors from us — log and re-test.
- **Negative-LBA (lead-in) reads.** Required when the drive offset is
  negative enough to push track 1's first samples below LBA 0.
  Per-drive support varies sharply: ~135 sectors of lead-in on most
  modern LG/ASUS/Lite-On; ~75 with Plextor's `0xD8` command (out of
  v1 scope); **0 on most cheap slim USB drives**. Failed reads are
  handled by zero-padding; AR's first-5-sectors skip region absorbs
  the discrepancy.
- **First-track pregap.** Audio CDs typically have a 2-second pregap
  before track 1; the TOC's track 1 start address (150) accounts for
  this. Don't try to read sector -150; start at 0 (LBA) = 150
  (absolute). The existing TOC math in ArVerify already uses the
  absolute convention.
- **Track lengths are sector-aligned** by the CD spec — the TOC math
  in `arverify::reconstructToc` already rejects non-aligned input
  with `nullopt`. New rips should never trip that.

---

## Validation plan

Validation is straightforward because the verifier is already
trustworthy on this data:

1. **TOC compare**: rip a Rachmaninoff disc you have rip data for;
   reconstructed DiscIds must match what `arverify_cli` computes on
   the existing FLACs of that disc.
2. **Byte-level compare**: at the correct drive offset, the
   freshly-ripped PCM should be bit-identical to the existing rip's
   PCM at the offset detected by the verifier on the existing rip
   (CD02 = -570, CD03 = -670, etc. — see CDRIP_STRATEGY.md table).
3. **AR/CTDB verify**: `arverify_cli <new-rip-folder>` reports AR
   offset 0 (drive offset applied) and full bit-accuracy on tracks
   that have DB coverage.

A passing run on at least one Rachmaninoff disc + one new disc
(different physical pressing, different drive offset DB entry) gives
confidence the path is generalizable.

---

## Deliberately deferred

- **Linux + Windows readers.** macOS first. Linux uses **`SG_IO` +
  `READ CD` (0xBE)** on `/dev/sr0` — *not* `CDROMREADAUDIO`, which
  caps at 75 sectors per call and hides C2. Windows uses **SPTI**
  (`IOCTL_SCSI_PASS_THROUGH_DIRECT`) with the same READ CD CDB shape.
  Both wrap the same MMC command our macOS path does via
  `DKIOCCDREAD`; the abstraction lives behind `CdDevice`. Do them
  after macOS lands.
- **Subchannel reads** (track-relative timecodes, ISRC, MCN). Not
  needed for clean rips. The TOC has everything we need. (v1.1+ may
  add ISRC via `DKIOCCDREADISRC`.)
- **C2 error pointer interpretation in v1.0.** Promoted from "defer
  forever" to **v1.1**: ship v1.0 without C2 (matches libcdio macOS
  default, gets AR/CTDB pipeline validated end-to-end), then enable
  C2 with re-read-on-flag in v1.1. Trivial 4-line edit once v1.0 is
  proven.
- **Multi-session and mixed-mode CDs.** Single-session audio CDs
  only. Mixed-mode would auto-mount the data track via DA — refuse
  loudly in v1; solve later if it comes up.
- **Cdparanoia-grade jitter correction.** Already explicitly out of
  scope in CDRIP_STRATEGY.md. Re-read on error is the v1 ceiling. If
  v2 wants this, the implementer should work from §H of
  `CD_READER_RESEARCH_FINAL.md` (clean-room algorithm description) +
  Xiph's paranoia FAQ — NOT from the cdparanoia source directly, to
  preserve the clean-room derivation.
- **Software concealment of unreadable samples.** Out of v1. Concealment
  (linear-interpolate between known-good neighbors, mute longer
  gaps) writes "good-sounding" bytes that will never match AR/CTDB
  checksums on the canonical rip. If v2 wants concealment, design
  the `Ripper` pipeline so it layers in as a post-processing step
  applied to a separate "listenable" output file, leaving the
  canonical rip zero-filled-where-unreadable for AR/CTDB verification.
  CTDB *parity* repair (below) is strictly better than concealment
  when the disc is in CTDB.
- **CTDB parity-based repair.** CTDB stores Reed-Solomon-style parity
  that can restore specific bad samples to their actual canonical
  values, not just flag them as wrong; out of scope for v1.
- **Plextor `0xD8` lead-in reads.** Required only for HTOA recovery and
  for the extra lead-in overread some Plextor drives offer. Out of
  scope; would require sending a vendor MMC command, which the BSD
  ioctl path can't do.
- **Drive cache-test runtime detection** (cdparanoia's `cachetest.c`).
  Out of scope; AR/CTDB catches the resulting bad rips downstream.
- **MMC `SET STREAMING` (0xB6)** for explicit read-cache control. Out
  of scope; would require SCSITaskUserClient / MMC user client, which
  we don't use.
- **AccurateRip submission.** We're a consumer of AR/CTDB, not a
  submitter (for now).
- **GUI integration.** A `ripper_cli` is enough for v1 validation.
  Wiring into the Concerto QML UI comes after the CLI rip works.

---

## References

- `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/IOKit.framework/Headers/storage/IOCDMediaBSDClient.h`
  — `DKIOCCDREAD`, `DKIOCCDREADTOC`, `DKIOCCDSETSPEED`, all the relevant
  ioctl structs.
- `IOCDTypes.h` (same dir) — sector area sizes table at lines ~194-204;
  the canonical map from `kCDSectorAreaUser`/`...ErrorFlags` bits to
  per-area byte counts.
- `IOCDMedia.h` (Kernel.framework headers) — `readCD()` doc comment is
  the authoritative statement that `byteStart` / `bufferLength` are
  bytes, not LBA frames.
- `IOStorageDeviceCharacteristics.h` (lines 92, 119) — the IORegistry
  property keys (`kIOPropertyVendorNameKey`, `kIOPropertyProductNameKey`).
- `<DiskArbitration/DiskArbitration.h>` — `DASession`, `DADisk`,
  `DADiskClaim`, the description-key constants.
- Linux UAPI: `<linux/cdrom.h>` (the `CDROMREADAUDIO` ioctl + 75-sector
  cap and `CD_FRAMESIZE_RAWER = 2646` constant) and `<scsi/sg.h>`
  (`sg_io_hdr_t` for the `SG_IO` path we'll prefer over CDROMREADAUDIO).
- Windows: `ntddcdrm.h` (CD-ROM ioctl definitions) and `ntddscsi.h`
  (SPTI / `SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER`).
- MMC-6 spec (T10/1836-D Revision 2g, 2009-12-11) — §6.19 `READ CD`
  Command, Tables 351-356; the authoritative byte-by-byte CDB layout.
  Mt. Fuji 7 / SFF-8090i is the equivalent committee draft.
- Microsoft Windows-driver-samples SPTI tool (MIT-licensed) — reference
  shape for `SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER` and INQUIRY usage.
  Safe to read and study (permissive license).
- AR drive offset table: <https://www.accuraterip.com/driveoffsets.htm>
- Redumper's drive DB (`drive.ixx` for quirks, `offsets.ixx` for the
  ~4,600-entry offset table) — GPL-3, but the offset data itself is
  factual / public, fine to bundle. Don't copy code.
- Apple's archived "Accessing SCSI Architecture Model Devices" — the
  document showing why peripheral type 0x05 uses the MMC user client
  (`kIOMMCDeviceUserClientTypeID`), not the generic SCSITaskUserClient.
- Reference *behavior* (GPL — readable for ideas, don't transcribe
  code/structure): whipper, morituri, cdparanoia, libcdio,
  libcdio-paranoia, redumper, Aaru. Studying GPL source for technique
  is clean-room-legitimate; copying expression is not.
- `CD_READER_RESEARCH_FINAL.md` (sibling file) — the consolidated
  research that produced the corrections folded into this plan.
  Useful for the cited line numbers, the full `CdDevice` header
  sketch, and the source-grounded jitter algorithm description.

---

## Posture for this session

The existing pipeline is the validation harness — if your ripper
produces FLACs that `arverify_cli` marks ACCURATE on a Rachmaninoff
disc, you're done with v1.

When in doubt, ship the minimum-viable thing that AR/CTDB can verify,
then layer in offset correction, tags, error handling. Don't try to
land all 8 MVP steps at once.

If a conversation drifts toward "let's just call `cdparanoia` as a
helper to get started" — that's a re-decision of something already
settled. The slower native-API path *is* the path, same as it was for
the rest of this stack.
