// DisplayClock — implementation. Compiled only on macOS via CMake.
//
// Two paths:
//
//   macOS 14+ : -[NSView displayLinkWithTarget:selector:] gives us a
//               CADisplayLink bound to the screen the view lives on.
//               targetTimestamp is the wall-clock instant the next frame
//               will appear (seconds-since-boot, same base as
//               CACurrentMediaTime and our AudioClock::hostToSeconds).
//
//               (We initially tried CAMetalDisplayLink, but attaching
//               one to Qt's CAMetalLayer breaks Qt's RHI: once a
//               CAMetalDisplayLink is registered, Apple forbids the
//               framework rendering to that layer from calling
//               -[CAMetalLayer nextDrawable]. Qt's QRhiMetal calls
//               nextDrawable on every frame, so the layer-based variant
//               was a non-starter. The NSView-based CADisplayLink is
//               purely observational — no exclusivity contract.)
//
//   macOS 13- : CVDisplayLink driven from the screen's CGDirectDisplayID.
//               CVTimeStamp.hostTime is mach_absolute_time, converted
//               to seconds in the same base.

#include "DisplayClock_macOS.h"

#include <QQuickWindow>

#include <QuartzCore/QuartzCore.h>
#include <AppKit/AppKit.h>
#include <CoreVideo/CVDisplayLink.h>
#include <mach/mach_time.h>

#include <atomic>

namespace plyr::sync {

namespace {

double machRatioDC()
{
    static const double r = []() {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return double(info.numer) / double(info.denom);
    }();
    return r;
}

double hostTicksToSeconds(uint64_t ticks)
{
    return double(ticks) * machRatioDC() * 1.0e-9;
}

} // namespace

} // namespace plyr::sync

// Bridging delegate for CADisplayLink. Plain Obj-C class — kept inside
// the implementation file so the public header stays pure C++.
@interface ConcertoDisplayLinkBridge : NSObject
@property(nonatomic) void* implPtr;   // plyr::sync::DisplayClock::Impl*
- (void)displayLinkTick:(CADisplayLink*)link;
@end

namespace plyr::sync {

struct DisplayClock::Impl {
    bool                       m_attached     = false;
    bool                       m_modernPath   = false;
    CADisplayLink*             m_displayLink  = nil;
    ConcertoDisplayLinkBridge* m_delegate     = nil;
    CVDisplayLinkRef           m_cvLink       = nullptr;

    // Seqlock state. Same pattern as AudioClock::Impl — odd seq = writing,
    // even = done; readers spin until v0 == v1 and v0 is even.
    std::atomic<uint64_t> m_seq{0};
    double                m_target          = 0.0;
    double                m_interval        = 0.0;
    uint64_t              m_callbackCount   = 0;

