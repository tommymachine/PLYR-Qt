# CD-DA Reader: Authoritative Reference

Consolidated, source-verified reference for implementing Concerto's native
CD-DA reader. Supersedes `CD_READER_RESEARCH.md` (initial broad survey) and
`CD_READER_RESEARCH_ADDENDUM.md` (source-grounded follow-up). Every load-bearing
claim is checked against a primary source (Apple SDK headers, T10/MMC-6 spec,
upstream Linux UAPI, libcdio/cdparanoia/redumper source) and cited inline.

This doc is self-contained. A reader who has never seen the prior two
research files should be able to use it to implement `CdDevice`.

---

## 1. TL;DR (what's authoritative now)

- **macOS BSD ioctls cover everything v1 needs.** `DKIOCCDREAD`,
  `DKIOCCDREADTOC`, `DKIOCCDSETSPEED`, `DKIOCCDGETSPEED`,
  `DKIOCCDREADISRC`, `DKIOCCDREADMCN`, `DKIOCCDREADDISCINFO`,
  `DKIOCCDREADTRACKINFO` are public, entitlement-free, and notarization-clean.
  No DriverKit entitlement is needed. The plan was right to pick this path;
  it stays.
- **`dk_cd_read_t.offset` is BYTES, computed as `lba × per_sector_total_size`** —
  NOT LBA. This is the silent-corruption-bug correction. The original report
  said "offset (LBA in frames)" which is wrong; the addendum was right.
  Apple's `IOCDMedia.h` doc comment is explicit. `bufferLength` is bytes too
  (`count × per_sector_total_size`). When `sectorArea` is just
  `kCDSectorAreaUser` (= `0x10`), per-sector size = 2352. With
  `kCDSectorAreaUser | kCDSectorAreaErrorFlags` (= `0x12`), per-sector size =
  2646. Get this wrong and you read garbage. Fixed.
- **IORegistry vendor/product keys are `Vendor Name` / `Product Name`** in
  a `Device Characteristics` dictionary. The plan currently says "Vendor
  Identification"/"Product Identification" — those are the raw SCSI INQUIRY
  field labels, not the IORegistry property keys. Fix.
- **MMC `READ CD` (0xBE) byte 1 layout**: bit 0 = Obsolete (was RELADR), bit
  1 = DAP, bits 2-4 = Expected Sector Type, bits 5-7 = LUN/reserved. For
  CDDA ripping use `Cdb[1] = 0x04` (type=CDDA=1, DAP=0). The original
  report's "RELADR is bit 1" was wrong. Both arrive at `0x04` for our use
  but for different reasons. **DAP=0 is the right choice for ripping**:
  DAP=1 *enables* the drive's audio mute/interpolate concealment on the
  returned bytes — exactly what a ripper does NOT want. cdparanoia and
  libcdio set DAP=1 for historical-compat reasons; we should NOT.
- **Cdparanoia's actual retry policy is 20 retries total per sector with no
  speed-down.** The original report's "3 retries → 4x → 5 more retries"
  ladder was extrapolation from EAC, not derived from cdparanoia. The
  addendum is right. For v1 we recommend a much simpler ladder than either
  (see §3.A) — 2 retries at speed, optional C2-triggered re-read, then
  zero-fill+log, with AR/CTDB as the actual safety net.
- **C2 error pointers are available via `kCDSectorAreaErrorFlags`** on
  macOS BSD ioctl. Confirmed structurally from Apple's headers. **But: no
  production GPL code exercises this exact path on macOS** — libcdio's
  macOS reader uses `kCDSectorAreaUser` only. Recommended v1 design: ship
  the C2 path but be prepared to encounter drive firmware that doesn't
  populate the error-flags region when accessed via DKIOCCDREAD. Have a
  feature-flag fallback to "no C2."
- **SCSITaskUserClient via the `MMCDeviceInterface → SCSITaskDeviceInterface`
  rendezvous IS accessible to user-space apps for optical drives** without
  any DriverKit entitlement. redumper (a current shipping tool) uses this
  path on macOS today. The original report's "entitlement-gated" reasoning
  was wrong; the addendum's "no entitlement needed" reasoning is right.
  **However**: we still stay off SCSITaskUserClient for v1 because BSD
  ioctls cover every command we need. The decision is correct, the
  rationale is now correct too.
- **Drive offset DB is essentially the AccurateRip drive offsets table**
  (~thousands of entries). The richer "per-drive quirks" DB (pregap_start,
  c2_shift, read_method, sector_order) only matters for archival imaging,
  not consumer ripping. v1 ships the AR offset table only.
- **Pregap (lead-in) reachability per drive**: -135 sectors for most
  modern LG/ASUS/Lite-On burners (BE command); -75 for good Plextor with
  D8 command; 0 (none) for many cheap slim USB drives. The AR checksum
  skips first 5 sectors and last 5 sectors specifically to absorb the
  difference. Drives that silently return zeros or cached data on
  negative-LBA reads are not detectable from software; AR/CTDB
  verification post-rip is the safety net.

Bottom line: the **plan stays mostly correct**; the **addendum's
corrections supersede the original report wherever they disagree** (with
the exceptions noted in §2 below); v1 implementation should follow the
synthesized direction in §3 and §4.

---

## 2. Canonical answer per gap

Each subsection is structured: *Verdict → Citation → Why the prior agents
disagreed → v1 implementation recommendation*.

### A. Retry ladder design

**Verdict.** Neither prior recommendation is what cdparanoia does;
neither is the right choice for v1.

- **What cdparanoia ACTUALLY does:** default 20 retries per sector,
  resets to zero on any forward progress, no speed-down ever. Every 5
  retries the dynoverlap (correlation-window) grows by 1.5x, capped at
  32 sectors. On exhaustion, `verify_skip_case` fills the sector from
  the best available cached fragment (or zero-fill) and continues.
- **What EAC does:** roughly the "drop speed on retry" ladder the
  original report described, but the specifics are not public spec —
  EAC is closed-source freeware.
- **What redumper does:** retry budget is per-sector and configurable
  via `--retries=N` (default 50); it makes multiple read passes and
  cross-correlates between passes (similar in spirit to paranoia but
  simpler). It does not speed-down.

**Citations:**
- cdparanoia retry constant: `cdio_paranoia_read(...)` calls
  `paranoia_read_limited(p, callback, 20)` at
  `libcdio-paranoia/lib/paranoia/paranoia.c:2880`.
- cdparanoia retry escalation logic: `paranoia.c:3057-3084` — counter
  resets on `lastend + 588 < re(root)` (line 3057), increments dynoverlap
  every 5 retries (line 3068), falls through to `verify_skip_case` at
  cap (line 3072).
- cdparanoia has no speed-down: `grep -i "set_speed\|speed_down" paranoia.c`
  returns nothing.
- AR "skip 5 sectors at each end" comment in `cd-paranoia.c:1622-1628`
  zero-fills the missing trailing sectors when not using `--force-overread`.

**Why the prior agents disagreed.** The original report wrote the EAC
ladder from external write-ups and labeled it as cdparanoia's; the
addendum read paranoia.c directly and corrected the record.

**v1 recommendation.** Simpler than either:
1. Issue `READ CD` (via `DKIOCCDREAD` / SG_IO / SPTI) at the drive's
   native speed.
2. On ioctl-level failure or short read: retry the same range up to
   **N=2** times immediately.
3. (With C2) On a successful read whose 294-byte C2 region is non-zero
   for any sector: re-read that sector range once.
4. If still failing: zero-fill the affected sectors, log the LBA range
   and the per-sector C2 mask in the rip log, continue.
5. **No speed-down in v1**. If real-world rips show speed-down would
   help, add it as a v1.1 knob via `DKIOCCDSETSPEED`. The mechanism is
   one ioctl call; it's not a complexity cost, just a policy lever.
