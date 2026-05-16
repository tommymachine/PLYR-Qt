# CD Shield

A small Disk Arbitration shield that prevents macOS Music.app from
hijacking an inserted CD. Held open by the Qt rip view for the
duration of a rip session.

## The name

`concerto::cd::CdShield` — a regular C++ class. No CLI, no IPC, no
background process. Just include the header and use it.

## Where it lives

| Piece | Path |
| --- | --- |
| Public interface | `src/CdShield.h` |
| macOS implementation | `src/CdShield_macOS.cpp` |
| Build wiring | `CMakeLists.txt` (already linked into `concerto`) |

Already part of the `concerto` target. AppKit + DiskArbitration
frameworks are linked alongside the existing macOS sources.

## How to use it from the rip view

Hold one as a member of whatever controller owns the rip view's
lifetime. Call `start()` when the view is shown, `stop()` when it
closes. The destructor calls `stop()` too, so just letting the
object go out of scope is also fine.

```cpp
#include "CdShield.h"

class RipController : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void openRipView()  { shield_.start(); /* ...show UI...*/ }
    Q_INVOKABLE void closeRipView() { shield_.stop();  /* ...tear down...*/ }
private:
    concerto::cd::CdShield shield_;
};
```

`start()` and `stop()` are idempotent — calling `start()` twice is a
no-op, same for `stop()`. They must be called from the main thread
(same thread as the Qt event loop) because the DA session is
scheduled on the main CFRunLoop.

That's the whole integration. No setup, no teardown beyond
`start()`/`stop()`. The header is a no-op on iOS and non-Apple
platforms, so the same code compiles everywhere.

## What it does

When `start()` is called, it:

1. Opens a Disk Arbitration session on the main run loop.
2. Registers a match for `IOCDMedia` (covers audio, data, and mixed
   CDs — DVDs/Blu-ray are out of scope).
3. For every CD that appears — including one already in the drive —
   calls `DADiskClaim` with permanent options.

Claimed discs are exclusive to the claimant. Music's CD handler,
Finder, and the system mounter all see "this disc is taken" and
silently leave it alone. No Apple Event reaches Music, so Music
doesn't activate.

When `stop()` is called, it unclaims everything it's holding and
tears down the DA session. Discs return to normal handling.

## What it does NOT do

- It does **not** kill Music if Music is already running and the user
  manually clicks on it. Cmd-Tab and dock clicks work normally.
- It does **not** prevent Music from launching in response to other
  triggers (e.g., a media-key press). For belt-and-suspenders against
  the rare fresh-launch case, the same module ships
  `concerto::MusicBlocker` (see `src/MusicBlocker.h`) — a `willLaunch`
  observer that force-terminates Music if the OS tries to spin it up.
  Use it the same way as `CdShield` if you want both.
- It does **not** filter to audio-only CDs. A data CD inserted while
  the shield is active will also be claimed (Finder won't mount it).
  This is by design — the rip flow can inspect the TOC after claiming
  and decide what to do.

## Future: handoff to `CdDevice::open()`

The shield holds a `DADiskRef` for each claimed disc. Once
`CdDevice::open()` is implemented (MVP step 2), the disc that the
user wants to rip will need a DA-claimed handle for exclusive sector
reads. The shield is already that handle — at that point we'll
either pass the `DADiskRef` from shield to device, or release the
shield's claim on that specific disc just before `CdDevice::open()`
reclaims it. Either works; pick whichever is cleaner once the
`open()` implementation lands.

## Manual verification (optional)

`cdrip_cli --shield` runs `CdShield` standalone with no Qt around
it, so you can test the behavior without wiring the rip view first.
Insert a CD; Music should stay quiet. Ctrl-C releases. Useful when
iterating on the shield itself; not part of any integration path.
