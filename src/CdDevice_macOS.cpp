// macOS implementation of plyr::cd::CdDevice.
//
// MVP steps 1-2:
//   * enumerate() — IOKit registry walk for IOCDMedia entries (one per
//     inserted CD), reads the BSD name and walks parents to pick up the
//     IOKit `Device Characteristics` vendor/product/revision strings.
//   * open()      — locates the IOMedia for a BSD name, gathers DriveInfo,
//     opens /dev/r<bsd> with O_RDONLY | O_NONBLOCK | O_EXLOCK.
//   * readToc()   — DKIOCCDREADTOC with kCDTOCFormatTOC (raw TOC); parses
//     Apple's CDTOC struct, filters to ADR-1 ("current position") track
//     descriptors and the POINT=0xA2 lead-out marker.
//
// Sector reads, speed control, ISRC, and cancel are stubbed pending the
// later MVP steps.
//
// We don't take a DiskArbitration claim here — the shield/rip session
// owns that at a higher layer (see CdShield). BSD raw-device access is
// independent of DA claim state on macOS; O_EXLOCK gives us mutual
// exclusion against other processes that go through the same /dev path.

#include "CdDevice.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOCDMedia.h>
#include <IOKit/storage/IOCDMediaBSDClient.h>
#include <IOKit/storage/IOCDTypes.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <libkern/OSByteOrder.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plyr::cd {