6. **AR + CTDB cross-verification is the actual error catch.** A
   defect-induced silent error (drive returns zeros, drive returns
   cached data, drive's C2 is broken) will not match any AR/CTDB pool,
   and the verifier reports it to the user.

This is materially simpler than the original report's "3 + 5 with
speed-down" and materially simpler than cdparanoia's verify/merge. It's
appropriate for modern Accurate-Stream drives ripping clean discs, which
is our use case.

---

### B. MMC `READ CD` (0xBE) CDB layout

**Verdict.** The addendum is right on byte 1 (bit 1 = DAP, not RELADR;
sector type is bits 2-4 only, not bits 2-5). The numerical CDB
construction `Cdb[1] = 0x04, Cdb[9] = 0x12` both prior docs arrived at
is correct. **DAP=0 is the right choice for ripping; do not set DAP=1.**

**Citation: MMC-6 spec Table 351, byte 1 layout (T10/1836-D Revision 2g,
11 December 2009, page 362):**

```
Byte 1: bit 7  bit 6  bit 5     bit 4 bit 3 bit 2   bit 1   bit 0
        |    Reserved    |  Expected Sector Type   | DAP  | Obsolete
```

So byte 1 has three fields: bits 7-5 reserved (3 bits), bits 4-2 Sector
Type (3 bits, max value 5), bit 1 DAP, bit 0 Obsolete (was RELADR). For
CDDA: `(1 << 2) = 0x04`, DAP=0 (raw), Obsolete=0 → `Cdb[1] = 0x04`.

**Citation: DAP semantics (MMC-6 §6.19.2.3, page 363):**

> "If the data being read is CD-DA and DAP is set to zero, then the user
> data returned to the Host **should not be modified by flaw obscuring
> mechanisms such as audio data mute and interpolate**. If the data
> being read is CD-DA and DAP is set to one, then the user data
> returned to the Host **should be modified by flaw obscuring
> mechanisms such as audio data mute and interpolate**."

This is the spec. **For ripping you want DAP=0** — the raw bytes, not
the firmware-concealed bytes. libcdio's default and cdparanoia's
`i_read_mmc3` set DAP=1, but they're inheriting old behavior from a
time when many drives misbehaved with DAP=0; that compat hedge is no
longer worth the cost of accepting concealed data. (We should
empirically verify on the user's actual drive that DAP=0 returns audio
correctly; if a specific drive fails with DAP=0, add a per-drive
override.)

**Byte 9 layout (MMC-6 Table 351 + Table 355, page 366):**

```
Byte 9: bit 7  bits 6-5    bit 4      bit 3       bits 2-1     bit 0
        SYNC   Header Code User Data  EDC/ECC   C2 Error Info Reserved
```

C2 Error Info is a **2-bit field** (bits 2-1):
- `00` → 0 bytes (no C2)
- `01` → 294 bytes (per-sample C2 flags)
- `10` → 296 bytes (Block Error Byte + pad + 294-byte C2)
- `11` → Reserved

For UserData + 294-byte C2: `Cdb[9] = (1<<4) | (1<<1) = 0x12`.

**Citation: libcdio's production CDB construction
(`libcdio/lib/driver/mmc/mmc_ll_cmds.c:326-335`):**

```c
CDIO_MMC_SET_READ_TYPE(cdb.field, read_sector_type);    /* cdb[1] = type << 2 */
if (b_digital_audio_play) cdb.field[1] |= 0x2;          /* DAP = bit 1 */
...
if (b_user_data) cdb9 |=  16;                           /* 0x10 = bit 4 */
cdb9 |= (header_codes & 3)         << 5;
cdb9 |= (c2_error_information & 3) << 1;                /* 2-bit C2 field */
cdb.field[9]  = cdb9;
cdb.field[10] = (subchannel_selection & 7);
```

**Field order in returned data (MMC-6 §6.19.2.8, page 366, "The Drive
shall transfer the selected fields in the following order"):** Sync,
Header, Sub-header, User Data, EDC, Mode 1 pad, ECC parity, C2 block
error bytes, C2 Error flags, Sub-channel. So for CDDA with `Cdb[9] =
0x12`, the per-sector payload is **User Data (2352 bytes) followed by
C2 Error flags (294 bytes) = 2646 bytes/sector**.

**Why the prior agents disagreed.** Original report wrote bit 1 = RELADR
from a misread of a textual MMC summary; the addendum read the libcdio
source which sets bit 1 from `b_digital_audio_play` and went back to the
spec to confirm. Spec wins.

**v1 recommendation.** Build the CDB once in a shared helper, parameterized
by `wantC2`:

```
Cdb[0]  = 0xBE                                  // READ CD
Cdb[1]  = (1 << 2) | (DAP << 1)                 // CDDA, DAP=0 for ripping
Cdb[2..5] = big-endian uint32 LBA               // signed value lets you express -ve LBAs
Cdb[6..8] = big-endian uint24 transfer_length   // sector count
Cdb[9]  = (1 << 4) | (wantC2 ? (1 << 1) : 0)    // UserData + optional C2
Cdb[10] = 0x00                                  // no sub-channel for v1
Cdb[11] = 0x00                                  // control byte
```

On macOS the kernel builds this CDB for us when we call `DKIOCCDREAD`.
On Linux we send it via SG_IO. On Windows via SPTI.

---

### C. `dk_cd_read_t.offset` units — critical implementation detail

**Verdict. The addendum is RIGHT, the original report and the plan are
both WRONG.** `dk_cd_read_t.offset` is BYTES from disc start, computed as
`lba × per_sector_total_size`. So is `bufferLength`. This is
silent-corruption-bug territory if you treat it as LBA-in-sectors.

**Citation: Apple `IOCDMediaBSDClient.h:51-66` (Xcode SDK header on the
user's machine):**

```c
typedef struct {
    uint64_t offset;            /* documented in IOCDMedia.h */
    uint8_t  sectorArea;
    uint8_t  sectorType;
    ...
    uint32_t bufferLength;      /* actual length on return */
    void *   buffer;
} dk_cd_read_t;
```

**Citation: Apple `IOCDMedia.h` doc comment on `readCD()` (file at
`/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/IOKit/storage/IOCDMedia.h`,
lines 213-221 and again at 254-261):**

> @param byteStart
> Starting byte offset for the data transfer (see sectorArea parameter).
> @param sectorArea
> Sector area(s) to read. **The sum of each area's size defines the
> natural block size of the media for the call. This should be taken
> into account when computing the address of byteStart.** See
> IOCDTypes.h.

**Citation: libcdio's production code at `osx.c:1077-1082` (the function
ships in libcdio's macOS driver):**

```c
cd_read.offset       = lsn * kCDSectorSizeCDDA;            // bytes
cd_read.sectorArea   = kCDSectorAreaUser;                  // 0x10
cd_read.sectorType   = kCDSectorTypeCDDA;                  // 0x01
cd_read.buffer       = p_data;
cd_read.bufferLength = kCDSectorSizeCDDA * i_blocks;       // bytes
ioctl(env->gen.fd, DKIOCCDREAD, &cd_read);
```

`kCDSectorSizeCDDA = 2352` per `IOCDTypes.h:230`.

**Per-area sizes for CDDA** (from `IOCDTypes.h:194-204`, the table comment):

| Area              | Bit mask | Size for CDDA |
|-------------------|---------:|--------------:|
| Sync              |     0x80 |             0 |
| Header            |     0x20 |             0 |
| SubHeader         |     0x40 |             0 |
| User              |     0x10 |          2352 |
| Auxiliary         |     0x08 |             0 |
| ErrorFlags (C2)   |     0x02 |           294 |
| SubChannel (raw)  |     0x01 |            96 |
| SubChannelQ       |     0x04 |            16 |

So per-sector total sizes for the bitmask combinations you might use:
- `0x10` (User only): 2352 bytes
- `0x12` (User + C2): 2352 + 294 = **2646 bytes**
- `0x14` (User + Q-subchannel): 2352 + 16 = 2368 bytes
- `0x16` (User + C2 + Q-sub): 2352 + 294 + 16 = 2662 bytes

For `wantC2=true`: set `sectorArea = 0x12`, then `offset = lba * 2646`,
`bufferLength = count * 2646`. Get this multiplication wrong and you'll
read overlapping or skipped sectors silently.

**v1 recommendation.** Centralize the `per_sector_size` lookup in one
helper:

```cpp
constexpr uint32_t perSectorSize(uint8_t sectorArea) {
    return (sectorArea & 0x80 ? 12 : 0)
         + (sectorArea & 0x20 ? 4  : 0)
         + (sectorArea & 0x40 ? 0  : 0)   // SubHeader: 0 for CDDA
         + (sectorArea & 0x10 ? 2352 : 0) // User: 2352 for CDDA
         + (sectorArea & 0x08 ? 0  : 0)   // Auxiliary: 0 for CDDA
         + (sectorArea & 0x02 ? 294 : 0)  // ErrorFlags (C2)
         + (sectorArea & 0x01 ? 96 : 0)   // SubChannel: 96
         + (sectorArea & 0x04 ? 16 : 0);  // SubChannelQ: 16
}
```

Then every call site computes `offset = lba * perSectorSize(sectorArea)`,
`bufferLength = count * perSectorSize(sectorArea)`. No room for a
silent-corruption bug.

---

### D. SCSITaskUserClient entitlement reality on signed apps in 2026

**Verdict. The addendum is right: optical drives ARE accessible to
user-space apps via the `MMCDeviceInterface → SCSITaskDeviceInterface`
path without any DriverKit entitlement.** The original report's
"entitlement-gated" reasoning was wrong. **We still skip
SCSITaskUserClient for v1**, but for the right reason: BSD ioctls cover
all our use cases, so the simpler path wins.

**Citation: Apple's own source confirms the exclusion of optical drives
from the *generic* SCSITaskUserClient initer
(`apple-oss-distributions/IOSCSIArchitectureModelFamily/UserClient/SCSITaskUserClientIniter.cpp:113-122`):**

```cpp
deviceType = ( ( OSNumber * ) nub->getProperty (
    kIOPropertySCSIPeripheralDeviceType ) )->unsigned32BitValue ( );
switch ( deviceType )
{
    case 0x00000000:   // Direct-Access (hard disks)
    case 0x00000005:   // Multimedia (CD-ROM/DVD-ROM/BD-ROM)
    case 0x00000007:   // Optical Memory
    case 0x0000000E:   // Reduced Block Command
        doMerge = false;
        break;
    default:
        break;
}
```

The comment at lines 102-104 says: *"Special case the
IOSCSIPeripheralDeviceNub. Since there are in-kernel drivers for some
devices, we only merge the properties for devices with no in-kernel
driver (e.g. a tape drive)."* So the *generic* SCSITaskUserClient is
not available for peripheral type 0x05. **But Apple ships a separate
MMC user client for optical drives** as documented by
`apple-oss-distributions/IOSCSIArchitectureModelFamily/UserClientLib/SCSITaskLib.h:46-50`:

> "SCSITaskLib implements non-kernel task access to specific IOKit
> object types, namely any SCSI Peripheral Device for which there
> isn't an in-kernel driver **and for authoring devices such as CD-R/W
> and DVD-R/W drives**."

And lines 66-69 define `kIOMMCDeviceUserClientTypeID` — the factory ID
that user-space apps call `IOCreatePlugInInterfaceForService` with to
get an MMC user client. This is the path libcdio uses (under
`GET_SCSI_FIXED`) and the path redumper uses unconditionally on macOS.

**Citation: redumper's production macOS code
(`superg/redumper/scsi/sptd.ixx:81-127`):**

```cpp
// Match service against authoring-device dictionary
CFDictionarySetValue(authoring_dictionary.get(),
    CFSTR(kIOPropertySCSITaskDeviceCategory),
    CFSTR(kIOPropertySCSITaskAuthoringDevice));
CFDictionarySetValue(matching_dictionary.get(),
    CFSTR(kIOPropertyMatchKey), authoring_dictionary.get());
// ... iterate, find by BSD name, then:
IOCreatePlugInInterfaceForService(_service.get(),
    kIOMMCDeviceUserClientTypeID, ...);
// Query for MMCDeviceInterface, get SCSITaskDeviceInterface, claim:
(*scsiTaskDeviceInterface)->ObtainExclusiveAccess(scsiTaskDeviceInterface);
```

redumper is a shipping tool with macOS builds; this code works under
Apple's standard Developer ID signing + notarization with **zero DriverKit
entitlements declared**. The Apple developer forums thread #650611
(quoted by the addendum) is about *driver development* support in
DriverKit, not about user-space consumers — the consumer path is alive.

**Why the prior agents disagreed.** Original report conflated DriverKit
entitlement requirements (for new kernel-like drivers) with user-space
consumption of existing in-kernel drivers (which is unaffected). The
addendum read the Apple source distribution and the redumper code and
correctly noted the path is open. The addendum's exact line — *"Apple's
own design intends for user-space apps to access them via
`MMCDeviceInterface → SCSITaskDeviceInterface`"* — is correct.

**v1 recommendation.** Still stay off SCSITaskUserClient for v1. Reason
stated correctly: **BSD ioctls (DKIOCCDREAD + DKIOCCDREADTOC +
DKIOCCDSETSPEED + DKIOCCDREADISRC + DKIOCCDREADMCN) cover every MMC
command we need.** Vendor-specific commands (Plextor D8, custom
mode-sense pages, SET STREAMING for cache control) are not needed for a
consumer ripper.

If a future feature needs Plextor D8 (deep lead-in reads for HTOA) or
SET STREAMING (read-ahead disabling), expect ~1 week of plumbing for the
`kIOMMCDeviceUserClientTypeID → MMCDeviceInterface →
SCSITaskDeviceInterface → SCSITaskInterface → ExecuteTaskSync` rendezvous.
The path is documented and unmysterious — see redumper's source for the
shape (read for understanding only; don't copy code).

---

### E. C2 access policy

**Verdict.** C2 is requestable on all three platforms. cdparanoia
doesn't request it (`Cdb[9] = 0x10`) for historical reasons that don't
apply to us. libcdio's `mmc_read_cd` exposes the C2 parameter. The right
v1 policy is **always request C2** and use any non-zero byte as a
re-read trigger, with the caveat that drive C2 accuracy varies.

**Why cdparanoia doesn't request C2:** when cdparanoia was written (late
1990s), C2 reporting was unreliable on most drives — many drives didn't
implement it, many reported false positives or false negatives. The
verify/jitter algorithm was its substitute for trusting C2. By the
mid-2000s drive C2 had improved enough that EAC and dBpoweramp use it as
a standard signal. The status as of 2026 is "mostly reliable on
mainstream drives" but still not perfect (see CdrInfo's "Testing C2"
write-ups).

**Where does libcdio's macOS path stand?** `osx.c:1078` sets
`cd_read.sectorArea = kCDSectorAreaUser` — i.e., it does NOT request C2
even though the BSD ioctl supports it. So we have **no production GPL
datapoint that DKIOCCDREAD returns valid C2 on macOS**. The header at
`IOCDTypes.h:200` says ErrorFlags is 294 bytes for CDDA and the kernel
side ought to forward whatever the drive returns. Empirical
verification on the user's actual drive is the only way to know for sure.

**v1 recommendation.** Two-phase rollout:

- **v1.0 ship without C2**: `sectorArea = kCDSectorAreaUser` (= `0x10`),
  per-sector = 2352, retry on ioctl failure only. This matches libcdio's
  default and is known-good on macOS. Get the rest of the pipeline
  validated against AR/CTDB at this level.
- **v1.1 add C2**: `sectorArea = kCDSectorAreaUser | kCDSectorAreaErrorFlags`
  (= `0x12`), per-sector = 2646. After each read, scan the 294-byte C2
  block per sector; on any non-zero byte, re-read once. The code change
  is a literal 4-line edit in `CdDevice::readSectors`. Gate it behind a
  config knob initially in case a user's drive doesn't honor it.

In either phase, AR/CTDB cross-verification is the actual error catch.
C2 is a fast filter to *trigger* a re-read without waiting for AR to
tell us a track is broken.

**Caveat on drive C2 quirks (redumper's drive DB documents this).** A
few drives have a `c2_shift` quirk: their C2 byte stream is offset from
the corresponding user-data byte stream by some number of bytes
(`294`, `295`, etc.). This is invisible to BSD-ioctl users because
Apple's kernel translates per-drive (presumably), but Linux/Windows raw
SG_IO/SPTI would need to be aware. For v1 we assume zero shift. If C2
re-reads consistently fire on a particular drive and re-reads always
succeed, that's a sign of c2_shift; punt to "no C2 for this drive."

---

### F. Lead-in/lead-out per-drive overread ranges

**Verdict.** The addendum's numbers are confirmed from redumper's
production drive DB. Most modern LG/ASUS/Lite-On burners reach -135
sectors of lead-in (via `READ CD` 0xBE). Plextor with the D8 command
reaches -75. Cheap USB slim drives reach 0 (no lead-in).

**Citation: redumper `drive.ixx:132-228` — the full drive DB.**

A representative sample (line numbers in `drive.ixx`):

| Drive (vendor / product / rev) | pregap_start | Method | Source line |
|---|---:|---:|---:|
| ATAPI iHBS112 2, PL06 (LITE-ON) | -135 | BE | 153 |
| HL-DT-ST BD-RE BU40N, 1.00 | -135 | BE | 154 |
| ASUS BW-16D1HT, 3.02 | -135 | BE | 155 |
| ASUS BW-12B1ST, 1.03 | -135 | BE | 161 |
| PLEXTOR CD-R PX-W4012A, 1.07 | -75 | D8 | 135 |
| PLEXTOR DVDR PX-755A, 1.08 | -75 | D8 | 150 |
| ASUS SDRW-08D2S-U, B901 | -135 | BE | 213 |
| ASUS SDRW-08U9M-U, A112 | -135 | BE | 214 |
| Lite-On LTN483S 48x Max, PD03 | **0** | BE | 215 |
| PIONEER BD-RW BDR-209D, 1.10 | **0** | BE | 218 |
| TSSTcorp DVD-ROM TS-H352C, DE02 | **0** | BE | 217 |

**Edge cases worth flagging:**
- **Lite-On LTN483S** has `read_offset = -1164` and `pregap_start = 0`
  — its drive offset is more negative than its lead-in reach. This
  drive cannot produce a bit-accurate rip of track 1 without external
  zero-padding (which AR's skip region absorbs anyway).
- **PIONEER BD-RW BDR-209D** has `read_offset = +667` and
  `pregap_start = 0` — needs lead-out overread by ~667 samples that it
  doesn't have. Same AR-skip-region absorbs.
- **PLEXTOR DVDR PX-740A** has the comment in the DB at line 223:
  *"doesn't stop on lead-out but always returns same sector"*. This is
  the silent-stale-cache failure mode. AR verification at any offset
  will fail across the whole disc and the user is told "something is
  wrong, try another drive."

**Lead-OUT side:** good Plextor drives read ~100 sectors of lead-out
via `0xBE`; good MediaTek-chipset drives ~75; most drives 0. The
addendum's numbers are right; the original report's "0-150" range was
too vague.

**Why the prior agents disagreed.** Original report inferred a vague
range from hydrogenaudio; addendum read redumper's DB directly. The
addendum is the verified source-of-truth.

**v1 recommendation.** Don't try to predict per-drive pregap_start.
Issue the read at the negative LBA; if it fails with sense data, fall
back to zero-pad. If the drive silently returns zeros or stale data,
AR/CTDB catches it post-hoc (the rip won't match any pressing). This is
what libcdio-paranoia's CLI does, so we have a working precedent.

A future v2 could bundle the addendum's distillation of redumper's
pregap_start table for *predicting* the failure (so the UI says "this
drive can't read lead-in, but we'll attempt the rip" with no surprise
later) — but that's pure polish.

---

### G. Drive-offset application at LBA boundaries

**Verdict.** Both prior docs cover this; the addendum's specific algorithm
description matches libcdio-paranoia's CLI behavior. The hybrid approach
(apply offset at read time, re-rip if AR's post-hoc scan detects a
different offset) is the right answer.

**Citation: libcdio-paranoia/src/cd-paranoia.c offset handling
(`cd-paranoia.c:1158-1165`):**

```c
if (sample_offset) {
    toc_offset += sample_offset / 588;     /* whole sectors */
    sample_offset %= 588;                   /* fractional remainder */
    if (sample_offset < 0) {
        sample_offset += 588;
        toc_offset--;
    }
}
```

After this, `toc_offset` (whole sectors) is added to every track's start
LBA in the in-memory TOC (line 1289: `i_first_lsn += toc_offset`); the
fractional `sample_offset` is the per-track buffer-skip amount applied
on output (`offset_skip = sample_offset * 4`, line 1300).

**Lead-out zero-pad** (`cd-paranoia.c:1616-1628`):

```c
/* Write sectors of silent audio to compensate for
   missing samples that would be in the leadout */
if (cdda_sector_gettrack(d, batch_last - toc_offset) == d->tracks
    && toc_offset > 0 && !force_overread) {
    char *silence = calloc(toc_offset, CD_FRAMESIZE_RAW);
    buffering_write(out, silence, missing_sector_bytes);
}
```

So without `--force-overread` (the default), the trailing samples are
zero-padded. The AR skip-region (last 5 sectors) absorbs the difference.

**Lead-in handling:** there's no special-casing. cdparanoia attempts the
negative-LBA read; outcome is drive-dependent (sense-error, zero-pad,
real data, cached/stale data, hang). The libs report whatever the drive
returns and AR's skip-region absorbs the difference at the start.

**Whipper's offset behavior.** Whipper just spawns
`cd-paranoia --sample-offset=N [--force-overread]` as a subprocess
(`whipper/program/cdparanoia.py:272-285`), so the actual algorithm is
cdparanoia's. The addendum's claim "whipper delegates entirely to
cdparanoia" is confirmed.

**The "skip 5 sectors at each end" in AR's checksum:** the AR v1/v2
formulas skip the first 2940 stereo samples (= 5 sectors × 588) of
track 1 and the last 2940 of the last track. Sized so that whether your
drive can or can't read lead-in/lead-out, and whether your offset
shift is up to ±2940 samples, the checksum-computed region matches across
all rips. This is *explicitly* why the rip can be AR-accurate without
needing successful lead-in/lead-out reads.

**v1 recommendation (matches both prior docs' hybrid approach):**
1. Apply the drive offset at read time from the AR DB lookup. Read whole
   sectors. Skip the fractional `(offset % 588)` samples on output.
2. For negative offsets pushing track 1 below LBA 0: attempt the
   negative-LBA read. On sense-error, zero-pad. Don't fail the rip.
3. For positive offsets pushing the last track past leadout: zero-pad
   the trailing samples. Don't issue overread reads. (Equivalent to
   cdparanoia *without* `--force-overread`.)
4. After ripping, run AR/CTDB verify. If AR detects offset ≠ 0, the DB
   was wrong by that amount. Auto-re-rip once with corrected offset; if
   still not 0, mark the rip "offset-uncertain" and let the user decide.

The plan's step 6 ("Apply offset at rip time") + step 8 ("re-read on
error") + the existing AR verifier already do this. Just write it down.

---

### H. cdparanoia's jitter correction algorithm

**Verdict.** The addendum's algorithm description (verify-then-merge,
two-stage matching, jiggle/dynoverlap) is faithful to a direct read of
`paranoia.c`. The original report's external-write-up sketch was
correct at the level of "what" but not "how exactly."

**v1 decision.** Not implementing in v1. Modern Accurate-Stream drives
don't jitter; AR/CTDB cross-verify catches the rare residual cases.
**Description must be accurate for v2 implementer.**

**Key constants and behaviors confirmed against `paranoia.c`:**
- `MAX_SECTOR_OVERLAP = 32` sectors; initial `dynoverlap` = 32 × 1176
  = 37632 words (paranoia.c sees ~32 sectors of position uncertainty).
- `MIN_WORDS_SEARCH = 64` (minimum match length to consider).
- `MIN_WORDS_OVERLAP = 64`, `MIN_WORDS_RIFT = 16`, `MIN_SILENCE_BOUNDARY = 1024`.
- `JIGGLE_MODULO = 15`, prime-23 sample-domain stride.
- `cdcache_size = 1200` sectors per top-level read; sub-reads at
  ~25-27 sectors (drive max-transfer cap of ~64KB).
- Retry counter resets on root growing by ≥ 588 samples
  (`paranoia.c:3057`); dynoverlap grows 1.5x every 5 retries
  (`paranoia.c:3068-3082`); falls through to `verify_skip_case` on cap.

The two-stage architecture (c_block → v_fragment → root) and the
"silence as fluid mantle, content as solid islands" intuition is
specific to paranoia and is the IP load-bearing part. A v2 clean-room
implementation should *not* reuse paranoia's variable names or trim
constants; it should redesign from "what does the algorithm need to
prove?" first principles.

**Recommendation for v2 (if ever needed):** the v2 author should read
this section + the addendum's section 1 (which has more detail) + the
xiph paranoia FAQ — and **not** read paranoia.c. The clean-room
constraint matters for the licensing posture.

---

### I. Other discrepancies surfaced by diff-reading

#### I.1. Drive DB size

- Original report: "AR DB has thousands of drives" (correct, vague).
- Addendum: "redumper has ~70 entries" (wrong, undercounted).
- **Truth:** redumper's *rich quirks DB* (with c2_shift, pregap_start,
  read_method, sector_order) has **85 entries** (counted in
  `drive.ixx:135-228`); redumper's *offset-only DB* (mirror of the AR
  drive offset table) has **4,626 entries** (`offsets.ixx`, `grep -c
  "^    {"`). For v1 we only ship the offset table; ~4-5k entries is
  realistic.

#### I.2. libcdio's macOS enumeration approach

- Addendum (line 539): "libcdio uses `IOServiceMatching("IOCDBlockStorageDevice")`
  to enumerate drive nubs."
- **Actual libcdio behavior (`osx.c:1856-1866`):**

```c
classes_to_match = IOServiceMatching( kIOMediaClass );
CFDictionarySetValue( classes_to_match, CFSTR(kIOMediaEjectableKey),
                      kCFBooleanTrue );
CFDictionarySetValue( classes_to_match, CFSTR(kIOMediaWholeKey),
                      kCFBooleanTrue );
```

So libcdio enumerates `kIOMediaClass` filtered by ejectable + whole. The
addendum was slightly off. Both approaches work; the libcdio approach
returns *media* nodes (with `kIOBSDNameKey` properties), and you walk
parents to get the drive-level vendor/product. Our `CdDevice::enumerate`
should match the libcdio approach: enumerate `kIOMediaClass` + ejectable
+ whole, filter to ones that `IOObjectConformsTo(node, kIOCDMediaClass)`,
yield the BSD name. Then on `open(bsdName)` walk parents for vendor/product.

#### I.3. `/dev/rdisk` rationale

- Original report (line 66): the raw device "doesn't go through buffer
  cache."
- Addendum (line 549): confirmed by libcdio's own comment.
- **Citation:** `libcdio/lib/driver/osx.c:1896-1898` — *"Below, by
  appending 'r' to the BSD node name, we indicate a raw disk. Raw disks
  receive I/O requests directly and don't go through a buffer cache."*

For CDDA reads via `DKIOCCDREAD` specifically, the buffer-cache concern
is secondary — the ioctl interprets the request, not pread. But the
raw device is the cleaner FD for ioctl work. Stay with `/dev/rdiskN`.

#### I.4. CDROMREADAUDIO Linux 75-sector cap

- Original report (line 159): "75-sector-per-call limit (~1 second of
  audio)."
- **Citation:** `drivers/cdrom/cdrom.c:3048` — `if (lba < 0 || ra.nframes
  <= 0 || ra.nframes > CD_FRAMES) return -EINVAL;` where `CD_FRAMES = 75`
  is defined in `include/uapi/linux/cdrom.h:354`.
- **Confirmed.** The cap is real and per-ioctl. SG_IO has no such cap
  (drive max-transfer limit only). Use SG_IO on Linux, not CDROMREADAUDIO.

#### I.5. Linux UAPI's CD_FRAMESIZE_RAWER = 2646

`include/uapi/linux/cdrom.h:367` defines `CD_FRAMESIZE_RAWER = 2646`
with the comment *"The maximum possible returned bytes"*. Linux's own
header documents the 2646-byte CDDA+C2 sector size. Useful evidence
that the layout is canonical, not a libcdio invention. Worth citing in
our README.

#### I.6. macOS `O_EXLOCK` vs `DAClaim` — recommendation refinement

- Original report (§A.4): "do both, belt-and-suspenders."
- Addendum: doesn't disagree but doesn't strongly endorse.
- **libcdio's actual behavior:** opens with `O_RDONLY | O_NONBLOCK`
  ONLY — no `O_EXLOCK`, no DAClaim (`osx.c:333`). It relies on the user
  having already ejected the FS-mounted media via Finder or `drutil`.
- **redumper's actual behavior:** calls a custom `unmountDisk()` (DA
  unmount) then claims the drive via SCSITaskUserClient's
  `ObtainExclusiveAccess` (`sptd.ixx:81-127`). DA unmount + SCSI
  exclusive is the strong form.
