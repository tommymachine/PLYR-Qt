# CD-DA Reader: Native-API Research

Deep-research companion to `CD_READER_PLAN.md` and `CDRIP_STRATEGY.md`. This is the technique-and-API survey across macOS, Linux, and Windows, anchored in primary sources (Apple headers, the Linux UAPI, Microsoft docs, the public MMC spec landscape). The goal is to settle the open questions the plan implicitly raised, not to repeat what the plan already nailed.

Bottom line up front: **the plan is mostly right but materially wrong in three places.** (1) C2 error pointers ARE accessible via Apple's BSD ioctl (`kCDSectorAreaErrorFlags` is exposed from `IOCDTypes.h`), so we should request and use them in v1. (2) The IORegistry property keys are `Vendor Name` / `Product Name` under a `Device Characteristics` dictionary, not `Vendor Identification` / `Product Identification`. (3) Speed control (`DKIOCCDSETSPEED`) is a public BSD ioctl too, so retry-with-slowdown is one line and should be in v1. The rest of the plan stands.

---

## A. macOS: BSD ioctls vs SCSITaskUserClient

### A.1 What the BSD path actually gives us

Reading the Apple headers directly: `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/IOKit.framework/Headers/storage/IOCDMediaBSDClient.h` and `IOCDTypes.h`. These are public, non-deprecated, entitlement-free.

The BSD ioctl surface is broader than the plan suggests:

```
DKIOCCDREAD          (96)   READ_CD with sector area + sector type
DKIOCCDREADISRC      (97)   per-track ISRC
DKIOCCDREADMCN       (98)   disc MCN (UPC/EAN)
DKIOCCDGETSPEED      (99)   GET CD SPEED
DKIOCCDSETSPEED      (99)   SET CD SPEED (kB/s)
DKIOCCDREADTOC       (100)  READ TOC/PMA/ATIP (all formats)
DKIOCCDREADDISCINFO  (101)  READ DISC INFORMATION
DKIOCCDREADTRACKINFO (102)  READ TRACK INFORMATION
```

The `dk_cd_read_t` struct has `sectorArea` as a `uint8_t` and `sectorType` as a `uint8_t`. From `IOCDTypes.h`:

```c
typedef enum {
    kCDSectorAreaSync        = 0x80,
    kCDSectorAreaHeader      = 0x20,
    kCDSectorAreaSubHeader   = 0x40,
    kCDSectorAreaUser        = 0x10,
    kCDSectorAreaAuxiliary   = 0x08,
    kCDSectorAreaErrorFlags  = 0x02,   // <-- C2 error pointers
    kCDSectorAreaSubChannel  = 0x01,
    kCDSectorAreaSubChannelQ = 0x04
} CDSectorArea;
```

And the table comment in `IOCDTypes.h` lays out the per-area sizes for CDDA explicitly:

```
                  CDDA
Sync          | 0
Header        | 0
SubHeader     | 0
User          | 2352
Auxiliary     | 0
ErrorFlags    | 294
SubChannel    | 96
SubChannelQ   | 16
```

So a `sectorArea = kCDSectorAreaUser | kCDSectorAreaErrorFlags` (= 0x12) request, with `sectorType = kCDSectorTypeCDDA` (= 0x01), yields **2646 bytes per sector** (2352 user + 294 C2). Adding `kCDSectorAreaSubChannelQ` (0x04) gets you another 16 bytes for ISRC/index tracking. This is the same shape MMC `READ CD` (0xBE) produces — the BSD ioctl is a structured wrapper around exactly that command.

The kernel-side `IOCDMedia.h` (`/System/Library/Frameworks/Kernel.framework/Headers/IOKit/storage/IOCDMedia.h`) documents `readCD()` with the same parameter set, plus `readTOC`, `readISRC`, `readMCN`, `getSpeed`, `setSpeed`, `readDiscInfo`, `readTrackInfo`. The BSD ioctls are 1:1 wrappers around these. There is no documented kernel hook for `SET STREAMING` (MMC 0xB6), but `DKIOCCDSETSPEED` calls through to MMC `SET CD SPEED` (0xBB) which is sufficient for our use case.

**What you CANNOT do from the BSD ioctl path:**
- Send arbitrary MMC commands (e.g., feature queries, mode sense beyond what `readDiscInfo` exposes).
- Read the lead-in via Plextor's negative-LBA trick (which uses a non-standard `READ CDDA` / `D8` command — that's a Plextor-only extension and irrelevant here).
- Mode select to disable read-ahead caching (some drives expose this via Mode Page 0x2A); irrelevant for v1.

`pread()` on `/dev/rdiskN` at sector-aligned offsets reads logical 2048-byte blocks, which makes zero sense for CD-DA. CDDA sectors are 2352 bytes and the kernel's pread path is wrong for that. Use `DKIOCCDREAD`. The plan's "Open the *raw* char device" advice is correct insofar as the ioctl needs an open file descriptor, but the device shape isn't relevant — the ioctl interprets the request, not pread.

**Recommendation:** stay on BSD ioctls; request `kCDSectorAreaUser | kCDSectorAreaErrorFlags` (= 0x12) so we get C2 alongside audio for free; treat any non-zero byte in the 294-byte C2 area as a sector-level error signal; trigger re-read on it. Add `setReadSpeed(kBps)` via `DKIOCCDSETSPEED` for the retry path. Don't bother with subchannel/ISRC/MCN in v1 — not in scope.

**Caveats:** the 294-byte error-flags layout is per-MMC: bit `(7 - n%8)` of byte `(n / 8)` is the C2 flag for user-data byte `n`. Drive-reported C2 accuracy is patchy (see G). Treat C2 as a "if non-zero, definitely re-read; if zero, *probably* fine but not guaranteed."

### A.2 SCSITaskUserClient — entitlement reality in 2026

`SCSITaskLib.h` lives in the SDK still (`/System/Library/Frameworks/IOKit.framework/Versions/A/Headers/scsi/SCSITaskLib.h`), unchanged since 2009 per its header copyright. The interface IDs (`kIOSCSITaskDeviceInterfaceID`, `kIOMMCDeviceInterfaceID`) are stable. The API is **not** formally deprecated.

But: to actually open one of these user clients on a signed/notarized app you need an entitlement. Apple's DriverKit guidance is that *new* drivers need DriverKit entitlements like `com.apple.developer.driverkit.transport.scsi`, granted only after a manual request through Apple's developer-services portal. Apple's public stance from WWDC sessions and the Driver Extensions guidance is: kernel-extension SCSI interfaces are legacy, DriverKit replaces them, and the entitlements are gated. Anecdotally on forums (newosxbook.com entitlement database; pqrs Karabiner-DriverKit project notes) the SCSI transport entitlement is gettable but enterprise-flavored — not casual.

The IOSCSIArchitectureModelFamily user-client path that `SCSITaskUserClient` represents predates DriverKit. The libcdio and morituri/whipper history shows it once worked without entitlements on unsigned binaries; modern macOS (since 10.13, escalated through 11+ and Apple Silicon) refuses on signed apps unless entitled. Apple Developer Forums thread #650611 confirms IOSCSIArchitectureModelFamily classes are not yet supported in DriverKit and that this is "to be added in a future release" — meaning the modern SCSI passthrough story is incomplete and the legacy path is the only path, but it's gated.

**Recommendation:** stay off SCSITaskUserClient for v1. The BSD ioctls cover every CD-DA read path we actually need. Reconsider only if (a) the entitlement becomes routinely gettable, or (b) we hit a drive feature we can't access via BSD ioctls (we don't expect to).

**Caveats:** unsigned dev builds *can* open SCSITaskUserClient without entitlements — useful for experimentation but not for ship. Don't write code paths that only work in dev.

### A.3 IORegistry path for vendor/product