namespace {

// Drop-in RAII for io_object_t / io_iterator_t. IOKit hands these out
// with a +1 retain count; the caller must IOObjectRelease.
class IOOwner {
public:
    IOOwner() = default;
    explicit IOOwner(io_object_t o) noexcept : obj_(o) {}
    ~IOOwner() { reset(); }
    IOOwner(const IOOwner&) = delete;
    IOOwner& operator=(const IOOwner&) = delete;
    IOOwner(IOOwner&& o) noexcept : obj_(o.obj_) { o.obj_ = IO_OBJECT_NULL; }
    IOOwner& operator=(IOOwner&& o) noexcept {
        if (this != &o) { reset(); obj_ = o.obj_; o.obj_ = IO_OBJECT_NULL; }
        return *this;
    }
    io_object_t get() const noexcept { return obj_; }
    explicit operator bool() const noexcept { return obj_ != IO_OBJECT_NULL; }
    void reset() noexcept { if (obj_) { IOObjectRelease(obj_); obj_ = IO_OBJECT_NULL; } }
private:
    io_object_t obj_ = IO_OBJECT_NULL;
};

struct CFOwnedDeleter {
    void operator()(CFTypeRef p) const noexcept { if (p) CFRelease(p); }
};
template <typename T>
using CFOwned = std::unique_ptr<std::remove_pointer_t<T>, CFOwnedDeleter>;

std::string cfStringToUtf8(CFStringRef s) {
    if (!s) return {};
    CFIndex len = CFStringGetLength(s);
    CFIndex cap = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(cap), '\0');
    if (!CFStringGetCString(s, out.data(), cap, kCFStringEncodingUTF8))
        return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

// Normalize for AR drive-offset DB key matching: trim outer whitespace,
// collapse internal runs to one space, uppercase. Internal spaces are
// preserved because they're meaningful in real drive names
// (e.g. "BD-RW BDR-XD05").
std::string canonicalize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool prevSpace = true;  // trim leading whitespace
    for (unsigned char c : in) {
        if (std::isspace(c)) {
            if (!prevSpace) out.push_back(' ');
            prevSpace = true;
        } else {
            out.push_back(static_cast<char>(std::toupper(c)));
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

io_object_t findDeviceCharacteristicsAncestor(io_object_t start) {
    io_iterator_t raw = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(start, kIOServicePlane,
                                      kIORegistryIterateParents | kIORegistryIterateRecursively,
                                      &raw) != KERN_SUCCESS)
        return IO_OBJECT_NULL;
    IOOwner iter(raw);

    io_object_t entry;
    while ((entry = IOIteratorNext(iter.get()))) {
        CFTypeRef dict = IORegistryEntryCreateCFProperty(
            entry, CFSTR(kIOPropertyDeviceCharacteristicsKey),
            kCFAllocatorDefault, 0);
        if (dict) {
            CFRelease(dict);
            return entry;  // transfer +1 ownership to caller
        }
        IOObjectRelease(entry);
    }
    return IO_OBJECT_NULL;
}

void fillVendorProduct(io_object_t mediaEntry, DriveInfo& info) {
    IOOwner anc(findDeviceCharacteristicsAncestor(mediaEntry));
    if (!anc) return;

    CFTypeRef raw = IORegistryEntryCreateCFProperty(
        anc.get(), CFSTR(kIOPropertyDeviceCharacteristicsKey),
        kCFAllocatorDefault, 0);
    if (!raw || CFGetTypeID(raw) != CFDictionaryGetTypeID()) {
        if (raw) CFRelease(raw);
        return;
    }
    CFOwned<CFDictionaryRef> dict(static_cast<CFDictionaryRef>(raw));

    auto pull = [&](CFStringRef key) -> std::string {
        CFTypeRef v = CFDictionaryGetValue(dict.get(), key);
        if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
        return cfStringToUtf8(static_cast<CFStringRef>(v));
    };
    info.vendor   = canonicalize(pull(CFSTR(kIOPropertyVendorNameKey)));
    info.product  = canonicalize(pull(CFSTR(kIOPropertyProductNameKey)));
    info.revision = canonicalize(pull(CFSTR(kIOPropertyProductRevisionLevelKey)));
}

bool readBsdName(io_object_t mediaEntry, std::string& out) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(
        mediaEntry, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
    if (!v) return false;
    CFOwned<CFTypeRef> guard(v);
    if (CFGetTypeID(v) != CFStringGetTypeID()) return false;
    out = cfStringToUtf8(static_cast<CFStringRef>(v));
    return !out.empty();
}

bool isWholeMedia(io_object_t mediaEntry) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(
        mediaEntry, CFSTR(kIOMediaWholeKey), kCFAllocatorDefault, 0);
    if (!v) return false;
    CFOwned<CFTypeRef> guard(v);
    return CFGetTypeID(v) == CFBooleanGetTypeID()
        && CFBooleanGetValue(static_cast<CFBooleanRef>(v));
}

// Walk the IOCDMedia registry for the entry whose BSD Name == `bsdName`.
// Returns +1; caller must IOObjectRelease.
io_object_t findMediaByBsdName(const std::string& bsdName) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOCDMediaClass);
    if (!m) return IO_OBJECT_NULL;
    io_iterator_t raw = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &raw) != KERN_SUCCESS)
        return IO_OBJECT_NULL;
    IOOwner iter(raw);

    io_object_t e;
    while ((e = IOIteratorNext(iter.get()))) {
        std::string name;
        if (readBsdName(e, name) && name == bsdName) {
            return e;   // transfer +1 ownership to caller
        }
        IOObjectRelease(e);
    }
    return IO_OBJECT_NULL;
}

class CdDeviceMac : public CdDevice {
public:
    CdDeviceMac(DriveInfo info, int fd)
        : info_(std::move(info)), fd_(fd) {}
    ~CdDeviceMac() override { if (fd_ >= 0) ::close(fd_); }

    const DriveInfo&   info() const override { return info_; }
    std::optional<Toc> readToc() override;

    ReadResult readSectors(int32_t lba, uint32_t count,
                           std::span<uint8_t> audio, bool wantC2) override;

    bool setReadSpeed(uint16_t /*kBps*/) override { return false; }     // step 5+
    std::string lastDeviceError() const override { return lastError_; }
    void cancel() override {}                                            // step 5+
    std::optional<std::string> readIsrc(uint8_t /*track*/) override {
        return std::nullopt;                                             // v2
    }

private:
    DriveInfo   info_;
    int         fd_ = -1;
    std::string lastError_;
};