- **v1 recommendation:** since we're on BSD ioctls (not SCSITask), use
  `DASessionCreate + DADiskClaim` for discovery and to register media
  changes. For the FD: `open("/dev/rdiskN", O_RDONLY | O_NONBLOCK |
  O_EXLOCK)`. The combination is cheap and rules out the most common
  contention failure modes (Music.app autoplay, Finder, third-party
  autoplay tools).

#### I.7. `Cdb[10]` sub-channel selection

- Original report: bits indicating P-W=001b, Q-only=010b, R-W=100b.
- Addendum: same enumeration confirmed.
- **MMC-6 §6.19.2.8, Table 356 (page 366):**

```
000b  No Sub-channel data shall be returned.     0 bytes
010b  Formatted Q sub-channel data.              16 bytes
011b  Reserved.                                  —
100b  Corrected and de-interleaved R-W.          96 bytes
all other values reserved
```

Both prior docs say `001b = raw P-W subchannel = 96 bytes` — that's
actually a misreading of the spec; **`001b` is not a listed value in
MMC-6 Table 356**. The legacy `0x01` from earlier MMC drafts meant raw
P-W, but in MMC-5/6 only `000b`, `010b`, `100b` are defined. Note Apple's
`kCDSectorAreaSubChannel = 0x01` (raw 96-byte P-W) IS still in the
header at `IOCDTypes.h:214` — the bit-encoding in `sectorArea` is
different from the CDB byte 10 encoding; the BSD ioctl translates. For
v1 we ignore sub-channel either way.

