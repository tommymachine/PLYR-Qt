# Cross-platform direction for the CD reader

Working notes for porting `CdDevice` beyond macOS and validating each
backend without buying a second machine. The interface in
[`src/CdDevice.h`](src/CdDevice.h) is already cross-platform; only the
per-platform `CdDevice_<os>.cpp` implementation files need adding.

The shared MMC command underneath every platform is `READ CD` (0xBE);
the OS-specific layer is just how that command gets to the drive. See
`CD_READER_PLAN.md` § "Cross-platform shape" for the canonical CDB
layout and `CD_READER_RESEARCH_FINAL.md` § 2B for byte-level details.

---

## Current state

| Platform | Status | Wrapper |
|---|---|---|
| macOS    | ✅ shipping | IOKit BSD-client ioctls (`DKIOCCDREAD` & co.) |
| Linux    | not started | SG_IO + raw CDB on `/dev/sr0` |
| Windows  | not started | SPTI (`IOCTL_SCSI_PASS_THROUGH_DIRECT`) on `\\.\CdRom0` |

The macOS impl is small enough (~370 lines, [`src/CdDevice_macOS.cpp`](src/CdDevice_macOS.cpp))
that the Linux and Windows ports should each be in the same
ballpark. The bulk is per-platform plumbing — enumerate / open /
close / cancel — plus a CDB builder for `READ CD` and `READ TOC`.

---

## Linux port plan

- Enumerate: walk `/sys/class/block/sr*` or use `libudev`. Vendor /
  product live at `/sys/block/sr0/device/{vendor,model}`. No
  permissions wall.
- Open: `open("/dev/sr0", O_RDWR | O_NONBLOCK)`. User needs membership
  in the `cdrom` or `disk` group on most distros.
- Read CDDA: `SG_IO` ioctl with `sg_io_hdr_t` carrying a 12-byte
  CDB exactly the shape macOS already builds. The payload comes back
  in the buffer the caller provided. Same 2352-byte / 2646-byte
  sector sizing as macOS.
- Read TOC: SG_IO + `READ TOC` (0x43). Same MMC command we issue via
  `DKIOCCDREADTOC`; parses identically.
- Speed: `CDROM_SELECT_SPEED` ioctl or SG_IO + `SET CD SPEED` (0xBB).
- Cancel in-flight: `close()` the FD from another thread.

Sources to study (BSD-licensed or permissive enough to be quoted):
- Linux UAPI headers `<linux/cdrom.h>` and `<scsi/sg.h>` — the
  authoritative interface definitions.
- MMC-6 spec for the CDB layout. Already cited in
  `CD_READER_PLAN.md`.

GPL sources (libcdio, redumper) can be **read for technique** but not
copied; the project's clean-room stance applies.

---

## Windows port plan

- Enumerate: `SetupDiGetClassDevs(GUID_DEVINTERFACE_CDROM)` →
  `SetupDiEnumDeviceInterfaces` → device-path strings like
  `\\.\CdRom0`.
- Open: `CreateFileW` with `GENERIC_READ` and the share flags that
  match SCSI exclusivity expectations.
- Read CDDA / TOC: `DeviceIoControl` with
  `IOCTL_SCSI_PASS_THROUGH_DIRECT` (SPTI), the CDB embedded in a
  `SCSI_PASS_THROUGH_DIRECT` struct, the data buffer separately.
- Speed: SPTI + `SET CD SPEED` (0xBB), same CDB shape as Linux.
- Cancel: `CancelIoEx` on the handle.

A signed binary doesn't need any special driver; SPTI is in the base
OS. Some operations may require running elevated (administrator)
depending on Windows version; verify during testing.

Reference shapes (permissively licensed, safe to read):
- Microsoft `windows-driver-samples` SPTI sample — MIT.
- Microsoft Storage Driver Reference docs.

---

## Test infrastructure on this Intel Mac

The big win of being on Intel: **Boot Camp is available**. Native dual-
boot gives the most representative test environment because the
optical drive is directly attached, not USB-passed-through.

