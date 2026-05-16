// macos_titlebar.h
//
// Tiny macOS-only NSWindow tweak that Qt 6.11 doesn't expose as a window
// flag: hide the title text. Pairs with QML flag NoTitleBarBackgroundHint
// (0x00800000), which makes the title-bar background transparent but
// leaves the title text rendered on top of the QML scene.
//
// SAFE to call alongside Qt's window flag handling — Qt's Cocoa plugin
// never reads or writes NSWindow.titleVisibility (verified against the
// PureBibleQt sibling project, which uses the same shim).

#pragma once

class QWindow;

namespace concerto {

void hideMacWindowTitle(QWindow* window);

} // namespace concerto