The plan's path is conceptually right but the property names are wrong. Reading `IOStorageDeviceCharacteristics.h`:

```c
#define kIOPropertyVendorNameKey            "Vendor Name"
#define kIOPropertyProductNameKey           "Product Name"
#define kIOPropertyProductRevisionLevelKey  "Product Revision Level"
```

These live in a dictionary at the `Device Characteristics` key on the IOMedia's parent device nub. So to read them, walk up from the IOCDMedia (which DA hands you via `kDADiskDescriptionMediaPathKey` or `DADiskCopyIOMedia`), one or two parents up to find the SCSI/ATAPI device nub, get its `Device Characteristics` dictionary, then `Vendor Name` and `Product Name`. The plan said the keys were `Vendor Identification` and `Product Identification` — those are the raw INQUIRY field labels in the MMC/SAM spec, but Apple's IORegistry uses friendly names.

For USB-attached drives (the only kind anyone owns now), the IORegistry chain looks roughly:

```
IOUSBHostDevice (Vendor ID, Product ID — numeric USB)
  └─ IOUSBHostInterface
       └─ IOUSBMassStorageInterfaceNub
            └─ IOSCSIPeripheralDeviceNub  (Device Characteristics dict here)
                 └─ IOSCSIPeripheralDeviceType05  (peripheral type 5 = MMC/optical)
                      └─ IOCDBlockStorageDriver
                           └─ IOCDMedia    (this is what DA gives you)
```

The `Device Characteristics` dict lives on the `IOSCSIPeripheralDeviceNub` or `IOSCSIPeripheralDeviceType05`. The strings come from the drive's MMC INQUIRY response, so they match the AccurateRip drive-offset DB strings (which were also captured from INQUIRY).

**Recommendation:** edit the plan to use `Vendor Name` / `Product Name` (the literal IORegistry keys). Walk up from `IOCDMedia` via `IORegistryEntryCreateIterator(... kIORegistryIterateParents | kIORegistryIterateRecursively ...)` and search for a `Device Characteristics` dictionary; pull the two strings out. Trim whitespace; uppercase for the AR DB lookup.

**Caveats:** some external USB enclosures sit a bridge chipset between the OS and the optical drive's actual ATAPI controller. In those cases the strings in `Device Characteristics` reflect the *bridge*, not the drive (e.g., "JMicron" rather than "PIONEER"). The AccurateRip DB is unhelpful here. v1 should fall back to "rip at offset 0, run the post-hoc AR offset scan that `arverify` already implements." Don't try to be clever.

### A.4 DAClaim vs O_EXLOCK

Both exist, both work, they prevent different things.

`DAClaim` prevents *another DiskArbitration client* from getting a claim. It's the polite-protocol layer. It also lets you register approval callbacks (so you can refuse a mount/unmount from inside your process). Music.app and Finder go through DA, so a `DAClaim` blocks them.

`O_EXLOCK` on `open(2)` of `/dev/rdiskN` is a flock-style exclusive lock at the BSD-vnode level. It prevents other `open(... O_EXLOCK)` and most importantly prevents the disk-image arbitration daemon and other low-level callers from competing.

