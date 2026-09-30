// ahb_bridge.h -- the display host's half of "the render server draws into the buffer
// that is scanned out".
//
// WHY THIS EXISTS.  A render server that is not the compositor has to be given the image
// in a form it can take, and on this driver there is exactly one: an AHardwareBuffer.  A
// container's dma-buf is not it (measured: no EGL_EXT_image_dma_buf_import on Adreno 750,
// and AHardwareBuffer_createFromHandle turns a bare fd down), and an app-allocated buffer
// is not it either -- that would cost the host its own scanout path.  What DOES work, and
// what this file does, is the third thing: the buffer the host has already dequeued is a
// real gralloc allocation, so the platform can hand back an AHardwareBuffer for it, and the
// server then draws into the very memory that is being displayed.
//
// The container's half is unchanged: the same buffer still travels to the container as a
// dma-buf fd plus the layout in buf_info, and the compositor still renders "into" it.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// ANativeWindowBuffer, not ANativeWindow_Buffer: the NDK's native_window.h only declares
// the latter, and the buffer the queue hands back (with its gralloc handle) is the one the
// project already declares for itself in anw_hidden.h.
#include "anw_hidden.h"

// One buffer, announced to whoever is listening on the bridge.  Fixed size and packed so
// the other end can mirror it byte for byte.  The AHardwareBuffer itself travels AHEAD of
// this struct on the same socket (AHardwareBuffer_sendHandleToUnixSocket), and that order
// is part of the protocol: the receiver reads the handle first, then this.
#define AHB_BRIDGE_MAGIC 0x4D474C41u  // 'ALGM', MobileGL read back to front
struct ahb_offer {
    uint32_t magic;
    uint32_t version;
    uint32_t index;   // the display host's own buffer index, stable for the window's life
    uint32_t width;
    uint32_t height;
    uint32_t stride;  // BYTES per row -- what buf_info carries, not pixels
    uint32_t format;  // the AHardwareBuffer format the wrap accepted
    uint32_t usage_lo;
    uint32_t usage_hi;  // the usage the wrap accepted, split to keep the struct 8-byte clean
    char note[64];
} __attribute__((packed));

// The other two halves of the exchange, both fixed size and packed like the offer.
//
// The round trip is checked from the RECEIVER's side on purpose.  A server that imports an
// image and clears a colour into it has proved that the platform took the buffer, not that
// the bytes landed where the display host can see them -- and "landed where the host can
// see them" is the entire question.  So the host reads both pixels back through its own
// view of the same buffer and returns them, and the receiver decides.
struct ahb_ack {  // receiver -> host: what the render server did with the buffer
    uint32_t magic;
    uint32_t version;
    uint32_t index;
    uint32_t imported;   // an EGLImage was created for it
    uint32_t drawn;      // the colour below was cleared through that image
    uint32_t fence_ok;   // the fence behind the clear reported a finished GPU
    uint8_t color[4];    // the colour cleared, in memory order
    char note[64];
} __attribute__((packed));

struct ahb_seen {  // host -> receiver: what the host's own view of the buffer holds now
    uint32_t magic;
    uint32_t version;
    uint32_t index;
    uint32_t locked;   // AHardwareBuffer_lock succeeded
    uint8_t observed[4];
    uint8_t observed_corner[4];
    char note[64];
} __attribute__((packed));

// How long the host waits for the receiver's answer before deciding nothing is listening.
// How long the host waits for the receiver's answer before it decides nothing is listening.
// It has to cover the receiver's first swap, not a round trip: the answer to a frame comes when
// the render server has DRAWN it, and a compositor may take seconds to reach its first one.
// Giving up early does not lose the frame - the next attempt re-offers it - but it makes the
// two ends disagree about which frame is current, which is worth 26 seconds of patience to avoid.
#define AHB_BRIDGE_ANSWER_MS 30000

// Where a listener may be: the same directory the daemon socket lives in.  An image bridge
// that is not running is not an error -- it only means nothing here is offering to render.
// AN ABSTRACT NAME, not a path.  Both ends are separate apps, and neither may create a socket
// file in /data/local/tmp (its directory grants others traverse only) - so a path is a place
// neither could listen.  An abstract name lives in the kernel namespace instead of the
// filesystem, which is the same reason this project names its own endpoints @mgl-*.
#define AHB_BRIDGE_SOCKET "@mobilegl-host-frame"

// Wraps a buffer the display host has dequeued -- one that is about to be scanned out -- as
// an AHardwareBuffer, so a process that is not this one can take it as an EGLImage.  False
// with `why` filled when the platform refuses.
//
// The usage is the one part of the description a dequeued buffer cannot tell us, so a small
// ladder of them is tried and the accepted one is what the offer carries: a refusal is then
// a fact about this handle on this device instead of "the platform cannot wrap at all".
// The same evidence channel the bridge uses, exposed so the rest of the native pipeline can
// say where it got to: this device does not show this app own logcat lines, and a pipeline
// that is invisible cannot be debugged.
void ahb_bridge_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// The same wrap with the geometry spelled out: what an import has to be told is the geometry
// the buffers were ALLOCATED at, and the surface does not always report that (a cropped
// surface reports its own size, and the mapper answers BAD_VALUE for the mismatch).
bool ahb_bridge_wrap_desc(const native_handle_t *handle, uint32_t width, uint32_t height, uint32_t stride,
                          uint32_t format, void **out_ahb, char *why, unsigned why_size);
bool ahb_bridge_wrap(const ANativeWindowBuffer *anb, void **out_ahb, char *why, unsigned why_size);

// The usage bits the last successful wrap was accepted with, for the offer's description.
uint64_t ahb_bridge_last_usage(void);

// Sends the handle and then its description.  False when the bridge went away, which also
// drops the cached socket so the next offer tries to reconnect.
bool ahb_bridge_offer(void *ahb, uint32_t index, uint32_t width, uint32_t height, uint32_t stride_bytes,
                      uint32_t format, uint64_t usage, const char *note);

// Releases a wrapped buffer.  It is held for the window's life -- a reference the host
// forgot it had would starve the buffer queue -- so this runs when the window goes away.
void ahb_bridge_release(void *ahb);
