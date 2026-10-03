# mobilegl-desktop.sh up|down|status - the MobileGL desktop's bring-up and tear-down, run as root
# (su) by the Anland app's MobileGL service (MobileGLDesktop.java builds the command line).
#
#   up      display daemon (started if absent) -> the server's backend published for the container
#           -> the container (started if stopped) -> its Plasma session (desktop-session.service,
#           which itself waits until the MobileGL server and the daemon are reachable)
#   down    end the Plasma session, stop the container. The display daemon stays: it is tiny.
#   status  one line per piece
#
# Idempotent: a piece that already runs is left alone. Called with these variables:
#   SOCK       display daemon socket; its directory is the container's /run/anland-mobilegl
#   BACKEND    the backend the app's MobileGL server runs: DirectGLES or DirectVulkan
#   CONTAINER  Droidspaces container name
#   DAEMON     display_daemon binary
#   DS         droidspaces binary
SOCK=${SOCK:-/data/local/tmp/anland-mobilegl/display.sock}
CONTAINER=${CONTAINER:-arch-kde-mgl}
DAEMON=${DAEMON:-/data/adb/modules/anland-daemon/display_daemon}
DS=${DS:-/data/local/Droidspaces/bin/droidspaces}
SHARE=${SOCK%/*}
CONF=${CONF:-${DS%/bin/*}/Containers/$CONTAINER/container.config}
LOG=$SHARE/desktop.log

# `su` raised this shell in place, so it still sits in the app's process cgroup and process group:
# Android ends both when it ends the app's process, and with them everything started below. Leave
# them first, so the daemon and the container outlive the window that asked for them.
echo $$ > /sys/fs/cgroup/cgroup.procs 2>/dev/null

mkdir -p "$SHARE"
chmod 777 "$SHARE"

log() {
    echo "$*"
    echo "$(date '+%F %T') $*" >> "$LOG"
}

daemon_pids() {
    for p in $(pidof display_daemon); do
        tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -qF " $SOCK " && echo $p
    done
}

container_pid() {
    p=$("$DS" --name="$CONTAINER" pid 2>/dev/null | tr -dc '0-9')
    [ -n "$p" ] && [ "$p" -gt 0 ] && echo "$p"
}

in_container() {
    "$DS" --name="$CONTAINER" run "$@"
}

up() {
    if [ -z "$(daemon_pids)" ] || [ ! -S "$SOCK" ]; then
        for p in $(daemon_pids); do kill $p; done
        rm -f "$SOCK"
        setsid "$DAEMON" "$SOCK" > "$SHARE/display-daemon.log" 2>&1 < /dev/null &
        i=0
        while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
        # The container's desktop user connects to it.
        chmod 666 "$SOCK"
        log up "display daemon started on $SOCK (pid $(daemon_pids))"
    fi

    # The container's session copies this into /etc/mobilegl/backend before it starts, so every
    # client asks the server for the backend it runs.
    case "$BACKEND" in
        DirectGLES|DirectVulkan)
            printf '%s\n' "$BACKEND" > "$SHARE/backend.new" && chmod 644 "$SHARE/backend.new" &&
                mv -f "$SHARE/backend.new" "$SHARE/backend" ;;
        *) log up "unknown backend '$BACKEND'; $SHARE/backend left as is" ;;
    esac

    if [ -z "$(container_pid)" ]; then
        log up "starting container $CONTAINER"
        "$DS" -C "$CONF" start > "$SHARE/container-start.log" 2>&1 < /dev/null
        [ -n "$(container_pid)" ] || { log up "container $CONTAINER did not start (see $SHARE/container-start.log)"; return 1; }
    fi

    # desktop-session.service is enabled, so a container that just booted starts it by itself; this
    # covers a session that was stopped or gave up. --no-block: the unit waits for the server.
    i=0
    until in_container systemctl start --no-block desktop-session.service > /dev/null 2>&1; do
        i=$((i + 1))
        [ $i -ge 60 ] && { log up "systemd in $CONTAINER did not take the session start"; return 1; }
        sleep 0.5
    done
    if [ "$(in_container systemctl is-failed desktop-session.service 2>/dev/null)" = failed ]; then
        in_container systemctl reset-failed desktop-session.service
        in_container systemctl start --no-block desktop-session.service
    fi
    log up "desktop up: daemon $(daemon_pids), container $(container_pid), backend $BACKEND"
}

down() {
    if [ -n "$(container_pid)" ]; then
        in_container systemctl stop desktop-session.service > /dev/null 2>&1
        "$DS" --name="$CONTAINER" stop > /dev/null 2>&1 < /dev/null
    fi
    c=$(container_pid)
    log down "desktop down (container $CONTAINER: ${c:-stopped})"
}

status() {
    echo "daemon: $(daemon_pids)"
    echo "container: $(container_pid)"
    [ -n "$(container_pid)" ] && echo "session: $(in_container systemctl is-active desktop-session.service 2>/dev/null)"
    echo "backend: $(cat "$SHARE/backend" 2>/dev/null)"
}

case "$1" in
    up) up ;;
    down) down ;;
    status) status ;;
    *) echo "usage: mobilegl-desktop.sh up|down|status" >&2; exit 2 ;;
esac