The Disk Arbitration Programming Guide says: "if you are working with burned optical media, you do not need to use the Disk Arbitration framework" — implying audio CDs (which don't auto-mount in macOS) don't *strictly* need DA-mediated exclusivity. But Music.app does claim the disc when it autoplays.

**Recommendation:** do both, belt-and-suspenders. (1) `DASessionCreate` + `DARegisterDiskDescriptionChangedCallback` for discovery; (2) `DADiskCreateFromBSDName` + `DADiskClaim` with an empty release callback to take the polite claim; (3) `open("/dev/rdiskN", O_RDONLY | O_EXLOCK | O_NONBLOCK)` for the actual ioctl FD. If anything else has the device open exclusively, the open fails with EAGAIN/EBUSY, which is the correct signal to the user.

**Caveats:** `O_NONBLOCK` is important — without it `open(O_EXLOCK)` blocks indefinitely. With it, an `EAGAIN` is a clear "drive busy, ask the user to quit Music.app." After the open succeeds you may want to `fcntl(F_SETFL, ...)` to drop `O_NONBLOCK` for the actual ioctl, but the ioctl is synchronous either way so it's belt-and-suspenders again.

### A.5 Audio CD auto-mount

Pure audio CDs (red book single-session) have no filesystem and don't auto-mount. The IOCDMedia node appears in IORegistry but there's no IOFilesystem on top. DA still publishes them and Music.app gets a notification — that's the autoplay vector, not a mount.

Hybrid / Mixed-mode discs:
- **Mixed-mode CD** (data track + audio tracks in track 1 data, tracks 2-N audio): the data track is published as a separate IOMedia child of IOCDMedia. macOS auto-mounts the data track's filesystem (typically ISO9660 or HFS). DA fires for the mount. This *does* fight you — Finder may open a window, the FS is read-only-mounted, and any read against the audio sectors via the same device may compete with FS read-ahead.
- **CD Extra / Blue Book / Enhanced CD** (audio tracks first, data track in a second session): the data session is published separately. macOS may auto-mount that too. Same issue.

**Recommendation:** v1 does *single-session audio-only CDs only*. Detect mixed/extra via `DKIOCCDREADTOC` parsing — if any track's `control` field has bit 2 set (data track), bail with "mixed-mode not yet supported." This is consistent with the plan's "Deliberately deferred" list.

**Caveats:** the bail-out should be a clean user-facing error, not a crash. Plan ahead for the eventual "support mixed-mode" feature: it's possible — you just have to read the audio tracks through the BSD ioctl and ignore the auto-mounted data partition.

---

## B. Linux: how thin is the path?

### B.1 CDROMREADAUDIO on modern kernels

The interface still exists in mainline (`include/uapi/linux/cdrom.h`):

```c
struct cdrom_read_audio {
    union cdrom_addr addr;   /* frame address */
    __u8  addr_format;       /* CDROM_LBA (0x01) or CDROM_MSF (0x02) */
    int   nframes;           /* number of 2352-byte frames to read */
    __u8 __user *buf;        /* output buffer: nframes * 2352 bytes */
};

#define CDROMREADAUDIO  0x530e   /* the ioctl number */
```

Per the kernel docs page, the ioctl returns EINVAL if `nframes` is outside `[1, 75]`. That's a hard 75-sector-per-call limit (~1 second of audio). For a 74-minute disc that's ~4400 ioctls just to read everything end-to-end. Fine for v1.

The ioctl is still wired up in `drivers/cdrom/cdrom.c` and works on `/dev/sr0` style sg-emulating block devices. Modern udev typically creates `/dev/sr0`, `/dev/sr1`, etc., plus a `/dev/cdrom` symlink for the first detected optical device. The plan should target `/dev/sr*` directly via udev enumeration (libudev or `/sys/class/block`), not the symlink.

### B.2 SG_IO as an alternative

Linux's SG_IO ioctl lets you send arbitrary SCSI/MMC commands to the device, replicated to block devices in 2.6+ so it works on `/dev/sr0` directly without the older `/dev/sg*` mapping. The `sg_io_hdr_t` struct (in `<scsi/sg.h>`) carries a CDB pointer, data direction, transfer buffer, sense buffer, timeout. From sg.danny.cz:

```c
typedef struct sg_io_hdr {
    int     interface_id;       /* [i] 'S' */
    int     dxfer_direction;    /* [i] SG_DXFER_FROM_DEV etc */
    unsigned char cmd_len;      /* [i] CDB length 6..16 */
    unsigned char mx_sb_len;    /* [i] max sense bytes */
    unsigned short iovec_count; /* [i] 0 = single buffer */
    unsigned int dxfer_len;     /* [i] data transfer length */
    void    *dxferp;            /* [i],[*io] data buffer */
    unsigned char *cmdp;        /* [i] pointer to CDB */
    unsigned char *sbp;         /* [i] pointer to sense buffer */
    unsigned int timeout;       /* [i] ms */
    /* ... return fields ... */
} sg_io_hdr_t;
```

Advantages of SG_IO over CDROMREADAUDIO:
- **C2 error flags available**: send `READ CD` (0xBE) with the error-flags bit set; get the 294-byte C2 alongside. CDROMREADAUDIO doesn't expose C2.
- **No 75-sector cap**: SG_IO lets you transfer larger buffers (subject to drive max-transfer limits, typically 64KB or more).
- **Direct sense data**: lets you distinguish "BUSY", "MEDIUM ERROR at LBA X", "ILLEGAL REQUEST" etc., which CDROMREADAUDIO collapses to errno.

Permissions: `/dev/sr0` is typically root:cdrom mode 660 on Debian/Ubuntu; mode 664 on Arch. The user must be in the `cdrom` group, OR udev rules must grant the user write access. CAP_SYS_RAWIO is required for *some* SG_IO commands (mostly write-side and mode-changing); plain `READ CD` with O_RDWR open generally works for unprivileged users in the cdrom group. The kernel filters which CDBs an unprivileged user can send via `scsi_blk_cmd_ioctl` — `READ CD` (0xBE) and `READ TOC` (0x43) are in the safe set.

**Recommendation:** use SG_IO directly with `READ CD` (0xBE). Treat CDROMREADAUDIO as a fallback if SG_IO fails (it never should on modern kernels, but the legacy path is one-line). Open `/dev/sr0` with `O_RDWR | O_NONBLOCK`. Document the cdrom-group requirement; ship an udev rule file in the project's `packaging/linux/` for distros that don't default-grant.

**Caveats:** the per-ioctl transfer size is capped by the drive's reported max-transfer-length (returned in INQUIRY VPD page 0xB0, or implicitly limited to ~64KB on USB bridges). 26 sectors per call (2352 * 26 = 61,152 bytes) is a safe default. Some Linux distros' `tmpfs` /dev limits SG_IO to root by default; check `/sys/class/block/sr0/device/scsi_generic/*/uevent` for the matching `/dev/sg*` if needed.

### B.3 C2, speed control, subchannel on Linux

- **C2 error pointers**: SG_IO + `READ CD` with the error-flags bit (CDB byte 9 bit 1) gets you the 294-byte payload exactly as on macOS.
- **Speed control**: `MMC SET CD SPEED` (0xBB) over SG_IO. There's also the legacy `CDROM_SELECT_SPEED` ioctl (0x5322) which wraps it. Use the legacy ioctl — one line.
- **Subchannel**: `MMC READ SUB-CHANNEL` (0x42) over SG_IO, or the legacy `CDROMSUBCHNL` (0x530b) ioctl. Not in v1 scope.

### B.4 Kernel changes in the last ~5 years

Nothing major affects audio CD reading. The CDROM driver hasn't been deprecated, SG_IO is solid, the UAPI hasn't changed shape. There's been ongoing work on async I/O and io_uring support for block devices that doesn't apply to ioctl-based CD reads. The `cdrom_generic_command` path (CDROM_SEND_PACKET) is still there but SG_IO is preferred — same wire protocol, cleaner ioctl shape.

**Recommendation (Linux overall):** SG_IO + `READ CD` (0xBE) is the right path. Mirror the macOS implementation's shape — request `User Data + C2` (2352 + 294 = 2646 bytes per sector); same retry-on-C2-nonzero logic; same drive-offset DB lookup using INQUIRY vendor/product strings (which on Linux you read via SG_IO + `INQUIRY` (0x12), or via `/sys/block/sr0/device/{vendor,model,rev}` which the kernel already populated from INQUIRY at probe time).

**Caveats:** drive-name strings via sysfs (`/sys/block/sr0/device/vendor` etc.) are convenient but truncated/stripped by the kernel in a slightly different way than Apple's IORegistry copies them. For AccurateRip DB matching, prefer sending INQUIRY ourselves and reading the raw bytes 8-15 (vendor) and 16-31 (product); strip trailing spaces; match.

---

## C. Windows: SPTI in 2026

### C.1 SPTI is still the right path

`IOCTL_SCSI_PASS_THROUGH_DIRECT` (`ntddscsi.h`) is the standard Win32 path for sending SCSI/MMC commands to an optical drive. From MS docs: works on a handle opened to a CD-ROM device interface (`\\.\CdRom0`, `\\.\E:`, etc.), allows almost any SCSI CDB, supports direct data transfer (DMA-friendly). No driver-signing implications for the *application*; the user just needs to be Administrator or the device ACL must grant access. For MSIX packages, sending SPTI commands works from a non-Administrator app *if* the drive is accessible via the user's session — Microsoft's storage stack has loosened up on this over the years.

The Microsoft SPTI sample at `microsoft/Windows-driver-samples/storage/tools/spti` is MIT-licensed (the Windows-driver-samples repo is MIT) — that's a permissively-licensed reference you can actually read for the `SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER` shape.

### C.2 Sending READ CD over SPTI

Build a `SCSI_PASS_THROUGH_DIRECT` with `Cdb[0] = 0xBE`, `CdbLength = 12`, `DataIn = SCSI_IOCTL_DATA_IN`, `DataBuffer = ...`, `DataTransferLength = sector_count * (2352 + 294)`, `SenseInfoLength = 32`, `SenseInfoOffset = offsetof(struct_with_sense, Sense)`, `TimeOutValue = 30`. Standard shape.

Sense data on failure: the SPTI infrastructure populates the sense buffer when `ScsiStatus` != 0 (typically `0x02` = check condition). Parse sense key + ASC + ASCQ as usual.

The "gotchas" with raw reading: many older Windows-storage write-ups mention IOCTL_CDROM_RAW_READ as a starting point. Don't use it for CD-DA. It works (you pass `TrackMode = CDDA`, get 2352-byte sectors), but it's a lower-fidelity wrapper that doesn't expose C2, and on some drives the storage driver substitutes a different MMC command than you'd want. SPTI direct is cleaner. (PCSX2 issue #648 documents real-world failures of `IOCTL_CDROM_RAW_READ` on certain games; SPTI is the workaround.)

### C.3 Drive enumeration without WMI

Don't use WMI. Use SetupAPI:

1. `SetupDiGetClassDevs(&GUID_DEVINTERFACE_CDROM, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)`.
2. `SetupDiEnumDeviceInterfaces` in a loop.
3. `SetupDiGetDeviceInterfaceDetail` to get the symbolic link (e.g., `\\?\ide#cdromTSSTcorp...`).
4. `CreateFileW(symLink, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)`.
5. Issue SPTI ioctls on the handle.

GUID for CD-ROM: `GUID_DEVINTERFACE_CDROM = {53F5630D-B6BF-11D0-94F2-00A0C91EFB8B}`. Defined in `ntddcdrm.h`.

### C.4 Vendor/product

Two options:
- **Via SPTI INQUIRY**: send a 36-byte `INQUIRY` (0x12); bytes 8-15 are vendor, 16-31 are product. Match the AR DB strings.
- **Via SetupAPI device properties**: `SetupDiGetDeviceRegistryProperty(SPDRP_FRIENDLYNAME)` or `SPDRP_HARDWAREID` give pre-baked strings. For AR DB matching, INQUIRY directly is more reliable — the SetupAPI strings sometimes have extra decoration.

**Recommendation:** SetupAPI for enumeration; CreateFile + SPTI for reads; INQUIRY for the AR DB lookup. Mirror macOS/Linux's read code path (READ CD with User Data + C2 = 2646 bytes/sector).

**Caveats:** on Windows, `\\.\CdRom0` requires Administrator handle for raw SCSI passthrough on some legacy storage paths but the SetupAPI-derived symbolic link path works for unprivileged users in default ACLs on Win10/11. Validate this on a non-admin user account before claiming it works.

---

## D. The MMC READ CD command, deeply

This is the cross-platform anchor. The plan implicitly used it on macOS (the BSD ioctl is a wrapper); we'll use it explicitly on Linux/Windows.

### D.1 Byte layout (CDB, 12 bytes)

```
Byte 0:  0xBE                                 (opcode)
Byte 1:  (Expected Sector Type << 2) | RELADR (RELADR = 0; obsolete in MMC-3+)
         Expected Sector Type values:
           0 = Any                            (drive doesn't enforce)
           1 = CD-DA                          (refuses to read data sectors as audio)
           2 = Mode 1
           3 = Mode 2 (formless)
           4 = Mode 2 Form 1
           5 = Mode 2 Form 2
Bytes 2-5: Starting LBA, big-endian uint32
Bytes 6-8: Transfer length in sectors, big-endian uint24
Byte 9:  Flag byte
           bit 7  Sync                        (12 bytes, useless for CDDA)
           bit 6:5 Header Code                (00 = none; CDDA = 00)
           bit 4  User Data                   (1 = include 2352 user bytes)
           bit 3  EDC/ECC                     (data sectors only)
           bit 2  RESERVED
           bit 1  Error Flags                 (1 = include 294-byte C2 block)
           bit 0  RESERVED
Byte 10: Sub-channel Selection
           000 = none
           001 = raw 96-byte P-W subchannel
           010 = Q-only (16 bytes)
           100 = R-W (corrected)
Byte 11: Control byte (0)
```

So for CDDA: `Cdb[1] = 1 << 2 = 0x04`, `Cdb[9] = 0x10` (User Data only) or `0x12` (User Data + Error Flags) or `0x14` (... reserved-bit + Error Flags? no — 0x12 is correct), and `Cdb[10] = 0x00` for v1.

### D.2 Why "Expected Sector Type = CDDA"

Setting type 1 (CDDA) makes the drive refuse to return data sectors as audio. This guards against accidentally ripping the data track of a mixed-mode disc as 2352-byte chunks (which would produce garbage WAV data). Setting type 0 ("any") works but is slightly less safe. Some buggy drives (per redumper's documentation, "good LG/ASUS" drives) ignore the constraint and let you read data sectors as CDDA anyway — useful for archival imaging tools but not for us.

**Recommendation:** always pass type = CDDA (1). If we hit a data track during a v1 rip we want to fail loudly, not produce hash.

### D.3 Per-sector payload arithmetic

For CDDA:
- User Data only: 2352 bytes
- User Data + Error Flags: 2352 + 294 = **2646 bytes**
- User Data + Q-subchannel: 2352 + 16 = 2368 bytes
- User Data + Error Flags + Q-subchannel: 2352 + 294 + 16 = 2662 bytes
- User Data + Error Flags + raw subchannel: 2352 + 294 + 96 = 2742 bytes

The drive returns exactly that much per sector. Buffer allocation is `n_sectors * bytes_per_sector`. For v1 we want 2646 (audio + C2).

### D.4 READ CD MSF (0xB9)

Same payload structure but addresses by MSF (minute/second/frame) instead of LBA. Almost never preferable — LBA addressing is the modern convention, MSF only matters if you're already working in MSF coordinates from somewhere upstream (a TOC parse). Don't use it.

### D.5 READ(10) for CD-DA

`READ(10)` (0x28) is the generic block-mode read. On a CD-DA track, drives that follow MMC strictly return 2048-byte ROM blocks per sector — i.e., wrong. Some drives' firmware switches to 2352-byte transfer when the drive sees a CDDA track, but this is non-standard and unreliable. **Don't use READ(10) for CD-DA**; always use READ CD (0xBE).

**Recommendation:** wrap READ CD (0xBE) shape identically across all three platforms. Same CDB, same expected sector type (CDDA), same flag byte (0x12). Linux sends it via SG_IO, Windows via SPTI, macOS via DKIOCCDREAD which is a structured wrapper around the same command.

**Caveats:** the 12-byte CDB above is MMC-5+ standard. MMC-2 was a similar shape with one fewer flag bit (no error flags in early drafts) — but every drive in the wild speaks MMC-3 or later. Don't worry about it.

---

## E. Drive-offset correction — the actual algorithm

### E.1 Concrete read range for offset = +6

A drive with offset +6 reads "6 samples earlier than asked." To get sample-accurate output, you ask for 6 samples *later* than the canonical track start.

For track N starting at LBA = `start_lba` (in sectors, 588 samples each):
- Canonical first sample at sample index = `start_lba * 588`
- With drive offset +6, ask the drive for samples starting at `start_lba * 588 + 6`

In practice: read full sectors. Read sectors `[start_lba, end_lba)`. Skip the first 6 samples (the offset) at the start of the buffer. Read one extra sector at the end (`end_lba` inclusive) to cover the trailing 6 samples. Slice `[6, total_samples)` to get the canonical track.

For track 1 starting at LBA 0 with offset = +6: ask for samples starting at sample 6. That's sector 0, skip 6 samples = trivial.

For a negative offset like -670, track 1 wants samples starting at `0 * 588 - 670 = -670`. That's *before LBA 0*, i.e., in the lead-in. Sector -2 (LBA -2) contains the bytes from frames around the start of the audio data. Most drives let you read 1-2 seconds into the lead-in (~150 sectors) but the exact limit is drive-specific:
- Plextor (with custom commands) can read all 150 sectors of pregap + RAW TOC.
- LG/ASUS/Lite-On drives typically read ~135 sectors of pregap (just shy of full 150).
- Many slim/laptop USB drives refuse negative-LBA reads entirely and either return a sense error or silently return zeros.

For offset = +6 on the last track: similar story but on the lead-out side. Ask for one sector past `last_track_end_lba`. Drives that support "overread into lead-out" do so up to ~75 sectors typically. Drives that don't, return silence (zeros), which is fine — the AR checksum already skips the last 5 sectors of the last track for exactly this reason.

### E.2 Reading past the boundaries — what really happens

From Hydrogenaudio and the BASS_CD documentation: drives respond to lead-in/lead-out reads in three ways:
1. **Fail with sense data** (medium error or illegal request). Software detects, falls back to zero-padding.
2. **Pad with digital silence** (return zeros). Software sees a successful read of zeros.
3. **Return actual lead-in/lead-out data** (good drives — Plextor in particular).

Behavior is per-drive, sometimes per-firmware. The AccurateRip checksum skip regions (first 2940 + last 2940 samples = 5 sectors each side) were chosen specifically so that even if you can't overread the boundaries, you can still verify the rest of the disc bit-accurately.

### E.3 Sample-accurate slicing — confirm the approach

Yes — read whole sectors (smallest atomic read unit on CD), then slice in software. Sectors are 588 samples. Discard the fractional samples at the start (`offset` samples) and end (`588 - offset`) to land exactly on the canonical track boundaries. This is what every CD ripper does — there's no other way given the sector-aligned hardware.

For a track of length `L` samples and drive offset `+O` (positive):
- Read sectors covering samples `[O, L + O]` — i.e., `ceil((L + O) / 588)` sectors starting at `floor(O / 588)`.
- Actually it's cleaner: read `[start_sector_canonical, end_sector_canonical + 1)` with offset-O shift, then `output = read_buffer[O*4 : O*4 + L*4]` (where `*4` is the bytes-per-sample factor for stereo 16-bit).

For `O = +6`: read `[start_sector, end_sector + 1)`, skip 24 bytes (6 samples × 4 bytes) at the buffer start, take exactly `L*4` bytes.

For `O = -670` on track 1 (start_lba = 0):
- Need samples `[-670, L - 670]`.
- Start sector = floor(-670 / 588) = -2 (since -670 / 588 = -1.14, floor is -2).
- End sector = ceil((L - 670) / 588).
- Read sectors `[-2, ceil((L-670)/588))`.
- The drive may not let us read LBA -2. Pad with zeros if it fails. The AR skip region covers it.

**Recommendation:** v1 implements offset correction at read time using the formula above. Treat lead-in/lead-out reads that fail with sense data as "return zeros, log a warning" — don't fail the whole rip. AR skip regions absorb this.

**Caveats:** Some drives report a successful read but return cached data from the *previous* sector when you ask for an out-of-range LBA. This is silently wrong. There's no good defense in v1 — the AR/CTDB verification will catch it (or won't, in the worst case). C2 doesn't help here because the drive doesn't think it's an error.

### E.4 Rip-at-offset-0 then re-rip? Or apply offset on first pass?

Tradeoff:
- **Apply offset on first pass** (current plan): one rip, lands at offset 0 against AR's reference. Requires correct DB lookup. Fails badly if the DB entry is wrong (silent misalignment of the final FLAC).
- **Rip at 0 then re-rip if needed**: two CD reads worst case, but the AR offset scan we already have detects the actual offset, and a second pass with the corrected offset gives bit-accurate output. More robust to DB errors / unknown drives.
- **Hybrid (recommended)**: rip with the DB offset applied. AR-verify post-rip. If AR verifies bit-accurate at offset 0 (i.e., AR thinks our offset is 0), done. If AR offset-scan finds a non-zero offset, that means our DB-lookup-derived offset was wrong by the AR-reported delta — log a warning, optionally re-rip with the corrected offset. Don't silently accept misalignment.

The post-hoc offset scan is essentially free (the verifier does it anyway). The cost of a wrong DB offset is a single re-rip on a small fraction of discs. Net: hybrid is the right answer.

**Recommendation:** v1: apply offset on first pass from the DB; AR-verify after; if AR's detected offset != 0, re-rip with the corrected offset; if still != 0 after that, mark the rip as offset-uncertain and let the user decide. This matches the plan's MVP step 6 + step 8.

**Caveats:** for drives with no DB entry, default to offset 0 and rely on the post-hoc detection. Document this. Don't try to be heuristic about substring matching against the DB.

---

## F. Error handling and re-read — clean-room technique

### F.1 What an "error" looks like

Across platforms:
- **macOS** `DKIOCCDREAD` returns -1 with `errno` set to `EIO` (medium error), `EBUSY` (drive busy), `ENXIO` (no media), `EINVAL` (parameters). The actual SCSI sense data isn't surfaced through this ioctl — it's collapsed to errno.
- **Linux** SG_IO returns 0 on success but the `sg_io_hdr_t` has `status`, `host_status`, `driver_status`, and a sense buffer. Real signal lives in sense data: sense key + ASC/ASCQ. Medium error = sense key 0x03; the ASC/ASCQ pair narrows it (e.g., 0x11 0x05 = L-EC uncorrectable error).
- **Windows** SPTI fills in `ScsiStatus` and the sense buffer; same sense-data shape as Linux. Win32 also returns `GetLastError()` codes (ERROR_IO_DEVICE, ERROR_NO_DATA_DETECTED).
- **All three**: a short read (fewer sectors than requested) without any failure code is possible on some drives — always check the returned byte count.

The v1 "error" signal is therefore: any ioctl-level failure, OR a short read, OR (when C2 is requested) any non-zero byte in the 294-byte C2 area for any sector in the response.

### F.2 cdparanoia-grade jitter correction — technique only

(Restating from xiph.org/paranoia and Hydrogenaudio writeups; we are NOT going to implement this in v1 but we should know the shape.)

The technique exists because some older drives, especially cheap ones, can't read CD-DA atomically: a single READ(N) command may return data that's been *internally rebuilt* by the drive from a few re-attempts, with sample-level skips or duplicates at the seam. This is "jitter." Modern drives with "Accurate Stream" don't exhibit this — they return positionally-stable data.

The conceptual algorithm (clean-room reimplementation guidance for some future v2):
1. Read overlapping ranges: read sectors `[A, B]` and `[B-K, C]` where the K-sector overlap is enough to find a match.
2. In the overlap region, sample-domain cross-correlate the two reads. If they align cleanly (zero shift), the drive is stable on this disc — accept the data.
3. If the overlap shows a non-zero shift, the drive has jittered — extract the actual sample offset, splice the data accordingly.
4. If the overlap doesn't correlate at all, re-read both ranges from scratch.
5. Dynamic overlap size: start with a few sectors; if jitter is detected, expand; if reads are consistent for a while, shrink to amortize speed.

The output is a "verified contiguous PCM stream" built from confirmed-aligned reads.

**v1 should NOT do this.** It buys ~nothing on modern Accurate-Stream drives, costs significant complexity, and AccurateRip cross-verification catches jitter-induced errors post-hoc anyway (any jitter manifests as a checksum mismatch). Defer.

### F.3 v1 retry path

Recommended pattern:
1. Read N sectors via the platform's READ CD path with User Data + C2.
2. If ioctl fails or short-read: classify the errno/sense to "transient" (BUSY, retry) vs "permanent" (medium error, retry then bail).
3. On retry: same read, no slowdown for first N retries.
4. If still failing after, say, 3 attempts: drop drive speed to 4x via SET CD SPEED, drain any drive cache (re-open the FD), retry up to 5 more times.
5. If C2 reports errors but ioctl succeeds: re-read the affected sector once at current speed; if C2 still reports, re-read at 4x.
6. If still bad after all that: fill that sector with zeros, mark the rip log with the LBA and the per-sector C2 mask, continue.

Why slow down? At lower spindle speeds the drive's PLL has more time per read, the laser has more time per sample, and read errors drop dramatically for marginal media. EAC documents this; every secure-mode ripper does it. 4x is the empirical sweet spot — 1x is overkill, 2x is sometimes too slow for the drive's firmware to even support.

Why re-open the FD? Some USB-attached drives stall after a sense error and need a SCSI START STOP UNIT (or just an FD close/reopen) to recover. Empirically known among optical-tool authors.

**Recommendation:** v1 ships with the retry+slowdown ladder above. `setReadSpeed(int kBps)` should be a method on `CdDevice`, exposing `DKIOCCDSETSPEED` on macOS, `CDROM_SELECT_SPEED` on Linux, MMC SET CD SPEED via SPTI on Windows. Default speed = drive max (let the drive decide); fallback = 706 kB/s (4x).

**Caveats:** speed limits don't persist across media changes; reset on each disc. Some drives ignore SET CD SPEED entirely (return success, change nothing). Don't depend on it being honored — it's a hint.

### F.4 C2 use, v1-grade

The simple rule: **if any C2 byte in the response is non-zero for any sector, re-read that sector.** Don't try to interpret which bytes failed; just trigger the retry. After max retries: log the sector and the per-byte C2 mask, fill with zeros, mark the rip as "partial."

The complex rules (interpolation, parity from CTDB, etc.) are out of scope for v1.

Drive C2 accuracy varies. Some drives miss C2-flagged sectors entirely (firmware bug). Some report C2 on every read regardless of disc condition (firmware bug, the other way). For v1 we treat C2 as a strong hint: trust it when set; don't trust its absence as a guarantee. Cross-verification via AR/CTDB is the actual safety net.

---

## G. The "post-CIRC PCM" claim

The plan asserts: drives do Viterbi + CIRC Reed-Solomon internally; the host sees only post-correction PCM; software can't redo EC.

This is correct and the assertion should stand:
- The MMC spec exposes `READ CD` with sector areas including User Data (post-CIRC PCM) and Error Flags (the *result* of CIRC, the C2 pointers). It does NOT expose pre-CIRC raw channel bits.
- No CD-DA drive in the consumer/external-USB universe exposes raw channel bits via MMC. There are no spec extensions for this.
- Plextor (deceased, the brand has been zombified) had a couple of legacy commands (D8) that read CDDA data in a slightly different way that bypassed some firmware offset correction, but it still operated on post-CIRC PCM — just with the offset-correction firmware disabled. Not raw channel bits.
- Industrial / forensic / mastering hardware can capture raw channel bits, but at the analog stage; not relevant for any consumer ripping software.

What we CAN see post-CIRC:
- The PCM bytes themselves.
- C2 error flags: 294 bytes per sector, one bit per data byte, "this byte was uncorrectable per CIRC and was concealed by the drive (typically with interpolation or last-good-sample repeat)."
- Q-subchannel: timecodes, ISRC, MCN (not needed for v1).

**Recommendation:** the plan's assertion is correct. Keep it in the doc as is.

**Caveats:** the claim "no software path can redo error correction" is sometimes nuanced — *some* secondary repair is possible (CTDB stores parity, can repair small errors). But that's not "redoing CIRC"; it's an external parity scheme on top of the post-CIRC PCM. Out of scope for v1.

---

## H. The cross-platform `CdDevice` abstraction

Opinionated sketch:

```cpp
namespace plyr::cd {

struct DriveInfo {
    std::string id;            // platform-specific opaque handle string
                               //   macOS: BSD name "disk2"
                               //   Linux: "/dev/sr0"
                               //   Win:   `\\?\...` symbolic link
    std::string vendor;        // INQUIRY vendor, AR-DB matching form
    std::string product;       // INQUIRY product, AR-DB matching form
    std::string revision;      // INQUIRY rev, informational
    bool hasMedia;
};

struct TocEntry {
    uint8_t  trackNumber;      // 1..N
    uint32_t startLba;         // LBA, NOT absolute MSF
    bool     isData;           // control bit 2
    bool     preEmphasis;      // control bit 0
};

struct Toc {
    std::vector<TocEntry> tracks;
    uint32_t leadOutLba;
};

enum class ReadStatus {
    Ok,
    TransientBusy,     // retry
    MediumError,       // re-read recommended
    OutOfRange,        // attempted negative LBA past drive's limit, etc.
    Aborted,           // user cancelled / device disappeared
    FatalDeviceError
};

struct ReadResult {
    ReadStatus status;
    uint32_t  sectorsRead;     // may be < requested on short read
    // C2 error flags, one bit per data byte; non-empty when C2 was requested.
    // Layout: sectorsRead * 294 bytes; bit i of byte j flags data byte j*8+i.
    std::vector<uint8_t> c2;
};

class CdDevice {
public:
    static std::vector<DriveInfo> enumerate();
    static std::unique_ptr<CdDevice> open(const std::string& id);
    virtual ~CdDevice() = default;

    virtual const DriveInfo& info() const = 0;

    virtual std::optional<Toc> readToc() = 0;

    // Reads `count` 2352-byte CDDA sectors starting at signed LBA into `audio`.
    // `audio` must be at least count * 2352 bytes.
    // If `wantC2` true, returns C2 pointer bits in result.c2 (count * 294 bytes).
    // LBA may be negative (lead-in); the platform impl decides if it succeeds.
    virtual ReadResult readSectors(int32_t lba,
                                   uint32_t count,
                                   std::span<uint8_t> audio,
                                   bool wantC2) = 0;

    // SET CD SPEED. kBps == 0 means "max". Returns false if the drive ignores it.
    virtual bool setReadSpeed(uint16_t kBps) = 0;

    // Optional: subchannel ISRC for a track. Empty if unsupported / unavailable.
    virtual std::optional<std::string> readIsrc(uint8_t track) = 0;
};

} // namespace plyr::cd
```

Threading: `CdDevice` is *not* thread-safe. Calls are blocking; serialize at the caller. The ripper runs in a worker thread; UI thread observes progress via signals/callbacks.

Ownership: `std::unique_ptr<CdDevice>` from `open()`. RAII closes the FD and releases DA/SPTI handles.

Error reporting: by `ReadStatus`. Sense data and platform errno's are folded into the enum; if needed for diagnostics, expose a separate `lastDeviceError()` accessor that returns a platform-formatted string.

**Should the API unify around MMC READ CD shape?** Yes. The shape `readSectors(lba, count, audio, wantC2)` is exactly what MMC READ CD does. On macOS, the impl translates `wantC2` into `kCDSectorAreaUser | kCDSectorAreaErrorFlags`. On Linux/Windows, it builds the CDB byte 9 = 0x10 | (wantC2 ? 0x02 : 0x00). One mental model across platforms.

**Recommendation:** adopt the above. Platform-private code lives in `src/CdDevice_{macos,linux,windows}.cpp` behind the abstract `CdDevice`. The interface header is platform-agnostic.

**Caveats:** the `readSectors` LBA being `int32_t` (signed) is deliberate — negative values express lead-in reads. Platform impls validate.

---

## I. Permissively-licensed implementations worth studying

Reference *behavior*, do NOT copy code. Most of the CD-DA-ripping ecosystem is GPL; the permissive subset is narrower than you'd hope.

**Useful:**
- **Microsoft Windows-driver-samples SPTI tool** (`microsoft/Windows-driver-samples/storage/tools/spti`) — MIT license. The reference implementation of how to construct `SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER` on Windows. Read it freely; it's the official sample.
- **sg3_utils examples** (`hreinecke/sg3_utils/examples/sg_simple4.c`) — BSD/GPL dual but the examples directory uses BSD. Reference for the `sg_io_hdr_t` shape on Linux. The library itself is GPL — don't link it; read `sg_simple4.c` for the shape only.
- **Apple's `IOCDMediaBSDClient.h` and `IOCDTypes.h`** — APSL 2.0 (permissive-ish, OSI-approved). Headers are documentation. The plan already cites these.
- **redumper** (`superg/redumper`) — GPL-3.0. Do NOT copy code. Useful as a *technical document* on which drives expose which raw read commands, the D8 vs BE distinction, and the practical state of C2 reporting in current optical drives. Read READMEs and issues, not source.
- **AccurateRip's drive offset HTML page** — public data, no license issue. Bundleable as a transcribed JSON (Plan already says this).

**Conceptual reference only (do NOT copy):**
- **cdparanoia / paranoia FAQ pages** (xiph.org/paranoia) — explanatory text is fine to read for *what* the algorithm does; the source code is GPL. We don't plan to clone paranoia in v1 anyway.
- **Hydrogenaudio Knowledgebase** — community documentation, free to read; describes EAC's secure-ripping technique without exposing GPL code (EAC itself is closed-source freeware).
- **VLC's CD-DA module** — LGPL 2.1+. We could *link* against libvlc as an LGPL dep (LGPL is compatible with a permissive app) — but it's a heavy dependency for one use case, and PLYR-Qt's permissive stance is cleaner. Not recommended.

**Not useful for our purposes:**
- **libcdio** — GPL; can't link, can't copy.
- **whipper / morituri** — GPL; can't link, can't copy. Same for any cdparanoia derivative.
- **Aaru** — GPL-3.0; same.
- **cdda2wav (cdrtools)** — CDDL. Not GPL but also not MIT/BSD; cross-licensing with a permissive app is murky. Avoid.

**Recommendation:** Microsoft's SPTI sample + sg3_utils examples directory + Apple headers + AccurateRip DB data are sufficient. Everything else is reference-by-design-discussion only.

**Caveats:** "I read the GPL code to understand the technique" is not safe practice for a clean-room reimplementation. Use the design discussions, FAQ, and your own understanding of the public MMC spec. If you find yourself needing to look at libcdio/paranoia source for an answer, the right move is to find that answer in the MMC spec or in our own technical write-ups (this doc), not in the GPL source.

---

## J. What the plan got wrong / missing

### J.1 Concrete corrections

1. **C2 IS available via BSD ioctl.** The plan's "C2 error pointers are requestable via the sector area flags on some drives, but interpreting them is drive-specific. Defer." should change to: "Request C2 alongside user data (sectorArea = `kCDSectorAreaUser | kCDSectorAreaErrorFlags` = 0x12); on any non-zero C2 byte, trigger re-read." This is a v1 lever, not deferred.

2. **IORegistry property keys are `Vendor Name` / `Product Name`.** Not "Vendor Identification" / "Product Identification." Fix the doc.

3. **Speed control is `DKIOCCDSETSPEED`**, available without entitlements. The plan should include `setReadSpeed()` on `CdDevice` and use it in the retry path.

4. **Subchannel data is available** via `DKIOCCDREADISRC` and `DKIOCCDREADMCN`, not just `DKIOCCDREAD` with a subchannel area flag. Either path works. Out of scope for v1, but noting it for completeness.

5. **The plan's `dk_cd_read_t` description is correct** — but it should explicitly mention that `sectorArea` is a bitmask, not a single value. The default in the plan ("request `kCDSectorAreaUser`") is fine but it's worth emphasizing.

6. **`/dev/disk` vs `/dev/rdisk`** — the plan says "open the *raw* char device" for buffering reasons. This is correct but the actual reason is that the ioctls themselves want a CD-aware FD; the buffer-cache concern is secondary. On macOS, the BSD client's ioctls work on both; raw is just cleaner.

7. **The plan's "DAClaim or O_EXLOCK" — recommend doing both** (see A.4). It's belt-and-suspenders but cheap.

### J.2 2026 reality checks

- The optical-drive market has fully converged on USB-attached slim drives. Internal SATA optical drives effectively don't exist in new hardware. USB-attached drives mean: bridge chipsets sometimes mask the real drive name from INQUIRY; the AR DB may not have a match for what INQUIRY returns. Plan for the "no DB match" case gracefully.
- Apple Silicon Macs don't ship optical drives. All testing will be on USB SuperDrive or external USB drives. macOS 14+ on Apple Silicon does NOT introduce any new restrictions on the BSD CD ioctl path that we're aware of; the plan's "BSD ioctls are entitlement-free" remains accurate as of late 2025 / 2026.
- Windows 11 24H2 didn't materially change the SPTI surface. The MSIX packaged-app distribution model works for SPTI-using apps without special manifest entries, in practice.
- Linux distros increasingly require explicit user-in-cdrom-group membership or a udev rule for `/dev/sr0` access. Ship an udev rule.

### J.3 Drive-speed as a v1 lever — yes, include it

The plan currently doesn't include any speed-control mechanism. It should. The retry path benefits enormously from slowing the drive to 4x. Add `setReadSpeed()` to `CdDevice` and exercise it in the re-read ladder.

### J.4 Other notes worth flagging

- **Async read / multi-sector**: read 26 sectors per ioctl call by default (61KB) to amortize ioctl overhead. Don't read 1 sector at a time. Don't read 1000 sectors at a time either (drive max-transfer limits).
- **Cancel path**: long rips need cancellation. The ioctl is blocking; a cancel signal must close the FD from another thread to interrupt the in-flight ioctl. Design `CdDevice` for that.
- **Disc removed mid-rip**: detect via ENXIO/EIO and surface a specific status. Don't crash.
- **Drive reports wrong leadout**: rare but seen on cheap drives. Compare leadout from TOC vs. attempting a read at `leadout - 1`; if the read fails consistently while shorter reads work, the TOC is lying. Out of scope to handle automatically; useful diagnostic.
- **Pre-emphasis flag** (TOC control bit 0): not in the plan. The standard says pre-emphasis CDs should have the encoder write a `EMPHASIS=ON` Vorbis tag or pre-emphasis-aware FLAC field. Most modern discs don't use pre-emphasis. v1 can ignore but log if seen.

---

## Concrete proposed edits to CD_READER_PLAN.md

The following bullet-level changes (insertions, replacements; no full rewrite):

1. **In the "macOS approach: which APIs" section, under "Sector + TOC reads":**
   - Add `DKIOCCDSETSPEED` / `DKIOCCDGETSPEED` to the listed ioctls; one-line description: "set or get drive transfer speed in kB/s (`kCDSpeedMin`..`kCDSpeedMax`)."
   - Add `DKIOCCDREADISRC` and `DKIOCCDREADMCN` to the listed ioctls; one-line: "track ISRC / disc MCN — out of v1 scope, listed for future."
   - Replace the C2 wording: "Request C2 error flags by ORing `kCDSectorAreaErrorFlags` into `sectorArea`. Sector size becomes 2352 + 294 = 2646 bytes (CDDA + 294-byte C2 mask). v1 re-reads any sector with non-zero C2 bytes."

2. **In the "Drive vendor/product — IORegistry" section:**
   - Replace `Vendor Identification` with `Vendor Name` (literal IORegistry key).
   - Replace `Product Identification` with `Product Name`.
   - Note the dict path: `Device Characteristics` -> `Vendor Name` / `Product Name`.
   - Document the IORegistry walk: from `IOCDMedia`, iterate parents with `kIORegistryIterateParents | kIORegistryIterateRecursively`, find the first entry that has a `Device Characteristics` dict, read out vendor/product.

3. **In "Proposed module layout / CdDevice section":**
   - Add `setReadSpeed(kBps)` to the public `CdDevice` API (it's referenced by step 8's re-read logic).
   - Add a `wantC2` (or similar) bool to `readSectors` so the caller can opt in/out per call.
   - Specify the return type carries a `c2` byte array when C2 was requested.
   - Make `lba` parameter signed `int32_t` to allow negative LBA (lead-in) reads.

4. **In "Known gotchas":**
   - Replace the "C2 error pointers are requestable... Defer." bullet with: "C2 error pointers are requestable via `kCDSectorAreaErrorFlags`. v1 uses them as a re-read trigger: any non-zero byte in the 294-byte C2 mask for any sector triggers a re-read of that sector. Drive C2 accuracy is patchy; treat non-zero as 'definitely error' but treat zero as 'probably OK,' with AR/CTDB cross-verification as the actual safety net."
   - Add a new bullet: "Drive speed control via `DKIOCCDSETSPEED`. The retry-on-error path drops to 4x (706 kB/s) after the first ioctl-level retry."
   - Add a new bullet: "Lead-in reads (negative LBA) are required when applying negative drive offsets to track 1. Drives differ in how much lead-in they expose: some 0 sectors, some ~135, some all 150. Pad with zeros on failure; the AR skip region (5 sectors at start and end) absorbs this."

5. **In "MVP path", step 8:**
   - Expand "Basic re-read on error" to: "Detect ioctl failure / short reads / C2-flagged sectors; retry up to 3x at current speed; then drop to 4x via setReadSpeed and retry up to 5x; if still failing, zero-fill the sector and log it in the rip log."

6. **In "Non-negotiables":** no changes — they're correct as-is.

7. **In "Deliberately deferred":**
   - Remove "C2 error pointer interpretation. Drive-specific behavior; not worth the complexity in v1." (Now v1.)
   - Keep "Cdparanoia-grade jitter correction" deferred.
   - Add "Plextor D8 command and lead-in/RAW-TOC reads — not needed; tools that do this are archival-imaging-grade, not consumer ripper-grade."

8. **In "References" section:**
   - Add: "Apple `IOStorageDeviceCharacteristics.h` — the `kIOPropertyVendorNameKey` / `kIOPropertyProductNameKey` definitions."
   - Add: "Linux `<linux/cdrom.h>` UAPI header; `<scsi/sg.h>` for SG_IO."
   - Add: "Windows `ntddcdrm.h` / `ntddscsi.h` for SPTI definitions."
   - Add: "MMC-5 `READ CD` (0xBE) CDB layout — public T10 specs (Mt. Fuji / SFF-8090)."

9. **A new "Cross-platform shape" section** can be added before "macOS approach":
   - Note that the same MMC `READ CD` (0xBE) command underlies all three platforms.
   - macOS wraps it in `DKIOCCDREAD`; Linux sends it via SG_IO; Windows via SPTI.
   - The shared payload is `User Data + Error Flags` for v1 = 2646 bytes per sector.

---

## Open questions for the user

1. **Drive offset re-rip behavior.** When AR's post-rip offset scan reports a non-zero offset (meaning the DB lookup was wrong by that amount), should v1: (a) silently re-rip with corrected offset, (b) warn and require user confirmation before re-ripping, (c) just log the issue and keep the rip at the wrong offset? My recommendation in this doc is (a) auto-re-rip then keep the corrected version. Confirm.

2. **C2 retry budget.** I proposed 3 retries at full speed, then 5 retries at 4x, then zero-fill-and-log. Is that the right ladder for v1, or should we be more / less aggressive? More aggressive = slower rips on marginal discs; less aggressive = more rips fail. The Rachmaninoff set is presumably clean enough that this rarely fires.

3. **Mixed-mode discs.** Plan currently says "single-session audio-only CDs only." Concretely, when a mixed-mode CD is inserted: (a) refuse to rip and tell the user, or (b) rip the audio tracks and ignore the data track? (a) is simpler; (b) is more useful if any of your eventual users have CD Extras.

4. **Lead-in read failure.** For very negative drive offsets, the drive may refuse to read lead-in. Plan to zero-pad (treat lead-in as silence). Is that acceptable, or do you want the rip to fail loudly when lead-in is needed but unavailable? My recommendation: zero-pad + log; AR skip region absorbs it.

5. **`/dev/rdiskN` device name evolution.** macOS still uses `disk2` / `rdisk2` style names but newer device-naming work has been hinted at. v1 should still target `rdiskN`; if names change later, fix at that point. Confirmed default.

6. **Pre-emphasis handling.** Should v1 detect+tag pre-emphasis-flagged tracks (rare but real), or ignore the flag? FLAC has a standard way to record this. My recommendation: detect, write a `PRE_EMPHASIS=true` Vorbis comment, never apply de-emphasis (that's playback's job). Cheap to add.

7. **USB bridge chipsets** that mask the real drive INQUIRY. When that happens, the AR DB lookup will fail. Recommendation: rip at offset 0, post-hoc detect via AR offset scan, re-rip if needed (per Q1). Confirm this fallback is OK for v1.

---

## Summary of the cross-platform shape

| Concern | macOS | Linux | Windows |
| --- | --- | --- | --- |
| Discovery | DiskArbitration (`IOCDMedia`) | libudev or `/sys/class/block` | SetupAPI + `GUID_DEVINTERFACE_CDROM` |
| Read CD-DA | `DKIOCCDREAD` (BSD ioctl) | `SG_IO` + `READ CD` (0xBE) | SPTI + `READ CD` (0xBE) |
| Sector payload | User + ErrorFlags = 2646 | Same | Same |
| Read TOC | `DKIOCCDREADTOC` | `CDROMREADTOCENTRY` or SG_IO + 0x43 | SPTI + 0x43 |
| Speed control | `DKIOCCDSETSPEED` | `CDROM_SELECT_SPEED` or SG_IO + 0xBB | SPTI + 0xBB |
| Vendor/product | IORegistry `Device Characteristics` dict | INQUIRY via SG_IO, or `/sys/block/.../device/{vendor,model}` | INQUIRY via SPTI, or SetupAPI `SPDRP_FRIENDLYNAME` |
| Exclusive lock | `DADiskClaim` + `O_EXLOCK` on `/dev/rdiskN` | `O_RDWR` on `/dev/sr0` (group cdrom) | `FILE_SHARE_NONE` flag on `CreateFile` |

Single mental model: open device, issue MMC commands (READ TOC, INQUIRY, READ CD), close. The platform glue is small; the core ripping logic is identical across all three.

---

## Sources

- Apple `IOCDMediaBSDClient.h`, `IOCDTypes.h`, `IOCDMedia.h` (Xcode SDK on the user's machine; lines cited in section A.1).
- Apple `IOStorageDeviceCharacteristics.h` (key constants at lines 92, 119, 146 of the local SDK).
- Apple `SCSITaskLib.h` (local SDK) and Apple Developer Forums thread #650611 on IOSCSIArchitectureModelFamily and DriverKit.
- Apple Developer Documentation: [DriverKit Entitlements](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development); [Disk Arbitration Programming Guide](https://developer.apple.com/library/archive/documentation/DriversKernelHardware/Conceptual/DiskArbitrationProgGuide/Introduction/Introduction.html); [Notarizing macOS Software](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
- Linux kernel UAPI: [`include/uapi/linux/cdrom.h`](https://github.com/torvalds/linux/blob/master/include/uapi/linux/cdrom.h); [Linux kernel CDROM ioctl docs](https://docs.kernel.org/userspace-api/ioctl/cdrom.html).
- Linux SG: [SCSI Generic (sg) HOWTO](https://sg.danny.cz/sg/p/sg_v3_ho.html); [`scsi/sg.h`](https://github.com/torvalds/linux/blob/master/include/scsi/sg.h).
- Microsoft Learn: [IOCTL_SCSI_PASS_THROUGH_DIRECT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddscsi/ni-ntddscsi-ioctl_scsi_pass_through_direct); [IOCTL_CDROM_RAW_READ](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddcdrm/ni-ntddcdrm-ioctl_cdrom_raw_read); [IOCTL_SCSI_GET_INQUIRY_DATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddscsi/ni-ntddscsi-ioctl_scsi_get_inquiry_data); [GUID_DEVINTERFACE_CDROM](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-cdrom); [CD-ROM Set Speed](https://learn.microsoft.com/en-us/previous-versions/windows/drivers/storage/cd-rom-set-speed).
- Microsoft SPTI sample (MIT licensed): [`microsoft/Windows-driver-samples/storage/tools/spti`](https://github.com/microsoft/Windows-driver-samples/blob/main/storage/tools/spti/README.md).
- MMC specs / community references: [T10 MMC-2 CD Media Commands (97-117r0)](https://www.t10.org/ftp/t10/document.97/97-117r0.pdf); [Reading CD subcode data](https://the6p4c.github.io/2020/01/29/cd-subcode.html); [redumper](https://github.com/superg/redumper) (GPL — reference for technical claims only).
- Hydrogenaudio Knowledgebase: [AccurateRip](https://wiki.hydrogenaudio.org/index.php?title=AccurateRip); [EAC Drive Options](https://wiki.hydrogenaudio.org/index.php?title=EAC_Drive_Options); [Secure ripping](https://wiki.hydrogenaudio.org/index.php?title=Secure_ripping); [Cdparanoia article](https://wiki.hydrogenaudio.org/index.php?title=Cdparanoia).
- cdparanoia: [paranoia FAQ](https://www.xiph.org/paranoia/faq.html).
- C2 reporting accuracy: [CdrInfo: Testing C2 information](https://www.cdrinfo.com/d7/content/testing-c2-information).
- AccurateRip drive offset DB: [AR drive offsets](https://www.accuraterip.com/driveoffsets.htm).
