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
//
// * What the shield does NOT do: prevent Music.app from launching.
//   DADiskClaim is asynchronous, and LaunchServices's CD-handler hook
//   fires on the appearance event without waiting for DA's session
//   state to settle. Empirically, an "always claim first, verify
//   audio second" variant did NOT win that race in practice — Music
//   still got started. The shield's value is exclusive disc access
//   (Finder / Spotlight / Music can't read our raw sectors out from
//   under a rip-in-progress), not Music suppression. Music
//   suppression is plyr::MusicBlocker's job: it observes
//   NSWorkspaceWillLaunchApplicationNotification and force-terminates
//   Music between fork and main-window. The visible "icon flash"
//   when a disc is inserted is Music.app briefly existing before
//   MusicBlocker kills it — there's no way to remove that flicker
//   short of rebinding the system's CD handler at the LaunchServices
//   level, which is more invasive than the project's scope.

#include "CdShield.h"

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOCDMedia.h>
#include <IOKit/storage/IOMedia.h>

#include <cstddef>
#include <string>
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

std::string bsdNameOf(DADiskRef disk) {
    const char* p = DADiskGetBSDName(disk);
    return p ? std::string(p) : std::string();
}

} // namespace

// State carried in the DA context: the claim set + the shield's
// listeners. CFMutableSetRef sits in `claimed`; the shield owns the
// listeners. One context per session avoids the
// "context-as-CFMutableSet" overload from before, which left no room to
// route disc events back into the shield instance.
struct ShieldCtx {
    CFMutableSetRef    claimed = nullptr;
    CdShield::DiscListener* appeared    = nullptr;
    CdShield::DiscListener* disappeared = nullptr;
};

namespace {

void diskAppeared(DADiskRef disk, void* raw) {
    auto* sc = static_cast<ShieldCtx*>(raw);
    if (!diskHasAudioTrack(disk)) return;  // data CDs: let Finder handle them

    CFSetAddValue(sc->claimed, disk);  // CFSet retains via kCFTypeSetCallBacks
    DADiskClaim(disk,
                kDADiskClaimOptionDefault,
                /*release cb*/ nullptr, /*release ctx*/ nullptr,
                /*claim cb*/   nullptr, /*claim ctx*/   nullptr);
    if (sc->appeared && *sc->appeared) {
        (*sc->appeared)(bsdNameOf(disk));
    }
}

void diskDisappeared(DADiskRef disk, void* raw) {
    auto* sc = static_cast<ShieldCtx*>(raw);
    CFSetRemoveValue(sc->claimed, disk);  // CFSet releases the held ref
    if (sc->disappeared && *sc->disappeared) {
        (*sc->disappeared)(bsdNameOf(disk));
    }
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

    // ShieldCtx owns the claim set + pointers to the shield's listener
    // members so the C callbacks can wake the Ripper without needing
    // an extra back-pointer to the shield itself.
    auto* ctx = new ShieldCtx{
        claimed,
        &appearedListener_,
        &disappearedListener_,
    };

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
                                   &diskAppeared, ctx);
    DARegisterDiskDisappearedCallback(session, match,
                                      &diskDisappeared, ctx);
    CFRelease(match);

    DASessionScheduleWithRunLoop(session, CFRunLoopGetMain(),
                                 kCFRunLoopCommonModes);

    session_ = session;   // owns +1
    claimed_ = ctx;       // owns the heap-allocated ShieldCtx + the CFSet
}

void CdShield::stop() {
    if (!session_) return;

    DASessionRef session = static_cast<DASessionRef>(session_);
    auto*        ctx     = static_cast<ShieldCtx*>(claimed_);

    DASessionUnscheduleFromRunLoop(session, CFRunLoopGetMain(),
                                   kCFRunLoopCommonModes);

    // Drop both callbacks. DAUnregisterCallback takes the function
    // pointer cast to void*, plus the same context we registered with.
    DAUnregisterCallback(session, reinterpret_cast<void*>(&diskAppeared),
                         ctx);
    DAUnregisterCallback(session, reinterpret_cast<void*>(&diskDisappeared),
                         ctx);

    // Unclaim every disc we currently hold. Snapshot the set's
    // contents into a vector first so the iteration order is stable
    // and we don't have to worry about DA mutating things mid-loop.
    const CFIndex n = CFSetGetCount(ctx->claimed);
    if (n > 0) {
        std::vector<const void*> disks(static_cast<std::size_t>(n));
        CFSetGetValues(ctx->claimed, disks.data());
        for (const void* v : disks) {
            DADiskUnclaim(reinterpret_cast<DADiskRef>(const_cast<void*>(v)));
        }
    }

    CFRelease(ctx->claimed);
    delete ctx;
    CFRelease(session);

    session_ = nullptr;
    claimed_ = nullptr;
}

void CdShield::setOnDiscAppeared(DiscListener listener) {
    appearedListener_ = std::move(listener);
    // Replay the current set so a disc already claimed at the time the
    // listener is installed still wakes the Ripper. DA's own callback
    // replay happens at register-time, which is before this listener
    // exists — so we synthesize the replay manually.
    if (!claimed_ || !appearedListener_) return;
    auto* ctx = static_cast<ShieldCtx*>(claimed_);
    const CFIndex n = CFSetGetCount(ctx->claimed);
    if (n == 0) return;
    std::vector<const void*> disks(static_cast<std::size_t>(n));
    CFSetGetValues(ctx->claimed, disks.data());
    for (const void* v : disks) {
        appearedListener_(bsdNameOf(reinterpret_cast<DADiskRef>(const_cast<void*>(v))));
    }
}

void CdShield::setOnDiscDisappeared(DiscListener listener) {
    disappearedListener_ = std::move(listener);
}

} // namespace plyr::cd
