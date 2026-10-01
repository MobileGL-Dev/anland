#ifndef ANLAND_SERVER_FRAME_H
#define ANLAND_SERVER_FRAME_H

#include <stdbool.h>
#include <android/native_window.h>

// The frames the render server draws into, and how this side puts them on the glass.
//
// MEASURED, and the reason this exists: AHardwareBuffer_createFromHandle accepts a buffer this
// process allocated itself (rc=0) and refuses one the display queue handed over (rc=3, the
// gralloc mapper's BAD_VALUE) -- same process, same run, same descriptor, same handle shape
// (numFds=2 numInts=26).  So the architecture cannot be "re-import what SurfaceFlinger
// allocated".  It is: allocate the frame here, let the server draw into it over the bridge, and
// hand THAT to the compositor through the NDK's own door for an AHardwareBuffer -- no Java, no
// reflection, no hidden API.
//
// The Anland display path is untouched: this is a second, independent way for pixels to reach
// the glass, and it is the one the render server can draw into.
//
// Idempotent for the same window at the same size.  A different window (a restarted session) or
// size (a surface change, e.g. the keyboard opening) replaces the presenting pool: the old one
// finishes its exchange, takes its layer off the window and frees itself.  A no-op if nothing is
// listening on the bridge: an absent render server is not a failure of the display.
void server_frames_start_async(ANativeWindow *window, int width, int height);

#endif  // ANLAND_SERVER_FRAME_H
