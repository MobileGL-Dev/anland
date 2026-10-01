// ahb_bridge.c -- see the header for why any of this exists.

#include "ahb_bridge.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_TAG "AnlandAhbBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// AHardwareBuffer_createFromHandle is a SystemApi: the NDK does not declare it, and the
// platform does not care who calls it.  libnativewindow.so is where it is exported.
typedef int (*create_from_handle_fn)(const AHardwareBuffer_Desc *, const void *, int, AHardwareBuffer **);

// The handle type that takes a gralloc handle.  Both values are tried anyway, because the
// enum is not in the NDK and the two are one integer apart in meaning: on the measured
// device type 2 accepts a real gralloc handle and type 1 refuses it, and reading that off
// the platform is cheaper than trusting a transcribed constant.
enum { AHB_HANDLE_TYPE_TRY_FIRST = 2, AHB_HANDLE_TYPE_TRY_SECOND = 1 };

static create_from_handle_fn s_create;
typedef const native_handle_t *(*get_native_handle_fn)(const AHardwareBuffer *);
static get_native_handle_fn s_get_handle;
static bool s_loaded;
static uint64_t s_last_usage;
static int s_sock = -2;  // -2 = never tried, -1 = no bridge listening

static void load_api(void)
{
    if (s_loaded)
        return;
    s_loaded = true;
    void *lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        LOGW("dlopen(libnativewindow.so) -> %s", dlerror());
        return;
    }
    s_create = (create_from_handle_fn)dlsym(lib, "AHardwareBuffer_createFromHandle");
    s_get_handle = (get_native_handle_fn)dlsym(lib, "AHardwareBuffer_getNativeHandle");
    LOGI("AHardwareBuffer_createFromHandle = %p", (void *)s_create);
}

uint64_t ahb_bridge_last_usage(void)
{
    return s_last_usage;
}

bool ahb_bridge_wrap(const ANativeWindowBuffer *anb, void **out_ahb, char *why, unsigned why_size)
{
    if (!anb)
        return ahb_bridge_wrap_desc(NULL, 0, 0, 0, 0, out_ahb, why, why_size);
    return ahb_bridge_wrap_desc(anb->handle, (uint32_t)anb->width, (uint32_t)anb->height,
                               (uint32_t)anb->stride, (uint32_t)anb->format, out_ahb, why, why_size);
}

bool ahb_bridge_wrap_desc(const native_handle_t *handle, uint32_t width, uint32_t height, uint32_t stride,
                          uint32_t format, void **out_ahb, char *why, unsigned why_size)
{
    *out_ahb = NULL;
    load_api();
    if (!s_create) {
        snprintf(why, why_size, "AHardwareBuffer_createFromHandle is not loadable in this process");
        return false;
    }
    if (!handle) {
        snprintf(why, why_size, "the dequeued buffer carries no native handle");
        return false;
    }

    AHardwareBuffer_Desc d;
    memset(&d, 0, sizeof(d));
    d.width = width;
    d.height = height;
    d.layers = 1;
    // HAL_PIXEL_FORMAT_* and AHARDWAREBUFFER_FORMAT_* agree on the packed 32-bit formats,
    // which is the only kind a SurfaceView queue hands out here.
    d.format = format;
    d.stride = stride;

    // What the render server needs first: to sample the image and to draw into it.
    // CPU read is asked for FIRST even though the render server only needs the two GPU
    // bits: the host is the only party that can look at the pixels and say whether the
    // server's drawing arrived, and that read needs the usage it is declaring.  The
    // GPU-only pair stays as the fallback for a platform that refuses the extra bit.
    static const uint64_t usages[] = {
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
            AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
        AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
    };
    const int types[] = {AHB_HANDLE_TYPE_TRY_FIRST, AHB_HANDLE_TYPE_TRY_SECOND};
    ahb_bridge_note("wrap: %ux%u format=0x%x stride=%u numFds=%d numInts=%d", d.width, d.height, d.format, d.stride,
                    handle->numFds, handle->numInts);
    int last_rc = 0;

    for (unsigned t = 0; t < sizeof(types) / sizeof(types[0]); ++t) {
        for (unsigned u = 0; u < sizeof(usages) / sizeof(usages[0]); ++u) {
            d.usage = usages[u];
            AHardwareBuffer *ahb = NULL;
            const int rc = s_create(&d, handle, types[t], &ahb);
            ahb_bridge_note("  rung type=%d usage=0x%llx -> rc=%d ahb=%p", types[t],
                            (unsigned long long)usages[u], rc, (void *)ahb);
            last_rc = rc;
            if (rc == 0 && ahb) {
                s_last_usage = usages[u];
                ahb_bridge_note("  accepted: type=%d usage=0x%llx", types[t],
                                (unsigned long long)usages[u]);
                *out_ahb = ahb;
                return true;
            }
        }
    }
    ahb_bridge_note("  every rung refused it (last rc=%d)", last_rc);
    if (s_get_handle) {
        /* SIDE BY SIDE, same descriptor, same call, different provenance: a buffer this process
         * allocated itself against the one the display queue handed over.  It separates "the
         * platform will not take this descriptor" from "the platform will not take THIS buffer". */
        AHardwareBuffer_Desc ad;
        memset(&ad, 0, sizeof(ad));
        ad.width = width;
        ad.height = height;
        ad.layers = 1;
        ad.format = format;
        ad.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
        AHardwareBuffer *own = NULL;
        if (AHardwareBuffer_allocate(&ad, &own) == 0 && own) {
            const native_handle_t *oh = s_get_handle(own);
            AHardwareBuffer *again = NULL;
            const int brc = oh ? s_create(&ad, oh, 2, &again) : -1;
            ahb_bridge_note("  SELF-ALLOCATED %ux%u: rc=%d (numFds=%d numInts=%d)", width, height, brc,
                            oh ? oh->numFds : -1, oh ? oh->numInts : -1);
            /* Deliberately NOT released: `again` and `own` wrap the same allocation, and
             * releasing both is the double free that deadlocked this thread inside the
             * allocator (the same shape the standalone probe died on).  One small buffer per
             * run is a fair price for the measurement. */
            (void)again;
            (void)own;
        }
    }
    snprintf(why, why_size,
             "createFromHandle refused this handle: %ux%u format=0x%x stride=%u numFds=%d numInts=%d, last rc=%d",
             width, height, format, stride, handle->numFds, handle->numInts, last_rc);
    return false;
}