    void publish(double targetSec, double intervalSec)
    {
        const uint64_t v0 = m_seq.load(std::memory_order_relaxed);
        m_seq.store(v0 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        m_target        = targetSec;
        m_interval      = intervalSec;
        m_callbackCount += 1;
        std::atomic_thread_fence(std::memory_order_release);
        m_seq.store(v0 + 2, std::memory_order_release);
    }

    // CVDisplayLink callback. Runs on a dedicated high-priority CV thread.
    static CVReturn cvCallback(CVDisplayLinkRef               /*link*/,
                               const CVTimeStamp*             /*inNow*/,
                               const CVTimeStamp*             inOutputTime,
                               CVOptionFlags                  /*flagsIn*/,
                               CVOptionFlags*                 /*flagsOut*/,
                               void*                          displayCtx)
    {
        if (!inOutputTime || !displayCtx) return kCVReturnSuccess;
        auto* self = static_cast<Impl*>(displayCtx);
        // hostTime is mach_absolute_time; convert to seconds (CA base).
        const double t = hostTicksToSeconds(inOutputTime->hostTime);
        double interval = 0.0;
        if (inOutputTime->videoRefreshPeriod > 0 &&
            inOutputTime->videoTimeScale     > 0) {
            interval = double(inOutputTime->videoRefreshPeriod) /
                       double(inOutputTime->videoTimeScale);
        }
        self->publish(t, interval);
        return kCVReturnSuccess;
    }
};

DisplayClock::DisplayClock()  : m_impl(std::make_unique<Impl>()) {}
DisplayClock::~DisplayClock() { detach(); }

bool DisplayClock::attach(QQuickWindow* window)
{
    if (!window || m_impl->m_attached) return m_impl->m_attached;

    NSView* view = reinterpret_cast<NSView*>(window->winId());
    if (!view) {
        NSLog(@"[DisplayClock] no NSView for QQuickWindow");
        return false;
    }

    // Modern path: -[NSView displayLinkWithTarget:selector:] returns a
    // CADisplayLink (macOS 14+). Pure observer — no exclusivity contract
    // with whatever framework is rendering to the view's metal layer.
    if ([view respondsToSelector:@selector(displayLinkWithTarget:selector:)]) {
        m_impl->m_delegate = [[ConcertoDisplayLinkBridge alloc] init];
        m_impl->m_delegate.implPtr = m_impl.get();
        m_impl->m_displayLink =
            [view displayLinkWithTarget:m_impl->m_delegate
                               selector:@selector(displayLinkTick:)];
        if (m_impl->m_displayLink) {
            [m_impl->m_displayLink addToRunLoop:[NSRunLoop mainRunLoop]
                                        forMode:NSRunLoopCommonModes];
            m_impl->m_modernPath = true;
            m_impl->m_attached   = true;
            const NSInteger fps = view.window.screen
                ? view.window.screen.maximumFramesPerSecond : 0;
            NSLog(@"[DisplayClock] attached CADisplayLink (NSView path) "
                  @"screen=%ld Hz", (long)fps);
            return true;
        }
        // displayLinkWithTarget:selector: returned nil — fall through to CV.
        NSLog(@"[DisplayClock] displayLinkWithTarget:selector: returned nil");
        m_impl->m_delegate = nil;
    }

    // Pre-macOS-14 fallback: CVDisplayLink against the screen the window
    // currently lives on.
    NSWindow* nsWin = view.window;
    if (!nsWin || !nsWin.screen) {
        NSLog(@"[DisplayClock] no NSScreen for fallback path");
        return false;
    }
    NSNumber* screenIDNum =
        nsWin.screen.deviceDescription[@"NSScreenNumber"];
    if (!screenIDNum) {
        NSLog(@"[DisplayClock] no NSScreenNumber");
        return false;
    }
    CGDirectDisplayID displayID =
        (CGDirectDisplayID)[screenIDNum unsignedIntValue];

    // CVDisplayLink was deprecated in macOS 15 in favor of
    // NSView.displayLink — but we only enter this branch on macOS 13.
    // Silence the deprecation locally so the build stays clean.
    CVReturn cvErr = kCVReturnSuccess;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    cvErr = CVDisplayLinkCreateWithCGDisplay(displayID, &m_impl->m_cvLink);
    if (cvErr == kCVReturnSuccess && m_impl->m_cvLink) {
        CVDisplayLinkSetOutputCallback(m_impl->m_cvLink,
                                       &Impl::cvCallback,
                                       m_impl.get());
        CVDisplayLinkStart(m_impl->m_cvLink);
    }
#pragma clang diagnostic pop
    if (cvErr != kCVReturnSuccess || !m_impl->m_cvLink) {
        NSLog(@"[DisplayClock] CVDisplayLinkCreateWithCGDisplay failed (%d)",
              cvErr);
        return false;
    }
    m_impl->m_modernPath = false;
    m_impl->m_attached = true;
    NSLog(@"[DisplayClock] Pre-macOS-14 — using CVDisplayLink fallback");
    return true;
}

void DisplayClock::detach()
{
    if (!m_impl || !m_impl->m_attached) return;

    if (m_impl->m_displayLink) {
        [m_impl->m_displayLink invalidate];
        m_impl->m_displayLink = nil;
    }
    m_impl->m_delegate = nil;

    if (m_impl->m_cvLink) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CVDisplayLinkStop(m_impl->m_cvLink);
        CVDisplayLinkRelease(m_impl->m_cvLink);
#pragma clang diagnostic pop
        m_impl->m_cvLink = nullptr;
    }
    m_impl->m_attached = false;
}

std::optional<DisplayAnchor> DisplayClock::load() const
{
    if (!m_impl || !m_impl->m_attached) return std::nullopt;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t v0 = m_impl->m_seq.load(std::memory_order_acquire);
        if (v0 == 0) return std::nullopt;
        if (v0 & 1u) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        DisplayAnchor a;
        a.version               = m_impl->m_callbackCount;
        a.targetPresentationSec = m_impl->m_target;
        a.refreshIntervalSec    = m_impl->m_interval;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t v1 = m_impl->m_seq.load(std::memory_order_acquire);
        if (v1 == v0) return a;
    }
    return std::nullopt;
}

} // namespace plyr::sync

// -------- Objective-C delegate ------------------------------------------

@implementation ConcertoDisplayLinkBridge

- (void)displayLinkTick:(CADisplayLink*)link
{
    if (!link || !self.implPtr) return;
    auto* impl = static_cast<plyr::sync::DisplayClock::Impl*>(self.implPtr);

    // CADisplayLink.targetTimestamp is the wall-clock instant the client
    // should target their render for — equivalent to the
    // targetPresentationTimestamp Apple's compositor will use to schedule
    // the next scanout. CFTimeInterval = seconds since boot in the same
    // base as CACurrentMediaTime + our converted mach_continuous_time.
    const double t = link.targetTimestamp;
    double interval = link.duration;
    if (interval <= 0.0) interval = 1.0 / 60.0;
    impl->publish(t, interval);
}

@end