| Path | Linux | Windows | USB passthrough quality | Notes |
|---|---|---|---|---|
| **Boot Camp dual-boot** | works (any distro) | works (Win 10; Win 11 needs TPM workaround) | n/a — drive is physically attached | Most realistic. Disk repartition required. |
| **VMware Fusion** (free 2024+) | works | works | excellent — drive shows up the moment you "Connect to VM" | Probably the smoothest VM path. |
| **VirtualBox** + Extension Pack | works | works | good for USB 2.0/3.0; needs Extension Pack (free for personal) | Long-standing community support. |
| **UTM** (QEMU) | works | rough | works but fiddly | Best free option for Linux; worth the patience. |
| **Parallels Desktop** | works | works (excellent) | best Windows experience overall | Paid (~$130/yr). |
| **Docker / OrbStack** | — | — | no real device access | Not viable for the reader. |

### Recommended starter setup

1. **Linux**: UTM or VMware Fusion → Debian / Ubuntu LTS. Install
   build tools (`apt install build-essential cmake qt6-base-dev
   libflac-dev zlib1g-dev`) and add yourself to the `cdrom` group.
   USB-pass the SuperDrive when the VM is running and the disc is
   inserted.
2. **Windows**: VMware Fusion → Windows 10 (avoid the Win11 TPM
   hurdle for now). Install Visual Studio Build Tools and Qt 6 MSVC.
   USB-pass the SuperDrive the same way.
3. **Optional**: a Boot Camp partition for the rare case where VM
   USB passthrough misbehaves on a specific drive firmware. Saves a
   "is this our code or the VM?" diagnostic detour.

USB passthrough caveats:
- The SuperDrive's bus enumeration may take a moment after VM start
  before the guest sees it. Eject + re-insert often forces the
  appearance event.
- VMs share USB bus state with the host — Music.app on the host can
  still grab the disc *if Concerto isn't running*, so close any host
  CD handlers first.

### Validation strategy per port

Same approach as we used on macOS:

1. **Drive enumeration** sanity check: confirm `CdDevice::enumerate()`
   returns the SuperDrive (or the platform's optical-drive entry).
2. **TOC compare**: the four disc IDs from `cdrip_cli --toc` must
   match the macOS-side IDs *exactly* (same disc, same TOC math).
3. **Byte-level compare**: read N sectors on the new platform, read
   the same N on macOS, diff. They must be byte-identical (the drive
   does CIRC + Reed-Solomon before the host sees anything; software
   has no degrees of freedom).
4. **Full rip + AR verify**: `cdrip_cli --rip` an AR-known disc
   (Ravel CD2 is our reference). `arverify_cli` must report AR
   offset 0 with all middle tracks ACCURATE.

If steps 1-3 pass and step 4 doesn't, the bug is in slicing / offset
application, not the platform code. If step 3 fails, the platform
code is sending the wrong CDB or interpreting the response wrong.

---

## CI thoughts (later)

GitHub Actions has Linux + Windows + macOS runners but no optical
drives attached. A useful subset still runs:

- Cross-compile each platform to verify the build is healthy.
- Run `cdrip_cli --db-info` on each platform — exercises the qrc
  resource embedding and JSON parse without needing a drive.
- Run the existing `arverify` / `flacrt` / `mbquery` CLIs against
  bundled test FLACs — those don't need a drive either.

Adding a self-hosted runner with a USB optical drive is overkill for
v1 but would catch drive-specific regressions later.

---

## Order of operations when porting

For each new platform:

1. Add `src/CdDevice_<os>.cpp` implementing the four virtual methods.
2. Update `CMakeLists.txt` to include it on that platform's
   `if(...)` branch and link the platform's optical-drive libs
   (`-lcrypto` is *not* needed; nothing fancy required).
3. Iterate the MVP steps from `CD_READER_PLAN.md` in order:
   enumerate → TOC → read sectors → encode → offset → MB tag → retry.
   Each step has the same "is it working" check it had on macOS.
4. Once `arverify_cli` shows the same offset behavior on the ported
   side as on macOS for the same disc, the port is done.