ReadResult CdDeviceMac::readSectors(int32_t lba, uint32_t count,
                                    std::span<uint8_t> audio, bool wantC2) {
    ReadResult r;
    if (lba < 0) {
        // Lead-in / negative-LBA reads need drive overread; deferred to
        // the step that wires up drive-offset application.
        r.status = ReadStatus::OutOfRange;
        lastError_ = "negative LBA reads not supported yet";
        return r;
    }
    if (wantC2) {
        // v1.0 ships without C2 (matches libcdio's macOS default). v1.1
        // will flip per-sector size to 2646 and demux audio+flags.
        r.status = ReadStatus::FatalDeviceError;
        lastError_ = "C2 read path is v1.1";
        return r;
    }
    if (count == 0) {
        r.status      = ReadStatus::Ok;
        r.sectorsRead = 0;
        return r;
    }

    constexpr uint32_t perSectorBytes = 2352;   // kCDSectorSizeCDDA
    const uint64_t     wantBytes      = uint64_t{count} * perSectorBytes;
    if (audio.size() < wantBytes
        || wantBytes > std::numeric_limits<uint32_t>::max()) {
        r.status   = ReadStatus::FatalDeviceError;
        lastError_ = "readSectors: caller buffer too small or count overflow";
        return r;
    }

    dk_cd_read_t arg{};
    arg.offset       = uint64_t{static_cast<uint32_t>(lba)} * perSectorBytes;
    arg.sectorArea   = kCDSectorAreaUser;       // 0x10 — audio payload only
    arg.sectorType   = kCDSectorTypeCDDA;       // 0x01
    arg.bufferLength = static_cast<uint32_t>(wantBytes);
    arg.buffer       = audio.data();

    if (::ioctl(fd_, DKIOCCDREAD, &arg) < 0) {
        const int err = errno;
        lastError_   = std::string("DKIOCCDREAD: ") + std::strerror(err);
        r.sectorsRead = arg.bufferLength / perSectorBytes;  // bytes done before failure
        switch (err) {
            case EBUSY:  r.status = ReadStatus::TransientBusy;    break;
            case EIO:    r.status = ReadStatus::MediumError;       break;
            case ENXIO:
            case EINVAL: r.status = ReadStatus::OutOfRange;        break;
            case EINTR:  r.status = ReadStatus::Aborted;           break;
            default:     r.status = ReadStatus::FatalDeviceError;
        }
        return r;
    }

    r.status      = ReadStatus::Ok;
    r.sectorsRead = arg.bufferLength / perSectorBytes;
    return r;
}

