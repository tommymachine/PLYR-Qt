// Exclusive-claim shield for audio CDs while Concerto is running.
//
// Problem: when an audio CD is inserted, macOS notifies registered
// handlers (still, in 2026, Music.app in many configurations). Music
// reacts by activating itself and reading the disc. While the user is
// in Concerto we want exclusive control — both for correctness (no
// other process competing for sector reads) and UX (no foreground
// hijack).
//
// Mechanism: open a Disk Arbitration session bound to the main run
// loop, register a "disk appeared" callback matching IOCDMedia, and
// DADiskClaim each audio-bearing disc on appearance. The
// audio-bearing check (any partition with IOMediaContent == "CD_DA")
// keeps the shield out of the way of data CDs — Finder still mounts
// installer / photo / mixed-purpose data discs normally. A claimed
// disc is exclusive to the claimant; Music's CD handler can't touch
// it. When Concerto exits we DADiskUnclaim everything and the discs
// return to normal handling.
//
// This is the same DA-claim that CdDevice::open() will need anyway
// for exclusive sector reads. For now CdShield owns the claim across
// the whole app lifetime; once a rip is in progress the claim is
// logically transferred from shield → device for the duration of the
// actual rip.
//
// All callbacks fire on the main thread. start()/stop() must also be
// called on the main thread. No-op on iOS / non-Apple platforms.

#pragma once

#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if !TARGET_OS_IPHONE
    #define CONCERTO_HAS_CD_SHIELD 1
  #endif
#endif

namespace plyr::cd {

#ifdef CONCERTO_HAS_CD_SHIELD

class CdShield {
public:
    CdShield() = default;
    ~CdShield();

    CdShield(const CdShield&)            = delete;
    CdShield& operator=(const CdShield&) = delete;

    void start();
    void stop();
    bool active() const { return session_ != nullptr; }

private:
    void* session_ = nullptr;   // DASessionRef
    void* claimed_ = nullptr;   // CFMutableSetRef of DADiskRef
};

#else  // no-op on iOS / non-Apple

class CdShield {
public:
    void start() {}
    void stop()  {}
    bool active() const { return false; }
};

#endif

} // namespace plyr::cd
