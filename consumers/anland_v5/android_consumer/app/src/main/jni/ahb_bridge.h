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

// Where a listener may be: the same directory the daemon socket lives in.  An image bridge
// that is not running is not an error -- it only means nothing here is offering to render.
#define AHB_BRIDGE_SOCKET "/data/local/tmp/mobilegl_bridge.sock"

// Wraps a buffer the display host has dequeued -- one that is about to be scanned out -- as
// an AHardwareBuffer, so a process that is not this one can take it as an EGLImage.  False
// with `why` filled when the platform refuses.
//
// The usage is the one part of the description a dequeued buffer cannot tell us, so a small
// ladder of them is tried and the accepted one is what the offer carries: a refusal is then
// a fact about this handle on this device instead of "the platform cannot wrap at all".
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