#### I.8. macOS CDB[1] DAP vs ripping

- Original report: didn't have an opinion (treated `0x04` as canonical).
- Addendum (line 281): "0x06 is what cdparanoia and libcdio send;
  empirically more compatible."
- **Closer look:** DAP=1 instructs the drive to APPLY firmware
  audio-mute/interpolate concealment. This is the OPPOSITE of what a
  ripper wants — you want the raw, unconcealed bytes so software
  (you + AR/CTDB) can decide what was recoverable. The addendum's
  empirical-compatibility claim is real (some old drives misbehave with
  DAP=0), but for a 2026 ripper targeting modern USB drives, DAP=0 is
  the principled choice. **Caveat:** we should add a per-drive override
  if a user reports their drive returns garbage with DAP=0.

#### I.9. Pre-emphasis tagging

- Original report (line 615): "FLAC has a standard way to record this.
  Detect, write a `PRE_EMPHASIS=true` Vorbis comment, never apply
  de-emphasis (that's playback's job). Cheap to add."
- Addendum: doesn't address it.
- **Reality:** pre-emphasis is a per-track flag in the TOC's `control`
  byte (bit 0 = pre-emphasis). Approximately 0.1% of commercial CDs use
  it (mostly classical and jazz from 1982-1987). The Vorbis comment
  convention is `PRE_EMPHASIS=true` and many players (foobar2000,
  mpv) honor it on playback by applying de-emphasis filtering.