std::optional<Toc> CdDeviceMac::readToc() {
    // ~213 bytes for a 16-track single-session CD (19 descriptors × 11 bytes
    // + 4-byte header). 1 KiB easily covers a 99-track multi-session
    // worst case.
    std::array<uint8_t, 1024> raw{};

    dk_cd_read_toc_t arg{};
    arg.format       = kCDTOCFormatTOC;     // 0x02 — raw TOC
    arg.formatAsTime = 0;                   // no effect on format 0x02; set 0 for sanity
    arg.address.session = 0;                // not used for format 0x02
    arg.bufferLength = static_cast<uint16_t>(raw.size());
    arg.buffer       = raw.data();

    if (::ioctl(fd_, DKIOCCDREADTOC, &arg) < 0) {
        lastError_ = std::string("DKIOCCDREADTOC: ") + std::strerror(errno);
        return std::nullopt;
    }
    if (arg.bufferLength < sizeof(CDTOC)) {
        lastError_ = "DKIOCCDREADTOC returned a truncated TOC header";
        return std::nullopt;
    }

    const auto*    toc        = reinterpret_cast<const CDTOC*>(raw.data());
    const uint32_t tocSize    = OSSwapBigToHostInt16(toc->length)
                                + static_cast<uint32_t>(sizeof(toc->length));
    if (tocSize <= sizeof(CDTOC) || tocSize > arg.bufferLength) {
        lastError_ = "DKIOCCDREADTOC: descriptor length out of range";
        return std::nullopt;
    }
    const uint32_t count =
        (tocSize - sizeof(CDTOC)) / sizeof(CDTOCDescriptor);

    Toc out;
    out.tracks.reserve(count);
    bool leadoutSeen = false;

    for (uint32_t i = 0; i < count; ++i) {
        const CDTOCDescriptor& d = toc->descriptors[i];
        // Only ADR=1 ("current position", Q-mode-1) descriptors carry the
        // track/lead-out addresses we care about. ADR 2/3/5 carry MCN,
        // ISRC, CD-TEXT keys we'll deal with in v2.
        if (d.adr != 1) continue;

        if (d.point >= 1 && d.point <= 99) {
            TocEntry te;
            te.trackNumber = d.point;
            te.startLba    = CDConvertMSFToLBA(d.p);
            // CDDA control nibble (MMC-6 §6.34, Table 547):
            //   bit 0  pre-emphasis (audio only)
            //   bit 1  digital copy permitted (irrelevant to ripping)
            //   bit 2  data track (vs audio)
            //   bit 3  4-channel audio (essentially never used)
            te.isData      = (d.control & 0x04) != 0;
            te.preEmphasis = (d.control & 0x01) != 0;
            out.tracks.push_back(te);
        } else if (d.point == 0xA2 && !leadoutSeen) {
            // First session's lead-out wins — v1 ignores subsequent sessions.
            out.leadOutLba = CDConvertMSFToLBA(d.p);
            leadoutSeen = true;
        }
        // POINT 0xA0/0xA1 (first/last track meta) are redundant with
        // walking the 1..99 descriptors directly. POINT 0xB0..0xC1 are
        // multi-session boundary markers; ignored in v1.
    }

    if (out.tracks.empty() || !leadoutSeen) {
        lastError_ = "DKIOCCDREADTOC: missing tracks or lead-out";
        return std::nullopt;
    }
    return out;
}

} // namespace

std::vector<DriveInfo> CdDevice::enumerate() {
    std::vector<DriveInfo> out;

    // IOServiceMatching's dict is consumed by IOServiceGetMatchingServices.
    CFMutableDictionaryRef matching = IOServiceMatching(kIOCDMediaClass);
    if (!matching) return out;

    io_iterator_t rawIter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &rawIter) != KERN_SUCCESS)
        return out;
    IOOwner iter(rawIter);

    io_object_t media;
    while ((media = IOIteratorNext(iter.get()))) {
        IOOwner owner(media);
        if (!isWholeMedia(media)) continue;       // skip partition slices

        DriveInfo info;
        if (!readBsdName(media, info.id)) continue;
        info.hasMedia = true;
        fillVendorProduct(media, info);
        out.push_back(std::move(info));
    }
    return out;
}

std::unique_ptr<CdDevice> CdDevice::open(const std::string& id) {
    IOOwner mediaEntry(findMediaByBsdName(id));
    if (!mediaEntry) return nullptr;

    DriveInfo info;
    if (!readBsdName(mediaEntry.get(), info.id)) return nullptr;
    info.hasMedia = true;
    fillVendorProduct(mediaEntry.get(), info);

    // Raw character device skips the kernel's buffer cache — important
    // since the cached /dev/diskN path doesn't preserve CDDA framing.
    //
    // No O_EXLOCK: the BSD lock layer can refuse character devices the
    // diskarbitration daemon has associated with media sessions (errno
    // EBUSY) even with no concurrent opener visible to lsof. Mutual
    // exclusion against autoplay handlers (Music.app, Finder) is the
    // CdShield's job via DADiskClaim; this is the same layout libcdio's
    // production macOS reader settled on (osx.c:333).
    const std::string rawPath = "/dev/r" + info.id;
    const int fd = ::open(rawPath.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return nullptr;

    return std::make_unique<CdDeviceMac>(std::move(info), fd);
}

} // namespace plyr::cd
