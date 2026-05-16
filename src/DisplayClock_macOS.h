// DisplayClock — Mac-only display-link shim.
//
// Qt's render path (QQuickWindow / QSGRenderLoop) exposes frameSwapped /
// beforeRendering / afterRendering, but none of them carry a target-
// presentation timestamp. Qt drives the threaded render loop from a
// QTimer pegged to nominal QScreen::refreshRate(), so there is no place
// in Qt's public API where we can ask "when will the next frame appear
// on screen?".
//
// We sidestep Qt by asking NSView for a CADisplayLink (macOS 14+) bound
// to the screen the window lives on. The link's targetTimestamp is the
// wall-clock instant Apple's compositor will scan out the next frame
// (CFTimeInterval, seconds-since-boot, same base as CACurrentMediaTime
// and AudioClock::nowSeconds — they compose directly).
//
// We deliberately do NOT use CAMetalDisplayLink, even though it carries
// a richer per-update payload: attaching one to Qt's CAMetalLayer breaks
// Qt's RHI because Apple disallows -[CAMetalLayer nextDrawable] once a
// CAMetalDisplayLink has been registered, and Qt's QRhiMetal calls
// nextDrawable every frame. NSView's displayLinkWithTarget:selector: is
// a pure observer with no exclusivity contract.
//
// Pre-macOS 14 falls back to CVDisplayLink, the same primitive Apple
// uses internally for AVSampleBufferRenderSynchronizer.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

class QQuickWindow;

namespace plyr::sync {

struct DisplayAnchor {
    uint64_t version                = 0;
    double   targetPresentationSec  = 0.0;  // CACurrentMediaTime base
    double   refreshIntervalSec     = 0.0;  // best-known frame interval
};

class DisplayClock {
public:
    DisplayClock();
    ~DisplayClock();
    DisplayClock(const DisplayClock&)            = delete;
    DisplayClock& operator=(const DisplayClock&) = delete;

    // Attach a CAMetalDisplayLink to the CAMetalLayer of the window's
    // NSView. Called from the GUI thread; the window must have been
    // created already. Idempotent. Returns true on success.
    //
    // Falls back to a CVDisplayLink if CAMetalDisplayLink isn't
    // available (macOS 13 and earlier).
    bool attach(QQuickWindow* window);

    // Detach (used by destructor).
    void detach();

    // Read the latest predicted target time. std::nullopt until the
    // display link has fired once.
    std::optional<DisplayAnchor> load() const;

    // Impl is forward-declared but exposed publicly in DisplayClock_macOS.mm
    // — the Obj-C bridge delegate needs a typed pointer back. Outside of
    // the .mm file no one should poke at it.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace plyr::sync
