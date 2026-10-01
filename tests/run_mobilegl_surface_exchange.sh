#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
daemon_pid=
consumer_pid=
cleanup() {
    [ -z "$consumer_pid" ] || kill "$consumer_pid" 2>/dev/null || true
    [ -z "$daemon_pid" ] || kill "$daemon_pid" 2>/dev/null || true
    rm -rf "$build"
}
trap cleanup EXIT HUP INT TERM
# Git on Windows may materialize symlinks as their target text. Stage the three
# authoritative producer includes explicitly so this check also runs via WSL.
mkdir -p "$build/source/common" "$build/source/libdisplay_consumer" \
    "$build/source/libdisplay_producer"
cp "$root/common/protocol.h" "$root/common/socket_utils.h" "$root/common/socket_utils.c" "$build/source/common/"
cp "$root/libdisplay_consumer/display_consumer.c" "$root/libdisplay_consumer/display_consumer.h" "$build/source/libdisplay_consumer/"
cp "$root/libdisplay_producer/display_producer.c" "$root/libdisplay_producer/display_producer.h" "$build/source/libdisplay_producer/"
cp "$root/common/protocol.h" "$root/common/socket_utils.h" "$build/source/libdisplay_producer/"
cc -DTEST_CONSUMER -I"$build/source/libdisplay_consumer" "$root/tests/mobilegl_surface_exchange.c" \
    "$build/source/libdisplay_consumer/display_consumer.c" "$build/source/common/socket_utils.c" \
    -pthread -o "$build/consumer"
cc -I"$build/source/libdisplay_producer" "$root/tests/mobilegl_surface_exchange.c" \
    "$build/source/libdisplay_producer/display_producer.c" "$build/source/common/socket_utils.c" \
    -pthread -o "$build/producer"
cc "$root/daemon/daemon.c" "$root/libdisplay_daemon/display_daemon.c" \
    "$root/common/socket_utils.c" -pthread -o "$build/daemon"
"$build/daemon" "$build/display.sock" >"$build/daemon.log" 2>&1 &
daemon_pid=$!
for attempt in 1 2 3 4 5; do
    [ ! -S "$build/display.sock" ] || break
    sleep 0.1
done
"$build/consumer" "$build/display.sock" &
consumer_pid=$!
sleep 0.1
"$build/producer" "$build/display.sock"
wait "$consumer_pid"
consumer_pid=