- **v1 recommendation:** detect and tag. The TOC parse already reads
  the control byte; just propagate it into the encoder's tag list. The
  Rachmaninoff set probably doesn't have any pre-emphasis tracks but
  the future user library might.

#### I.10. CDROM_SELECT_SPEED on Linux

- Original report (line 197): "Use the legacy ioctl — one line."
- **Confirmed via `drivers/cdrom/cdrom.c`:** the ioctl wraps MMC `SET CD
  SPEED` (0xBB) and has been stable for 20+ years. Take the legacy
  ioctl; if it ever vanishes (it won't), fall back to SG_IO + 0xBB.

#### I.11. `cdda2wav` / cdrtools licensing

- Original report (line 569): "CDDL. Avoid."
- **Truth.** Joerg Schilling's cdrtools is CDDL 1.0 (post-2007). CDDL is
  weak copyleft per-file. We could technically link cdrtools without
  re-licensing Concerto, but the linker-driven aggregation is murky and
  the cdrtools community is small. Not worth the risk; staying clean.

---

## 3. Unified `CdDevice` architecture

Consolidating §H of the original report, §6 of the addendum, and the
plan's API sketch into one coherent shape.

### 3.A. Header sketch

```cpp
// src/CdDevice.h
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace concerto::cd {

struct DriveInfo {
    std::string id;       // platform-specific opaque handle string
                          //   macOS: BSD name "disk2"
                          //   Linux: "/dev/sr0"
                          //   Win:   `\\?\...` symbolic link
    std::string vendor;   // INQUIRY vendor, AR-DB matching form (trimmed)
    std::string product;  // INQUIRY product, AR-DB matching form (trimmed)
    std::string revision; // INQUIRY rev, informational
    bool hasMedia;        // disc currently loaded?
};

struct TocEntry {
    uint8_t  trackNumber;   // 1..N
    uint32_t startLba;      // LBA in sectors, signed-treated for math
    bool     isData;        // control bit 2 set
    bool     preEmphasis;   // control bit 0 set
};

struct Toc {
    std::vector<TocEntry> tracks;
    uint32_t              leadOutLba;
};

enum class ReadStatus {
    Ok,
    TransientBusy,     // BUSY etc; retry is appropriate
    MediumError,       // medium error reported; re-read recommended
    OutOfRange,        // attempted negative LBA past drive's limit, etc.
    Aborted,           // user cancelled / device disappeared
    FatalDeviceError   // drive went away, bus error, etc.
};

struct ReadResult {
    ReadStatus status;
    uint32_t   sectorsRead;   // may be < requested on short read
    // When wantC2 was true: sectorsRead × 294 bytes of C2 flag bytes.
    // Bit (7 - n%8) of byte (n/8) flags user-data byte n.
    // Empty if wantC2 was false.
    std::vector<uint8_t> c2;
};

class CdDevice {
public:
    // Discovery (free function form to avoid an instance just to enumerate)
    static std::vector<DriveInfo> enumerate();
    static std::unique_ptr<CdDevice> open(const std::string& id);

    virtual ~CdDevice() = default;
    virtual const DriveInfo& info() const = 0;

    // TOC. nullopt on failure (no disc, sense error).
    virtual std::optional<Toc> readToc() = 0;

    // Reads `count` 2352-byte CDDA sectors starting at signed LBA into
    // `audio` (must be at least count * 2352 bytes). If wantC2 is true,
    // result.c2 contains count * 294 bytes of C2 flag bytes.
    //
    // LBA may be negative (lead-in) — platform impl validates; on some
    // drives a negative LBA fails with status=OutOfRange, on others it
    // succeeds with real or zero data.
    virtual ReadResult readSectors(int32_t lba,
                                   uint32_t count,
                                   std::span<uint8_t> audio,
                                   bool wantC2) = 0;

    // SET CD SPEED (kB/s; 0 = drive max, ~706 = 4x). Returns true if
    // the platform reported success; drives often ignore this without
    // an error.
    virtual bool setReadSpeed(uint16_t kBps) = 0;

    // Diagnostic. Platform-formatted last error string for the most
    // recent failed call. Empty if last call was Ok.
    virtual std::string lastDeviceError() const = 0;

    // Cancellation hook. Closes the underlying FD/handle from another
    // thread to unblock an in-flight blocking ioctl. Safe to call after
    // every readSectors call returns.
    virtual void cancel() = 0;

    // Optional/v2: subchannel ISRC for a track. Empty if unsupported.
    virtual std::optional<std::string> readIsrc(uint8_t track) = 0;
};

} // namespace concerto::cd
```

### 3.B. Per-platform mapping

| Concern                  | macOS                                            | Linux                                           | Windows                                 |
|--------------------------|--------------------------------------------------|-------------------------------------------------|-----------------------------------------|
| Enumeration              | `IOServiceMatching(kIOMediaClass)` + ejectable + whole | `libudev` or `/sys/class/block/sr*`              | `SetupDiGetClassDevs(GUID_DEVINTERFACE_CDROM)` |
| Open device              | `open("/dev/rdiskN", O_RDONLY \| O_NONBLOCK \| O_EXLOCK)` | `open("/dev/sr0", O_RDWR \| O_NONBLOCK)`         | `CreateFileW(symLink, GENERIC_READ\|WRITE, FILE_SHARE_READ\|WRITE)` |
| Exclusivity              | `DASession` + `DADiskClaim` + `O_EXLOCK`         | group cdrom + open `O_RDWR`                     | `FILE_SHARE_*` flags on `CreateFile`    |
| Read CDDA (no C2)        | `DKIOCCDREAD` w/ `sectorArea=0x10`, `sectorType=CDDA`, `offset=lba*2352` | SG_IO + `READ CD` (0xBE), `Cdb[9]=0x10`         | SPTI (`IOCTL_SCSI_PASS_THROUGH_DIRECT`) + same CDB |
| Read CDDA + C2           | `sectorArea=0x12`, `offset=lba*2646`              | `Cdb[9]=0x12`, 2646 bytes/sector buffer         | Same SPTI CDB shape                     |
| Read TOC                 | `DKIOCCDREADTOC` (format 0x02)                    | SG_IO + `READ TOC/PMA/ATIP` (0x43)              | SPTI + 0x43                             |
| Speed control            | `DKIOCCDSETSPEED` (uint16 kB/s)                   | `CDROM_SELECT_SPEED` (legacy) or SG_IO + 0xBB   | SPTI + 0xBB                             |
| Vendor/product           | IORegistry `Device Characteristics` dict → `Vendor Name`, `Product Name` | SG_IO + INQUIRY, or `/sys/block/sr0/device/{vendor,model}` | SPTI + INQUIRY, or `SetupDiGetDeviceRegistryProperty(SPDRP_FRIENDLYNAME)` |
| Cancel in-flight read    | close FD from another thread                      | close FD from another thread                    | `CancelIoEx` on the handle              |

### 3.C. Notes on the API

- **Ownership.** `std::unique_ptr<CdDevice>` from `open()`. RAII closes
  the FD and releases any DA/SPTI handles.
- **Threading.** `CdDevice` is **not thread-safe**. Calls are blocking;
  serialize at the caller. `cancel()` is the one exception — designed to
  be called from a UI thread to abort a worker-thread read.
- **The `wantC2` parameter.** Encodes the macOS bit and the
  Linux/Windows CDB byte 9 bit. One mental model across platforms.
- **`int32_t lba` is signed.** Negative values express lead-in
  positions. Platform impls validate against drive capability.
- **`readSectors` returns `sectorsRead` that may be < requested.** This is
  a real failure mode on some drives at boundaries; caller must check.
- **The drive offset lookup is the caller's job**, not `CdDevice`'s.
  `Ripper` (in `src/Ripper.cpp`) calls `DriveOffsetDb::lookup(vendor,
  product)` after `CdDevice::open`, then applies the offset at read time.

### 3.D. What we deliberately leave out

- **CD-TEXT.** Not needed; MusicBrainz provides tagging metadata.
- **CD audio analog playback** (`audio_play_msf` etc.). 1990s mode;
  modern apps decode CDDA to PCM and play through OS audio.
- **Mode 1 / Mode 2 / Form 1/Form 2 data sector reads.** We don't touch
  data tracks in v1.
- **`writeCD`.** Read-only library.
- **Multi-session.** Single-session audio CDs only in v1.
- **`run_mmc_cmd` escape hatch.** We don't need vendor-specific CDBs.
  If we ever do, add it as a `runVendorCommand` method, not as a
  generic MMC pass-through.
- **`eject`.** A `drutil eject` subprocess from the GUI is fine for
  v1; ejecting via MMC is overkill.

---

## 4. Concrete edits to `CD_READER_PLAN.md`

The plan is mostly right. The bulleted edits below are the surgical
changes needed to bring it up to date. Each is small enough to fold in
mechanically.

### 4.1. In §"macOS approach", under "Sector + TOC reads"

- Add `DKIOCCDSETSPEED` and `DKIOCCDGETSPEED` to the listed ioctls:
  "*Set or get drive transfer speed in kB/s (uint16 argument;
  `kCDSpeedMin = 0xB0`, `kCDSpeedMax = 0xFFFF`).*"
