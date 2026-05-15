// macOS implementation of plyr::cd::CdShield. See CdShield.h.
//
// Notes on DA semantics:
//
// * DARegisterDiskAppearedCallback replays the *current* set of
//   matching disks at registration time as synthetic "appeared"
//   events, so a CD already in the drive when start() is called
//   gets claimed without a separate cold-scan pass.
//
// * DADiskClaim is asynchronous. Passing nullptr for both the release
//   callback and the claim-complete callback means: claim permanently
//   (no one can ask us to release) and don't notify us when the claim
//   resolves. Sufficient for shield duties — we don't have a recovery
//   policy if a claim fails (someone else already has it), and we
//   release explicitly in stop().
//
// * Callbacks fire on whichever run loop the session is scheduled on.
//   We use the main run loop in common modes so we keep firing even
//   when Qt is in a modal panel mode.
//
// * Scope: audio-bearing CDs only. Pure-data CDs fall through to
//   Finder so the user can still mount installer/photo discs while
//   Concerto is running. We detect "audio-bearing" by walking the
//   IOCDMedia's children for any partition with IOMediaContent ==
//   "CD_DA" — pure-data discs have no such children, pure-audio and
//   mixed-mode discs do.

#include "CdShield.h"

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOCDMedia.h>
#include <IOKit/storage/IOMedia.h>

#include <cstddef>
#include <vector>

namespace plyr::cd {

namespace {

bool diskHasAudioTrack(DADiskRef disk) {
    const char* bsd = DADiskGetBSDName(disk);
    if (!bsd) return false;

    CFMutableDictionaryRef match = IOBSDNameMatching(kIOMainPortDefault, 0, bsd);
    if (!match) return false;
    // IOServiceGetMatchingService consumes the dict.
    io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!media) return false;

    io_iterator_t iter = IO_OBJECT_NULL;
    const kern_return_t kr = IORegistryEntryCreateIterator(
        media, kIOServicePlane, kIORegistryIterateRecursively, &iter);
    if (kr != KERN_SUCCESS) {
        IOObjectRelease(media);
        return false;
    }

    bool found = false;
    for (io_object_t child; (child = IOIteratorNext(iter));) {
        CFTypeRef content = IORegistryEntryCreateCFProperty(
            child, CFSTR(kIOMediaContentKey), kCFAllocatorDefault, 0);
        if (content) {
            if (CFGetTypeID(content) == CFStringGetTypeID()
                && CFEqual(static_cast<CFStringRef>(content), CFSTR("CD_DA"))) {
                found = true;
            }
            CFRelease(content);
        }
        IOObjectRelease(child);
        if (found) break;
    }
    IOObjectRelease(iter);
    IOObjectRelease(media);
    return found;
}

void diskAppeared(DADiskRef disk, void* ctx) {
    if (!diskHasAudioTrack(disk)) return;  // data CDs: let Finder handle them

    CFMutableSetRef claimed = static_cast<CFMutableSetRef>(ctx);
    CFSetAddValue(claimed, disk);  // CFSet retains via kCFTypeSetCallBacks
    DADiskClaim(disk,
                kDADiskClaimOptionDefault,
                /*release cb*/ nullptr, /*release ctx*/ nullptr,
                /*claim cb*/   nullptr, /*claim ctx*/   nullptr);
}

void diskDisappeared(DADiskRef disk, void* ctx) {
    CFMutableSetRef claimed = static_cast<CFMutableSetRef>(ctx);
    CFSetRemoveValue(claimed, disk);  // CFSet releases the held ref
}

} // namespace

CdShield::~CdShield() {
    stop();
}

void CdShield::start() {
    if (session_) return;

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return;

    CFMutableSetRef claimed = CFSetCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeSetCallBacks);
    if (!claimed) {
        CFRelease(session);
        return;
    }

    // Match every CD media at the DA layer, then filter to audio-bearing
    // discs in diskAppeared() via an IOKit child-walk. DA's "media kind"
    // is the IOKit class name; "IOCDMedia" covers every CD variant.
    // DVDs (IODVDMedia) and Blu-ray (IOBDMedia) are out of scope —
    // Music doesn't auto-launch on those and we don't rip them.
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(match, kDADiskDescriptionMediaKindKey,
                         CFSTR(kIOCDMediaClass));

    DARegisterDiskAppearedCallback(session, match,
                                   &diskAppeared, claimed);
    DARegisterDiskDisappearedCallback(session, match,
                                      &diskDisappeared, claimed);
    CFRelease(match);

    DASessionScheduleWithRunLoop(session, CFRunLoopGetMain(),
                                 kCFRunLoopCommonModes);

    session_ = session;   // owns +1
    claimed_ = claimed;   // owns +1
}

void CdShield::stop() {
    if (!session_) return;

    DASessionRef    session = static_cast<DASessionRef>(session_);
    CFMutableSetRef claimed = static_cast<CFMutableSetRef>(claimed_);

    DASessionUnscheduleFromRunLoop(session, CFRunLoopGetMain(),
                                   kCFRunLoopCommonModes);

    // Drop both callbacks. DAUnregisterCallback takes the function
    // pointer cast to void*, plus the same context we registered with.
    DAUnregisterCallback(session, reinterpret_cast<void*>(&diskAppeared),
                         claimed);
    DAUnregisterCallback(session, reinterpret_cast<void*>(&diskDisappeared),
                         claimed);

    // Unclaim every disc we currently hold. Snapshot the set's
    // contents into a vector first so the iteration order is stable
    // and we don't have to worry about DA mutating things mid-loop.
    const CFIndex n = CFSetGetCount(claimed);
    if (n > 0) {
        std::vector<const void*> disks(static_cast<std::size_t>(n));
        CFSetGetValues(claimed, disks.data());
        for (const void* v : disks) {
            DADiskUnclaim(reinterpret_cast<DADiskRef>(const_cast<void*>(v)));
        }
    }

    CFRelease(claimed);
    CFRelease(session);

    session_ = nullptr;
    claimed_ = nullptr;
}

} // namespace plyr::cd
