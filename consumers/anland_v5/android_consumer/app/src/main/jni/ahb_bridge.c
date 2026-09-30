// ahb_bridge.c -- see the header for why any of this exists.

#include "ahb_bridge.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_TAG "AnlandAhbBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// AHardwareBuffer_createFromHandle is a SystemApi: the NDK does not declare it, and the
// platform does not care who calls it.  libnativewindow.so is where it is exported.
typedef int (*create_from_handle_fn)(const AHardwareBuffer_Desc *, const void *, int, AHardwareBuffer **);

// The handle type that takes a gralloc handle.  Both values are tried anyway, because the
// enum is not in the NDK and the two are one integer apart in meaning: on the measured
// device type 2 accepts a real gralloc handle and type 1 refuses it, and reading that off
// the platform is cheaper than trusting a transcribed constant.
enum { AHB_HANDLE_TYPE_TRY_FIRST = 2, AHB_HANDLE_TYPE_TRY_SECOND = 1 };

static create_from_handle_fn s_create;
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
    LOGI("AHardwareBuffer_createFromHandle = %p", (void *)s_create);
}

uint64_t ahb_bridge_last_usage(void)
{
    return s_last_usage;
}

bool ahb_bridge_wrap(const ANativeWindowBuffer *anb, void **out_ahb, char *why, unsigned why_size)
{
    *out_ahb = NULL;
    load_api();
    if (!s_create) {
        snprintf(why, why_size, "AHardwareBuffer_createFromHandle is not loadable in this process");
        return false;
    }
    if (!anb || !anb->handle) {
        snprintf(why, why_size, "the dequeued buffer carries no native handle");
        return false;
    }

    AHardwareBuffer_Desc d;
    memset(&d, 0, sizeof(d));
    d.width = (uint32_t)anb->width;
    d.height = (uint32_t)anb->height;
    d.layers = 1;
    // HAL_PIXEL_FORMAT_* and AHARDWAREBUFFER_FORMAT_* agree on the packed 32-bit formats,
    // which is the only kind a SurfaceView queue hands out here.
    d.format = (uint32_t)anb->format;
    d.stride = (uint32_t)anb->stride;

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
    int last_rc = 0;

    for (unsigned t = 0; t < sizeof(types) / sizeof(types[0]); ++t) {
        for (unsigned u = 0; u < sizeof(usages) / sizeof(usages[0]); ++u) {
            d.usage = usages[u];
            AHardwareBuffer *ahb = NULL;
            const int rc = s_create(&d, anb->handle, types[t], &ahb);
            last_rc = rc;
            if (rc == 0 && ahb) {
                s_last_usage = usages[u];
                *out_ahb = ahb;
                return true;
            }
        }
    }
    snprintf(why, why_size,
             "createFromHandle refused this handle: %ux%u format=0x%x stride=%u numFds=%d numInts=%d, last rc=%d",
             d.width, d.height, d.format, d.stride, anb->handle->numFds, anb->handle->numInts, last_rc);
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
    strncpy(a.sun_path, AHB_BRIDGE_SOCKET, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return s_sock;
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

bool ahb_bridge_offer(void *ahb, uint32_t index, uint32_t width, uint32_t height, uint32_t stride_bytes,
                      uint32_t format, uint64_t usage, const char *note)
{
    if (!ahb)
        return false;
    const int fd = bridge_socket();
    if (fd < 0)
        return false;

    if (AHardwareBuffer_sendHandleToUnixSocket((const AHardwareBuffer *)ahb, fd) != 0) {
        LOGW("AHardwareBuffer_sendHandleToUnixSocket -> errno=%d(%s)", errno, strerror(errno));
        return false;
    }
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
    LOGI("buffer %u: server imported=%u drawn=%u fence=%u colour=%s | host saw %s / %s%s", index, ack.imported,
         ack.drawn, ack.fence_ok, ackS, firstS, cornerS, seen.locked ? "" : " (the lock was refused)");
    return true;
}

void ahb_bridge_release(void *ahb)
{
    if (ahb)
        AHardwareBuffer_release((AHardwareBuffer *)ahb);
}
