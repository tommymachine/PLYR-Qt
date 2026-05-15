// Native CD-DA reader interface. The cross-platform shape — macOS, Linux, and
// Windows implementations all back the same MMC `READ CD` (0xBE) command
// through the platform's preferred wrapper (DKIOCCDREAD on macOS, SG_IO on
// Linux, SPTI on Windows). Per-platform implementations live in
// `CdDevice_<platform>.cpp`.
//
// Not thread-safe except `cancel()`, which is intentionally callable from
// another thread to unblock an in-flight read. Drive-offset lookup is the
// caller's job — `Ripper` queries the offset DB after `open()` and applies
// the offset at read time.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace plyr::cd {

// One enumerated optical drive. `id` is the platform handle to pass back into
// `CdDevice::open` — on macOS the BSD name without the `/dev/` prefix (e.g.
// "disk5"), on Linux "/dev/sr0", on Windows the Win32 device path.
struct DriveInfo {
    std::string id;
    std::string vendor;    // canonicalized for AR drive-offset DB matching
    std::string product;
    std::string revision;
    bool        hasMedia = false;
};

// One row of a parsed CD table of contents. `startLba` is the absolute frame
// address (track 1 typically sits at LBA 0 by MMC convention; pregap is
// already accounted for by the drive).
struct TocEntry {
    uint8_t  trackNumber = 0;
    uint32_t startLba    = 0;
    bool     isData      = false;
    bool     preEmphasis = false;
};

struct Toc {
    std::vector<TocEntry> tracks;
    uint32_t              leadOutLba = 0;
};

enum class ReadStatus {
    Ok,
    TransientBusy,
    MediumError,
    OutOfRange,
    Aborted,
    FatalDeviceError,
};

struct ReadResult {
    ReadStatus           status      = ReadStatus::FatalDeviceError;
    uint32_t             sectorsRead = 0;  // may be < requested
    std::vector<uint8_t> c2;               // sectorsRead × 294 bytes when wantC2
};

class CdDevice {
public:
    // List optical drives currently visible to the OS. v1 ships filtering to
    // drives that have CD media inserted (the `hasMedia=false` field is
    // wired for future "no disc" enumeration). The returned
    // `DriveInfo::id` is what `open()` accepts.
    static std::vector<DriveInfo>    enumerate();

    // Acquire exclusive use of a drive. Returns nullptr if the drive is
    // gone, already claimed by another process, or platform-incompatible.
    // Not yet implemented — comes online with MVP step 2.
    static std::unique_ptr<CdDevice> open(const std::string& id);

    virtual ~CdDevice() = default;

    virtual const DriveInfo&           info() const                          = 0;
    virtual std::optional<Toc>         readToc()                             = 0;
    virtual ReadResult                 readSectors(int32_t lba, uint32_t count,
                                                   std::span<uint8_t> audio,
                                                   bool wantC2)               = 0;
    virtual bool                       setReadSpeed(uint16_t kBps)           = 0;
    virtual std::string                lastDeviceError() const               = 0;
    virtual void                       cancel()                              = 0;
    virtual std::optional<std::string> readIsrc(uint8_t track)               = 0;
};

} // namespace plyr::cd
