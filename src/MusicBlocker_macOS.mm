// macOS implementation of plyr::MusicBlocker. See MusicBlocker.h for
// the design rationale.
//
// Block-based observer instead of a selector + target — same semantics
// as NoTunes (https://github.com/tombonez/noTunes, MIT) for the
// will-launch path. The observer token returned by addObserverForName:
// is autoreleased; this TU compiles without ARC (matching the rest of
// the project's Obj-C++), so we retain manually into `observer_` and
// release in stop().

#import <AppKit/AppKit.h>

#include "MusicBlocker.h"

namespace plyr {

namespace {

bool isMusicOrItunes(NSString* bid) {
    return [bid isEqualToString:@"com.apple.Music"]
        || [bid isEqualToString:@"com.apple.iTunes"];
}

} // namespace

MusicBlocker::~MusicBlocker() {
    stop();
}

void MusicBlocker::start() {
    if (observer_) return;
    @autoreleasepool {
        NSNotificationCenter* nc = [[NSWorkspace sharedWorkspace] notificationCenter];

        id token = [nc addObserverForName:NSWorkspaceWillLaunchApplicationNotification
                                   object:nil
                                    queue:nil
                               usingBlock:^(NSNotification* note) {
            NSRunningApplication* app = note.userInfo[NSWorkspaceApplicationKey];
            if (isMusicOrItunes(app.bundleIdentifier)) {
                [app forceTerminate];
            }
        }];
        [token retain];
        observer_ = token;
    }
}

void MusicBlocker::stop() {
    if (!observer_) return;
    id token = static_cast<id>(observer_);
    observer_ = nullptr;
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:token];
    [token release];
}

} // namespace plyr
