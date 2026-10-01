// See server_frame.h for why this exists and what was measured.

#include "server_frame.h"

#include <android/hardware_buffer.h>
#include <android/surface_control.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ahb_bridge.h"

// Three is enough to have one on the glass while the server draws the next, and few enough that
// the pool costs nothing worth measuring.
#define SERVER_FRAMES 3

struct pool {
    AHardwareBuffer *buf[SERVER_FRAMES];
    ASurfaceControl *sc;
    ARect rect;
    int count;
    // The window this pool presents into (a reference of the pool's own) and the size it was made
    // for: a pool is for exactly one window at one size.
    ANativeWindow *window;
    int width;
    int height;
    volatile bool stop;
};

// THE POOL THAT IS PRESENTING NOW.  A session restart hands this side a new ANativeWindow
// (nativeStart releases the old one), and a surface change - the keyboard opening and closing -
// a new size; a pool made for the old window would keep presenting through a SurfaceControl of a
// released window, at a size that no longer fits.  So the current pool is replaced: the old one
// is told to stop, finishes the exchange it may be in (the bridge serialises them), takes its
// layer off the window and frees itself.
static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pool *s_current;

static void pool_free(struct pool *p)
{
    if (p->sc) {
        ASurfaceTransaction *tx = ASurfaceTransaction_create();
        if (tx) {
            ASurfaceTransaction_setVisibility(tx, p->sc, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
            ASurfaceTransaction_reparent(tx, p->sc, NULL);
            ASurfaceTransaction_apply(tx);
            ASurfaceTransaction_delete(tx);
        }
        ASurfaceControl_release(p->sc);
    }
    for (int i = 0; i < p->count; ++i)
        if (p->buf[i])
            AHardwareBuffer_release(p->buf[i]);
    if (p->window)
        ANativeWindow_release(p->window);
    free(p);
}

static void present(struct pool *p, int i)
{
    ASurfaceTransaction *tx = ASurfaceTransaction_create();
    if (!tx)
        return;
    // No fence to hand over: the bridge's offer already waited for the server's own fence before
    // it answered, so the pixels are complete by the time this runs.
    ASurfaceTransaction_setBuffer(tx, p->sc, p->buf[i], -1);
    ASurfaceTransaction_setVisibility(tx, p->sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    // No transform: this window is not rotated and the frame is already the size of it.
    ASurfaceTransaction_setGeometry(tx, p->sc, &p->rect, &p->rect, 0 /* no transform */);
    ASurfaceTransaction_apply(tx);
    ASurfaceTransaction_delete(tx);
}

static void *frames_thread(void *arg)
{
    struct pool *p = arg;
    // THE SERVER DRIVES, THIS SIDE ANSWERS.  The render server takes a frame when it needs one,
    // draws into it, says so, and takes the next; this loop is the other half of that
    // conversation and runs for as long as the window does.  A server that is not there yet is
    // waited for rather than given up on: it is a separate app that has to come up, and until it
    // does there is simply nothing to draw.
    int i = 0;
    while (!p->stop) {
        AHardwareBuffer_Desc d;
        AHardwareBuffer_describe(p->buf[i], &d);
        // The descriptor an import has to be told: what this buffer IS, not what a queue hands
        // back.  No guessing, because this side allocated it.
        if (!ahb_bridge_offer(p->buf[i], (uint32_t)i, d.width, d.height, d.stride * 4u, d.format, d.usage,
                              "host frame")) {
            usleep(200000);
            continue;
        }
        // A pool replaced while its exchange was in flight presents nothing more.
        if (p->stop)
            break;
        // The server has drawn into it and answered; the frame is this side's to put on the
        // glass.
        present(p, i);
        ahb_bridge_note("frames: presented %d (%ux%u)", i, d.width, d.height);
        i = (i + 1) % p->count;
    }
    ahb_bridge_note("frames: thread done (%dx%d)", p->width, p->height);
    pool_free(p);
    return NULL;
}

void server_frames_start_async(ANativeWindow *window, int width, int height)
{
    if (!window || width <= 0 || height <= 0)
        return;

    pthread_mutex_lock(&s_pool_lock);
    if (s_current && s_current->window == window && s_current->width == width && s_current->height == height) {
        pthread_mutex_unlock(&s_pool_lock);
        return;
    }

    struct pool *p = calloc(1, sizeof(*p));
    if (!p) {
        pthread_mutex_unlock(&s_pool_lock);
        return;
    }
    p->width = width;
    p->height = height;

    AHardwareBuffer_Desc d;
    memset(&d, 0, sizeof(d));
    d.width = (uint32_t)width;
    d.height = (uint32_t)height;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    // What the server needs and what this side needs, in one allocation: the two GPU bits so the
    // server can sample and draw, CPU read so this side can look at the pixels afterwards and say
    // whether the drawing arrived.
    d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
              AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;

    for (int i = 0; i < SERVER_FRAMES; ++i) {
        if (AHardwareBuffer_allocate(&d, &p->buf[i]) != 0 || !p->buf[i]) {
            ahb_bridge_note("frames: allocate %d failed", i);
            break;
        }
        p->count++;
    }
    ahb_bridge_note("frames: %d of %d self-allocated %dx%d usage=0x%llx", p->count, SERVER_FRAMES, width, height,
                    (unsigned long long)d.usage);
    if (p->count == 0) {
        pool_free(p);
        pthread_mutex_unlock(&s_pool_lock);
        return;
    }

    // A child of this window: the frame is what the window shows, and the window's own queue
    // keeps feeding the Anland display path untouched.  The pool holds a reference of its own,
    // so the window outlives a session that releases its copy before this pool has stopped.
    ANativeWindow_acquire(window);
    p->window = window;
    p->sc = ASurfaceControl_createFromWindow(window, "mobilegl-server-frame");
    if (!p->sc) {
        ahb_bridge_note("frames: no surface control for this window");
        pool_free(p);
        pthread_mutex_unlock(&s_pool_lock);
        return;
    }
    p->rect.left = 0;
    p->rect.top = 0;
    p->rect.right = width;
    p->rect.bottom = height;

    pthread_t t;
    if (pthread_create(&t, NULL, frames_thread, p) != 0) {
        ahb_bridge_note("frames: could not start the thread");
        pool_free(p);
        pthread_mutex_unlock(&s_pool_lock);
        return;
    }
    pthread_detach(t);
    // The previous pool stops after its exchange and frees itself; this one presents from now on.
    if (s_current) {
        ahb_bridge_note("frames: replacing the %dx%d pool with a %dx%d one", s_current->width, s_current->height,
                        width, height);
        s_current->stop = true;
    }
    s_current = p;
    pthread_mutex_unlock(&s_pool_lock);
}
