// Real implementation of prewarmOpenPanel(). Only compiled on macOS
// (CMake's APPLE AND NOT IOS guard); on iOS this file is excluded.

#import <AppKit/AppKit.h>

extern "C" void prewarmOpenPanel()
{
    @autoreleasepool {
        // Instantiating the panel is enough to trigger:
        //   - dyld loads of AppKit / QuickLook / CoreServices helpers
        //   - XPC connection to com.apple.appkit.xpc.openAndSavePanelService
        //   - Launch Services + IconServicesAgent warmup
        // The object goes out of scope immediately; we never show it.
        NSOpenPanel* p = [NSOpenPanel openPanel];
        (void)p;
    }
}