static int bridge_socket(void)
{
    if (s_sock != -2)
        return s_sock;
    s_sock = -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return s_sock;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (AHB_BRIDGE_SOCKET[0] == '@') {
        /* The abstract namespace: sun_path[0] is NUL and the name follows it. */
        a.sun_path[0] = 0;
        strncpy(a.sun_path + 1, AHB_BRIDGE_SOCKET + 1, sizeof(a.sun_path) - 2);
    } else {
        strncpy(a.sun_path, AHB_BRIDGE_SOCKET, sizeof(a.sun_path) - 1);
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        /* -2, NOT -1: a server that is not up yet is the ordinary case here (it waits for this
         * side for as long as its own accept timeout), so the next offer has to try again.
         * Parking on -1 made the connection unrepeatable and the window blank. */
        s_sock = -2;
        return -1;
    }
    LOGI("connected to the image bridge at %s", AHB_BRIDGE_SOCKET);
    s_sock = fd;
    return s_sock;
}

static bool write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return false;
        }
        b += w;
        n -= (size_t)w;
    }
    return true;
}

// Fills exactly n bytes or reports failure, so a short read from a receiver that died
// halfway through an answer is never mistaken for a complete one.
static bool read_all(int fd, void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return false;
        }
        b += r;
        n -= (size_t)r;
    }
    return true;
}

static void hex4(const uint8_t v[4], char *out, size_t n)
{
    snprintf(out, n, "%02x%02x%02x%02x", v[0], v[1], v[2], v[3]);
}