- Add `DKIOCCDREADISRC` and `DKIOCCDREADMCN`: "*Track ISRC and disc
  MCN; out of v1 scope, listed for future v2.*"
- **Replace the `dk_cd_read_t` field description**:
  > **`offset`** is bytes from disc start, computed as `lba *
  > per_sector_total_size`. **NOT LBA in frames.** The per-sector
  > total is the sum of the requested area sizes; for CDDA + C2
  > (`sectorArea = kCDSectorAreaUser | kCDSectorAreaErrorFlags`) it's
  > 2646; for CDDA only (`kCDSectorAreaUser`) it's 2352. See Apple's
  > `IOCDMedia.h` `readCD()` doc comment.
  >
  > **`bufferLength`** is also bytes, = `sector_count *
  > per_sector_total_size`.
  >
  > **`sectorArea`** is a bitmask; `kCDSectorAreaUser` (= 0x10) for
  > audio bytes alone, OR with `kCDSectorAreaErrorFlags` (= 0x02) for
  > C2 error pointers (294 bytes per sector after the audio).
- **Replace the C2 paragraph**:
  > C2 error pointers are requestable by OR-ing
  > `kCDSectorAreaErrorFlags` into `sectorArea`. Per-sector size
  > becomes 2352 + 294 = 2646 bytes. v1.0 ships without C2 (matches
  > libcdio's macOS default and is known-good); v1.1 enables C2 and
  > uses any non-zero byte in the 294-byte C2 mask as a re-read
  > trigger. AR/CTDB cross-verification remains the actual safety net.

### 4.2. In §"Drive vendor/product — IORegistry"

- Replace `Vendor Identification` with `Vendor Name` (literal IORegistry
  key; constant `kIOPropertyVendorNameKey` from
  `IOStorageDeviceCharacteristics.h:92`).
- Replace `Product Identification` with `Product Name`
  (`kIOPropertyProductNameKey`, line 119).
- Note the dictionary path: starting from the IOCDMedia node DA hands
  you, walk parents (`IORegistryEntryCreateIterator(...
  kIORegistryIterateParents | kIORegistryIterateRecursively ...)`)
  until you find an entry with a `Device Characteristics` dictionary
  property; read `Vendor Name` and `Product Name` from that dict.
- Trim whitespace, uppercase for AR DB lookup; both are convention.

### 4.3. In §"Proposed module layout / `CdDevice`"

Replace the API sketch with the one from §3.A above. Key changes from
the plan's current sketch:

- `readSectors` takes a `wantC2 bool` parameter; returns `ReadResult`
  carrying the C2 byte array.
- `lba` is `int32_t` (signed; allows lead-in).
- Add `setReadSpeed(kBps) -> bool`.
- Add `lastDeviceError() -> std::string`.
- Add `cancel()` for worker-thread abort.

### 4.4. In §"Known gotchas"

- **Replace** the "C2 error pointers are requestable on some drives,
  but interpreting them is drive-specific. Defer." bullet with:
  > C2 error pointers are requestable via `kCDSectorAreaErrorFlags`.
  > Per-sector size = 2646 bytes. v1.0 ships without C2; v1.1 adds
  > C2-triggered re-read. Drive C2 accuracy varies; treat non-zero
  > as "definitely re-read," treat zero as "probably OK, not
  > guaranteed," with AR/CTDB as the actual safety net.
- **Add a new bullet**:
  > **Drive speed control via `DKIOCCDSETSPEED`.** Available
  > entitlement-free. v1.0 does not exercise it; v1.1+ may add
  > speed-down on retry as a knob once telemetry from real rips shows
  > it helps.
- **Add a new bullet**:
  > **Negative-LBA (lead-in) reads.** Required when the drive offset is
  > negative enough to push track 1's first sample below LBA 0. Drives
  > vary: -135 sectors on most modern LG/ASUS/Lite-On, -75 with
  > Plextor D8 command, 0 on most cheap slim USB drives. Failed reads
  > are handled by zero-padding; the AR skip region (first 5 sectors)
  > absorbs the discrepancy.
- **Add a new bullet**:
  > **`dk_cd_read_t.offset` is BYTES, not LBA.** Use `lba *
  > per_sector_total_size`. See plan's macOS approach section for the
  > per-area-size lookup.
- **Add a new bullet**:
  > **MMC byte 1 `DAP` bit.** For ripping use DAP=0 (raw, no
  > firmware concealment). cdparanoia/libcdio set DAP=1 for legacy
  > compat; we want DAP=0. If a specific drive reports unreadable
  > sectors with DAP=0 that DAP=1 silently "fixes," that drive's
  > firmware is hiding errors from us — log and re-test.

### 4.5. In §"MVP path", step 8

Expand "Basic re-read on error" to:

> **Detect ioctl failure / short reads;** retry the affected sector range
> up to 2 times at current speed. If still failing, zero-fill those
> sectors and log the LBA range + count in the rip log. (When C2
> support lands in v1.1: also re-read on any non-zero C2 byte; same
> retry budget; same zero-fill-and-log fallback.) No speed-down in
> v1; AR/CTDB cross-verify catches what re-read can't.

### 4.6. In §"Non-negotiables"

No changes. The plan's four non-negotiables remain correct.

### 4.7. In §"Deliberately deferred"

- **Remove** "C2 error pointer interpretation. Drive-specific behavior;
  not worth the complexity in v1." (Now v1.1.)
- Keep "Cdparanoia-grade jitter correction" deferred.
- **Add**: "Plextor `0xD8` lead-in reads — required only for HTOA
  recovery; out of scope."
- **Add**: "Drive cache-test runtime detection (cdparanoia's
  `cachetest.c`) — out of scope; AR/CTDB catches the resulting bad
  rips."
