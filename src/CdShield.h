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

#include <functional>
#include <string>

#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if !TARGET_OS_IPHONE
    #define CONCERTO_HAS_CD_SHIELD 1
  #endif
#endif

namespace concerto::cd {

#ifdef CONCERTO_HAS_CD_SHIELD

class CdShield {
public:
    // Listener invoked when an audio-bearing CD appears or disappears in
    // any drive. Fires on the main run loop. `bsdName` is the BSD device
    // identifier without the "/dev/" prefix (e.g. "disk5") — the same
    // string CdDevice::open() expects. Set to {} to clear.
    //
    // The shield owns its DA claim across the listener's lifetime; the
    // listener is purely informational — it does NOT need to manage
    // claims or open the device. CdDevice::open() reads the raw character
    // device independently of DA claim state.
    using DiscListener = std::function<void(const std::string& bsdName)>;

    CdShield() = default;
    ~CdShield();

    CdShield(const CdShield&)            = delete;
    CdShield& operator=(const CdShield&) = delete;

    void start();
    void stop();
    bool active() const { return session_ != nullptr; }

    // Set the appeared / disappeared callbacks. If a disc is already
    // present when `setOnDiscAppeared` is called, the appeared callback
    // fires synchronously with the current disc — so the Ripper doesn't
    // miss a disc that was already in the drive when its session opened.
    void setOnDiscAppeared(DiscListener listener);
    void setOnDiscDisappeared(DiscListener listener);

private:
    void* session_ = nullptr;   // DASessionRef
    void* claimed_ = nullptr;   // heap-allocated ShieldCtx (owns CFSet)
    DiscListener appearedListener_;
    DiscListener disappearedListener_;
};

#else  // no-op on iOS / non-Apple

class CdShield {
public:
    using DiscListener = std::function<void(const std::string&)>;
    void start() {}
    void stop()  {}
    bool active() const { return false; }
    void setOnDiscAppeared(DiscListener)    {}
    void setOnDiscDisappeared(DiscListener) {}
};

#endif

} // namespace concerto::cd