// The app own logcat lines are not visible on this device (measured: the framework code
// running in the same process logs fine while this app own calls never appear), so every step
// of the bridge also lands in a file this build owns and root can read.
void ahb_bridge_note(const char *fmt, ...)
{
    FILE *f = fopen("/data/data/com.anland.consumer.goldtest/files/ahb_bridge.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc((int)10, f);
    fclose(f);
}

static bool offer_locked(void *ahb, uint32_t index, uint32_t width, uint32_t height, uint32_t stride_bytes,
                         uint32_t format, uint64_t usage, const char *note)
{
    if (!ahb)
        return false;
    const int fd = bridge_socket();
    ahb_bridge_note("offer buffer %u: bridge fd=%d", index, fd);
    LOGE("bridge: offer buffer %u, fd=%d", index, fd);
    if (fd < 0)
        return false;

    // The return value is recorded, NOT obeyed.  Measured on the device: this call has been
    // seen to report a failure while the descriptor still crossed, and bailing on it is what
    // produced a receiver holding a handle with no description to go with it -- the failure
    // then looked like a protocol error on the far side.  The sideband is the part this side
    // controls, so it goes out either way and the receiver decides what it got.
    const int sendRc = AHardwareBuffer_sendHandleToUnixSocket((const AHardwareBuffer *)ahb, fd);
    ahb_bridge_note("  sendHandle rc=%d (ahb=%p)", sendRc, ahb);
    if (sendRc != 0)
        LOGW("AHardwareBuffer_sendHandleToUnixSocket -> rc=%d errno=%d(%s); sending the description anyway",
             sendRc, errno, strerror(errno));
    struct ahb_offer offer;
    memset(&offer, 0, sizeof(offer));
    offer.magic = AHB_BRIDGE_MAGIC;
    offer.version = 1;
    offer.index = index;
    offer.width = width;
    offer.height = height;
    offer.stride = stride_bytes;
    offer.format = format;
    offer.usage_lo = (uint32_t)(usage & 0xFFFFFFFFu);
    offer.usage_hi = (uint32_t)(usage >> 32);
    if (note)
        strncpy(offer.note, note, sizeof(offer.note) - 1);
    ahb_bridge_note("  sideband write about to go out: %zu bytes", sizeof(offer));
    if (!write_all(fd, &offer, sizeof(offer))) {
        LOGW("the image bridge closed while offering buffer %u; dropping it until the next window", index);
        close(fd);
        s_sock = -2;  // a bridge that went away may come back
        return false;
    }
    LOGI("offered buffer %u to the render server (%ux%u stride=%u format=0x%x usage=0x%llx)", index, width,
         height, stride_bytes, format, (unsigned long long)usage);

    // The answer, and then this side's own look at the same pixels.  A receiver that never
    // answers leaves the buffer offered and the window working: an offer is not a request
    // and nothing here is fatal.
    struct timeval tv;
    tv.tv_sec = AHB_BRIDGE_ANSWER_MS / 1000;
    tv.tv_usec = (AHB_BRIDGE_ANSWER_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct ahb_ack ack;
    memset(&ack, 0, sizeof(ack));
    if (!read_all(fd, &ack, sizeof(ack)) || ack.magic != AHB_BRIDGE_MAGIC) {
        LOGI("buffer %u: nothing answered on the bridge, so nothing is drawing into it", index);
        return false;
        return true;
    }

    struct ahb_seen seen;
    memset(&seen, 0, sizeof(seen));
    seen.magic = AHB_BRIDGE_MAGIC;
    seen.version = 1;
    seen.index = index;
    snprintf(seen.note, sizeof(seen.note), "host readback after the receiver's ack");

    // The only view of this buffer that belongs to this side.  AHardwareBuffer_Desc::stride
    // is in PIXELS and every offset here is in bytes.
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe((const AHardwareBuffer *)ahb, &d);
    void *p = NULL;
    if (d.width && d.height &&
        AHardwareBuffer_lock((const AHardwareBuffer *)ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &p) == 0 &&
        p) {
        const size_t byteStride = (size_t)d.stride * 4u;
        const uint8_t *base = p;
        memcpy(seen.observed, base, 4);
        memcpy(seen.observed_corner,
               base + (size_t)(d.height - 1) * byteStride + (size_t)(d.width - 1) * 4u, 4);
        seen.locked = 1;
        AHardwareBuffer_unlock((const AHardwareBuffer *)ahb, NULL);
    }
    (void)write_all(fd, &seen, sizeof(seen));

    char ackS[16], firstS[16], cornerS[16];
    hex4(ack.color, ackS, sizeof(ackS));
    hex4(seen.observed, firstS, sizeof(firstS));
    hex4(seen.observed_corner, cornerS, sizeof(cornerS));
    const bool drew = ack.imported && ack.drawn;
    LOGI("buffer %u: server imported=%u drawn=%u fence=%u colour=%s | host saw %s / %s%s", index, ack.imported,
         ack.drawn, ack.fence_ok, ackS, firstS, cornerS, seen.locked ? "" : " (the lock was refused)");
    return drew;
}

// ONE EXCHANGE AT A TIME ON THE ONE SOCKET.  An offer is handle + description out, answer in,
// readback out: three steps the receiver reads in exactly that order.  Two threads offering at once
// interleave them - measured when a surface change restarted the session while the frames thread
// was mid-exchange: the receiver read one thread's description after the other's handle and the
// waiting thread blocked in its read until the 30 s answer deadline, with the window frozen behind
// it.  The whole exchange is therefore serialised here.
static pthread_mutex_t s_offer_lock = PTHREAD_MUTEX_INITIALIZER;

bool ahb_bridge_offer(void *ahb, uint32_t index, uint32_t width, uint32_t height, uint32_t stride_bytes,
                      uint32_t format, uint64_t usage, const char *note)
{
    pthread_mutex_lock(&s_offer_lock);
    const bool drew = offer_locked(ahb, index, width, height, stride_bytes, format, usage, note);
    pthread_mutex_unlock(&s_offer_lock);
    return drew;
}

void ahb_bridge_release(void *ahb)
{
    if (ahb)
        AHardwareBuffer_release((AHardwareBuffer *)ahb);
}
