// Last-line defense against macOS auto-launching Music.app / iTunes
// during a CD rip session.
//
// The primary defense is CdShield, which DA-claims any inserted disc
// before Music can react — so in practice this observer rarely fires
// at all. It stays as a safety net for the rare configuration where
// the OS still tries to fresh-launch Music in response to some legacy
// CD-handler hook even when there's no disc for it to handle.
//
// Mechanism (macOS): register a single NSWorkspace observer on
// willLaunchApplicationNotification, filter by bundle ID
// (com.apple.Music, com.apple.iTunes), and forceTerminate the
// NSRunningApplication before its UI appears.
//
// Already-running Music is intentionally left alone — CdShield's
// claim is responsible for keeping it from getting the disc event.
// If it does activate anyway, the user can dismiss it like any other
// foreground app; we no longer fight Cmd-Tab into it.
//
// No entitlements required. No-op on iOS and non-Apple platforms.

#pragma once

#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if !TARGET_OS_IPHONE
    #define CONCERTO_HAS_MUSIC_BLOCKER 1
  #endif
#endif

namespace concerto {

#ifdef CONCERTO_HAS_MUSIC_BLOCKER

class MusicBlocker {
public:
    MusicBlocker() = default;
    ~MusicBlocker();

    MusicBlocker(const MusicBlocker&)            = delete;
    MusicBlocker& operator=(const MusicBlocker&) = delete;

    void start();
    void stop();
    bool active() const { return observer_ != nullptr; }

private:
    void* observer_ = nullptr;  // NSWorkspace willLaunch observer token
};

#else  // no-op on iOS / non-Apple

class MusicBlocker {
public:
    void start() {}
    void stop()  {}
    bool active() const { return false; }
};

#endif

} // namespace concerto