- **Add**: "MMC `SET STREAMING` (`0xB6`) for explicit read-cache
  control — out of scope; would require SCSITaskUserClient (which we
  don't use)."

### 4.8. In §"References"

Add these:

- Apple `IOStorageDeviceCharacteristics.h` — the IORegistry property
  keys (`kIOPropertyVendorNameKey`, etc.).
- Apple `IOCDTypes.h` — sector area sizes table at lines 194-204.
- Apple `IOCDMedia.h` (Kernel.framework) — `readCD()` doc comment
  documenting `byteStart` as bytes.
- Linux UAPI `include/uapi/linux/cdrom.h` — `CDROMREADAUDIO` and
  `CD_FRAMES = 75` cap.
- Linux UAPI `<scsi/sg.h>` — `sg_io_hdr_t` for SG_IO.
- Windows `ntddcdrm.h`, `ntddscsi.h` — SPTI definitions.
- MMC-6 spec (T10/1836-D Revision 2g, 11 December 2009) — §6.19
  `READ CD` Command, Tables 351-356.
- Microsoft Windows-driver-samples SPTI tool (MIT-licensed) — reference
  shape for `SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER`.
- AccurateRip drive offsets — `https://www.accuraterip.com/driveoffsets.htm`.

### 4.9. (New section) "Cross-platform shape"

Add a new section above "macOS approach":

> The same MMC `READ CD` (`0xBE`) command underlies all three platforms.
> macOS wraps it in `DKIOCCDREAD` (the kernel builds the CDB for us);
> Linux sends it via SG_IO + raw 12-byte CDB; Windows via SPTI + raw
> 12-byte CDB. The shared per-sector payload for v1 is
> `User Data + Error Flags` = 2646 bytes when C2 is enabled, 2352
> bytes when not. The CDB shape:
>
> ```
> Cdb[0]  = 0xBE                                   // READ CD
> Cdb[1]  = (1 << 2) | (0 << 1)                    // CDDA + DAP=0
> Cdb[2..5] = big-endian LBA (int32, signed-treated)
> Cdb[6..8] = big-endian transfer-length (sectors, uint24)
> Cdb[9]  = (1 << 4) | (wantC2 ? (1 << 1) : 0)     // User + optional C2
> Cdb[10] = 0x00                                   // no sub-channel
> Cdb[11] = 0x00                                   // control
> ```

---

## 5. Open questions (design judgments for the user)

1. **Drive offset re-rip behavior.** When AR's post-rip scan reports
   offset ≠ 0 (DB lookup was wrong), should v1: (a) silently re-rip with
   corrected offset, (b) prompt user, (c) keep at wrong offset and warn?
   Recommendation: **(a) auto-re-rip once, fall back to (b) if still wrong**.

2. **C2 retry budget.** Recommended ladder: 2 retries on ioctl failure +
   1 re-read on C2-non-zero (when C2 is enabled in v1.1). Sufficient?
   More aggressive = slower marginal-disc rips; less = more skipped
   sectors. Recommendation: ship the simple ladder, add knobs in
   `Ripper` config if user pushback materializes.

3. **Mixed-mode disc behavior.** Refuse loudly, or rip audio tracks and
   ignore the data track? Plan says "refuse loudly," which is
   defensible. **Recommendation: keep the plan's refuse-loudly stance**;
   the Rachmaninoff set is single-session audio only.

4. **Lead-in read failure.** Zero-pad silently + log, or fail loudly?
   Plan implies zero-pad. **Recommendation: zero-pad + log**; AR skip
   region absorbs it.

5. **C2 in v1.0 or v1.1?** Original report said v1.0; addendum said
   v1.1 (defer until basic flow proven). **Recommendation: ship v1.0
   without C2** — it matches libcdio's macOS default (which we know
   works), gets the AR/CTDB pipeline validated end-to-end, and the C2
   add is a 4-line edit later. Confirm preference.

