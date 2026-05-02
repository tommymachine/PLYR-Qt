// macOS startup warmup — pre-load AppKit's NSOpenPanel so the first
// user-visible FolderDialog.open() doesn't stall the main thread.
//
// The first invocation of NSOpenPanel on a cold app session spawns the
// com.apple.appkit.xpc.openAndSavePanelService XPC process, dlopens
// additional AppKit/QuickLook/CoreServices code paths, and populates
// Launch Services + icon caches. That can take 100-500 ms on the main
// thread — long enough to drain Qt's audio ring buffer (default 250 ms,
// 500 ms in our build) and produce an audible skip when the user clicks
// "Open Folder…".
//
// Calling this during startup pays the cost once, early, when a brief
// stall is invisible. On non-macOS platforms this is a compile-time no-op.

#pragma once

#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if !TARGET_OS_IPHONE
    #define CONCERTO_HAS_MACOS_PREWARM 1
  #endif
#endif

#ifdef CONCERTO_HAS_MACOS_PREWARM
extern "C" void prewarmOpenPanel();
#else
inline void prewarmOpenPanel() {}
#endif
