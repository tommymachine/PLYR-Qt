#include "macos_titlebar.h"

#import <AppKit/AppKit.h>

#include <QWindow>

namespace concerto {

void hideMacWindowTitle(QWindow* window)
{
    if (!window) return;
    NSView* view = reinterpret_cast<NSView*>(window->winId());
    if (!view) return;
    NSWindow* ns = view.window;
    if (!ns) return;
    ns.titleVisibility = NSWindowTitleHidden;
    // With NoTitleBarBackgroundHint (titlebarAppearsTransparent = YES),
    // what shows through the title bar is NSWindow.backgroundColor —
    // which Qt leaves at the system default (light/dark gray). Force it
    // to black so the title bar matches the QML scene below it.
    ns.backgroundColor = [NSColor blackColor];
}

} // namespace concerto
