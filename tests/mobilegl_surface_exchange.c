/* End-to-end private-protocol check: real daemon, consumer and producer, no
 * GPU or Android dependency. Build twice with -DTEST_CONSUMER / otherwise. */
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#ifdef TEST_CONSUMER
#include "display_consumer.h"
#else
#include "display_producer.h"
#endif

int main(int argc, char **argv)
{
    assert(argc == 2);
    display_ctx *ctx = NULL;
    assert(connect_to_deamon(&ctx, argv[1]) == 0);
#ifdef TEST_CONSUMER
    assert(set_screen_info(ctx, 320, 240, ANLAND_FORMAT_MOBILEGL_SURFACE, 60000) == 0);
    assert(push_mobilegl_surface(ctx, 320, 240) == 0);
    for (int attempt = 0; attempt < 500 && consumer_is_fallback(ctx); ++attempt) {
        assert(select_dmabuf(ctx, 0) == 0);
        usleep(10000);
    }
    assert(!consumer_is_fallback(ctx));
    int fence = refresh_done(ctx);
    assert(fence == -1); // the Android worker's swapchain owns all output fences
    assert(!consumer_is_fallback(ctx));
    struct InputEvent event = {.type = INPUT_TYPE_KEY};
    event.key.keycode = 30;
    assert(push_input_event(ctx, &event) == 0);
    usleep(200000);
#else
    for (int attempt = 0; attempt < 500 && is_fallback(ctx); ++attempt) {
        try_exit_fallback(ctx);
        usleep(10000);
    }
    assert(!is_fallback(ctx));
    assert(get_buf_count(ctx) == 1);
    struct buf_info info;
    assert(get_dmabuf_info_at(ctx, 0, &info) == 0);
    assert(info.format == ANLAND_FORMAT_MOBILEGL_SURFACE);
    assert(info.width == 320 && info.height == 240 && info.stride == 0);
    assert(get_dmabuf_fd_at(ctx, 0) == -1);
    struct pollfd ready = {.fd = get_buffer_ready_fd(ctx), .events = POLLIN};
    assert(poll(&ready, 1, 2000) == 1);
    eventfd_t count;
    assert(eventfd_read(ready.fd, &count) == 0 && count > 0);
    assert(get_selected_idx(ctx) == 0);
    assert(trigger_refresh(ctx) == 0);
    struct InputEvent event;
    assert(poll_input_event(ctx, &event, 2000) == 1);
    assert(event.type == INPUT_TYPE_KEY && event.key.keycode == 30);
#endif
    disconnect(ctx);
    puts("MobileGL Surface exchange passed");
    return 0;
}