6. **Pre-emphasis tag.** Detect + tag, or ignore? **Recommendation:
   detect + write `PRE_EMPHASIS=true` Vorbis comment when control bit 0
   is set.** Cheap to add; useful for the ~0.1% of discs that use it.

7. **USB bridge chipset masking.** When the bridge masks INQUIRY (e.g.,
   "JMicron" instead of "PIONEER"), no AR DB match → default offset 0
   → AR's post-hoc scan reports the real offset → re-rip if needed.
   **Recommendation: this is fine for v1**; add a "manual offset entry"
   UI in v2 if it becomes common.

8. **DAP=0 vs DAP=1.** **Recommendation: ship DAP=0 (raw bytes, no
   firmware concealment).** If a user reports their drive returns
   garbage at DAP=0, switch to DAP=1 for that specific drive (per-drive
   override). Document this in the rip log.

---

## 6. What to discard / what to keep

After folding this doc into the project, the two prior research docs
can be deleted. The state-of-the-world for each:

### `CD_READER_RESEARCH.md` — DELETE

- **What was correct and is preserved here:** the high-level shape
  (BSD ioctls, IORegistry walk, drive offset DB, AR cross-verify),
  the Linux SG_IO recommendation, the Windows SPTI recommendation, the
  cross-platform table, the `CdDevice` API sketch.
- **What was wrong and is corrected here:** `dk_cd_read_t.offset`
  units; IORegistry property keys (in the plan, not the report —
  but the report had them right); MMC CDB byte 1 layout
  (RELADR vs DAP); retry ladder (3+5+speed-down); SCSITaskUserClient
  entitlement claim; cdparanoia speed-down claim.
- **What was extrapolation:** the retry ladder and the speed-down
  recommendation. The addendum corrected these from source.

### `CD_READER_RESEARCH_ADDENDUM.md` — DELETE

- **What was correct and is preserved here:** all the source-grounded
  corrections (offset is bytes, DAP bit, cdparanoia 20-retry cap, no
  speed-down, redumper drive DB numbers, SCSITaskUserClient via MMC
  interface, libcdio's BSD-ioctl-only macOS default, pregap_start
  per-drive table).
- **What needed sharpening:** the cdparanoia algorithm description
  (correct at technique level but uses too many of paranoia's internal
  variable names — a clean-room v2 implementer should NOT use those
  names); the redumper DB size (under-estimated; corrected here);
  libcdio's macOS enumeration approach (slightly off; corrected here);
  the C2 BSD-ioctl-on-macOS empirical-evidence caveat (kept, important).

### `CD_READER_PLAN.md` — KEEP, but apply edits in §4

- **Conceptually correct.** The non-negotiables, the AR/CTDB
  validation harness, the deferred list (with the edits in §4.7) all
  stand.
- **Implementation specifics need updating** per §4.

### `CDRIP_STRATEGY.md` — KEEP unchanged

The strategy doc is the higher-level decision record (non-GPL stance,
native APIs, no jitter v1). Nothing here changes any of those
decisions. The Rachmaninoff findings, ID conventions, AR/CTDB math
gotchas, hard-won technical learnings — all still load-bearing and
not relitigated.

---

## 7. Appendix: where each source lives

For the implementer's convenience, the canonical sources cited above:

**Apple SDK headers (on user's machine):**
- `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/IOKit.framework/Headers/storage/IOCDMediaBSDClient.h`
- `.../IOKit.framework/Headers/storage/IOCDTypes.h`
- `.../IOKit.framework/Headers/storage/IOStorageDeviceCharacteristics.h`
- `.../Kernel.framework/Versions/A/Headers/IOKit/storage/IOCDMedia.h` (doc comments)
- `.../IOKit.framework/Headers/scsi/SCSITaskLib.h` (if/when SCSITaskUserClient is needed)

**Apple OSS source (for understanding only — APSL 2.0, permissive-ish):**
- `github.com/apple-oss-distributions/IOSCSIArchitectureModelFamily/UserClient/SCSITaskUserClientIniter.cpp` (type 0x05 exclusion)
- `github.com/apple-oss-distributions/IOSCSIArchitectureModelFamily/UserClientLib/SCSITaskLib.h` (kIOMMCDeviceUserClientTypeID)

**Linux UAPI:**
- `github.com/torvalds/linux/blob/master/include/uapi/linux/cdrom.h` (`CDROMREADAUDIO`, `CD_FRAMES=75`, `CD_FRAMESIZE_RAWER=2646`)
- `github.com/torvalds/linux/blob/master/drivers/cdrom/cdrom.c` (`mmc_ioctl_cdrom_read_audio`)
- `github.com/torvalds/linux/blob/master/include/scsi/sg.h` (`sg_io_hdr_t`)

**Microsoft (MIT-licensed reference):**
- `github.com/microsoft/Windows-driver-samples/blob/main/storage/tools/spti` (SPTI sample)
- Microsoft Learn: `IOCTL_SCSI_PASS_THROUGH_DIRECT`, `GUID_DEVINTERFACE_CDROM`.

**MMC spec:**
- `https://www.13thmonkey.org/documentation/SCSI/mmc6r02g.pdf` (T10/1836-D Rev 2g, 11 December 2009)
- §6.19 "READ CD Command" pages 362-369; Tables 351-356.

**GPL source — for understanding only, do NOT copy code:**
- `libcdio/libcdio/lib/driver/osx.c` (macOS BSD ioctl driver; default config uses no SCSITaskUserClient)
- `libcdio/libcdio/lib/driver/mmc/mmc_ll_cmds.c` (`mmc_read_cd` CDB construction)
- `libcdio/libcdio-paranoia/lib/paranoia/paranoia.c` (jitter algorithm)
- `libcdio/libcdio-paranoia/src/cd-paranoia.c` (offset/leadout handling)
- `superg/redumper/drive.ixx` (per-drive quirks DB)
- `superg/redumper/offsets.ixx` (~4626-entry AR offset table)
- `superg/redumper/scsi/sptd.ixx` (macOS SCSITask via MMC interface)

**AccurateRip drive offsets (public data):**
- `https://www.accuraterip.com/driveoffsets.htm`

**Hydrogenaudio (community docs):**
- `https://wiki.hydrogenaudio.org/index.php?title=AccurateRip`
- `https://wiki.hydrogenaudio.org/index.php?title=EAC_Drive_Options`
- `https://wiki.hydrogenaudio.org/index.php?title=Secure_ripping`
- `https://wiki.hydrogenaudio.org/index.php?title=Cdparanoia`
- `https://wiki.hydrogenaudio.org/index.php?title=Pre-Emphasis`

**xiph paranoia FAQ:**
- `https://www.xiph.org/paranoia/faq.html` (for v2 jitter-algorithm reference)

---

*End of authoritative reference. v1.0 implementation can proceed from here.*
