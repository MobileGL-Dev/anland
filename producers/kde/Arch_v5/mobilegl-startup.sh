#!/bin/bash
# Run the MobileGL session, or install the KWin user service override and start
# a complete Plasma session. Run as the container's desktop user.
#
# MobileGL is the container's system-wide GL vendor, so nothing here selects it per process:
#   /usr/share/glvnd/egl_vendor.d/10_mobilegl.json  EGL (sorts ahead of the system's own vendor,
#                                                   which serves whenever MobileGL declines)
#   /etc/mobilegl/client.conf + /etc/mobilegl/backend  transport, endpoint and backend
#   /etc/environment.d/10-mobilegl.conf, /etc/profile.d/mobilegl.sh  GLX vendor name + GBM backend
# (installed by anland-build-client.sh or MobileGL's xdeploy.sh). What stays here is what only
# KWin needs: its own build, the server-owned surface, the anland backend and its GBM node.
set -euo pipefail

MODE="${1:-compositor}"
if [ "$#" -gt 0 ]; then shift; fi
KWIN_BIN="${KWIN_BIN:-/opt/mobilegl/kwin/bin/kwin_wayland}"
KWIN_LIB_DIR="${KWIN_LIB_DIR:-/opt/mobilegl/kwin/lib}"
MOBILEGL_VENDOR_JSON="${MOBILEGL_VENDOR_JSON:-/usr/share/glvnd/egl_vendor.d/10_mobilegl.json}"
# This launcher serves the MobileGL experiment only. /etc/environment leaks the
# baseline session's ANLAND_SOCKET into the login session through pam_env, which
# would silently override the systemd drop-in; the experiment's daemon socket
# is therefore the default here, not /run/display.sock.
ANLAND_SOCKET="${ANLAND_MOBILEGL_SOCKET:-/run/anland-mobilegl/display.sock}"
# The endpoint every client dials is client.conf's (unix:@anland-mobilegl). MOBILEGL_ENDPOINT
# overrides it for this session only.
if [ -n "${MOBILEGL_ENDPOINT:-}" ]; then
    export MOBILEGL_IPC_CONTROL="$MOBILEGL_ENDPOINT"
fi
# The endpoint every client dials: MOBILEGL_ENDPOINT, else client.conf's MOBILEGL_IPC_CONTROL.
server_endpoint() {
    local ep="${MOBILEGL_IPC_CONTROL:-}"
    if [ -z "$ep" ] && [ -r /etc/mobilegl/client.conf ]; then
        ep=$(sed -n 's/^[[:space:]]*MOBILEGL_IPC_CONTROL[[:space:]]*=[[:space:]]*//p' /etc/mobilegl/client.conf | tail -n 1)
    fi
    printf '%s' "${ep:-unix:@anland-mobilegl}"
}
# True once something listens there. The container shares the phone's network namespace, so the
# Anland app's abstract socket shows in /proc/net/unix (flags 00010000 = listening).
server_listening() {
    local ep
    ep=$(server_endpoint)
    case "$ep" in
        unix:*) ep=${ep#unix:} ;;
        *) return 0 ;;
    esac
    awk -v p="$ep" '$4 == "00010000" && $8 == p { found = 1 } END { exit !found }' /proc/net/unix
}

# The two steps desktop-session.service runs before the session itself. They need none of the
# desktop user's environment below: sync-backend runs as root.
case "$MODE" in
    wait)
        # The session starts with the container, but KWin may only come up once the Anland app's
        # MobileGL server listens (it does from the app's first window on) and the display
        # daemon's socket exists. Until then it waits here instead of failing or rendering
        # through another GL stack.
        said=0
        until server_listening && [ -S "$ANLAND_SOCKET" ]; do
            if [ "$said" = 0 ]; then
                echo "waiting for the MobileGL server ($(server_endpoint)) and the display daemon ($ANLAND_SOCKET): open the Anland app"
                said=1
            fi
            sleep 1
        done
        echo "MobileGL server and display daemon are up"
        exit 0
        ;;
    sync-backend)
        # The Anland app publishes the backend its server runs - its setting - next to the daemon
        # socket before it starts this container. Every client of the session asks the server for
        # /etc/mobilegl/backend, so the two must agree.
        published="${MOBILEGL_PUBLISHED_BACKEND:-$(dirname "$ANLAND_SOCKET")/backend}"
        backend=$(tr -d '[:space:]' < "$published" 2>/dev/null || true)
        case "$backend" in
            DirectGLES|DirectVulkan) ;;
            *) echo "no backend published at $published; /etc/mobilegl/backend stays $(cat /etc/mobilegl/backend 2>/dev/null)"; exit 0 ;;
        esac
        if [ "$(tr -d '[:space:]' < /etc/mobilegl/backend 2>/dev/null)" != "$backend" ]; then
            mkdir -p /etc/mobilegl
            printf '%s\n' "$backend" > /etc/mobilegl/backend.new
            mv -f /etc/mobilegl/backend.new /etc/mobilegl/backend
            echo "/etc/mobilegl/backend -> $backend (the Anland app's setting)"
        fi
        exit 0
        ;;
esac

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ ! -d "$XDG_RUNTIME_DIR" ]; then
    export XDG_RUNTIME_DIR="$HOME/.local/run/anland-$(id -u)"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 0700 "$XDG_RUNTIME_DIR"
fi
if [ -S "$XDG_RUNTIME_DIR/bus" ]; then
    export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"
fi
# The backend every client asks the embedded server for (DirectGLES or DirectVulkan) is
# /etc/mobilegl/backend, which the library reads itself; it must match the Anland app's choice
# (`--es mobilegl_backend`, or on a debuggable build `setprop debug.mobilegl.backend`).
export KWIN_DISABLE_VULKAN=1 KWIN_NO_TIMER_QUERY=1 KWIN_PERSISTENT_VBO=0
# KWin now has a DRM device (below), and with one it turns wl_shm buffers into udmabufs and
# imports them through EGL; MobileGL's EGL only imports images its server allocated, so that
# import would fail per buffer. Keep it off.
export KWIN_DISABLE_UDMABUF_IMPORT=1
# KWin's GBM device: libgbm loads /usr/lib/gbm/mobilegl_gbm.so (GBM_BACKEND=mobilegl, set
# system-wide), whose buffers are MobileGL server images. Its device node is only an identity -
# the backend never issues an ioctl on it - so the first render node this user can open is
# borrowed, else /dev/null. MobileGL's EGL device reports the same node (MOBILEGL_DEVICE_DRM_NODE,
# whose default is the first render node), so KWin's dma-buf feedback names one device throughout.
if [ -z "${MOBILEGL_GBM_NODE:-}" ]; then
    MOBILEGL_GBM_NODE=/dev/null
    for node in /dev/dri/renderD*; do
        if [ -r "$node" ] && [ -w "$node" ]; then
            MOBILEGL_GBM_NODE="$node"
            break
        fi
    done
fi
MOBILEGL_DEVICE_DRM_NODE="${MOBILEGL_DEVICE_DRM_NODE:-$MOBILEGL_GBM_NODE}"
unset MESA_LOADER_DRIVER_OVERRIDE GALLIUM_DRIVER FD_FORCE_KGSL ANLAND_DRM_DEVICE
unset XWAYLAND_GBM_DEVICE ANLAND_SKIP_IMPLICIT_SYNC_WAIT

CHROME_BIN="${CHROME_BIN:-/opt/google/chrome/google-chrome}"
HELPER_PATH="$(readlink -f "$0")"

# The image's /usr/local/bin/google-chrome runs Chrome on ANGLE's Vulkan backend over the
# container's own GPU driver. In this session the menu and the panel launch it through this
# helper instead (a per-user copy of its .desktop file), so Chrome draws through MobileGL.
install_chrome_launcher() {
    local system_desktop=/usr/share/applications/google-chrome.desktop
    [ -r "$system_desktop" ] && [ -x "$CHROME_BIN" ] || return 0
    local apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    mkdir -p "$apps"
    sed -E "s#^Exec=/usr/local/bin/google-chrome#Exec=$HELPER_PATH chrome#" \
        "$system_desktop" > "$apps/google-chrome.desktop"
}

case "$MODE" in
    chrome)
        # ANGLE's GLES-on-EGL backend over the system EGL, which is MobileGL's vendor. With the GPU
        # in the browser process Chrome draws into wl_egl_windows on its own Wayland connection,
        # which MobileGL presents as linux-dmabuf buffers backed by server images - no readback.
        # It is pointed at no render node so it keeps that path rather than allocating its own
        # GBM scanout buffers. On
        # that path Chrome never sends its fractional-scale viewport, so on this scale-2 output
        # the window would show at twice its size; with integer scaling it sends
        # wl_surface.set_buffer_scale, which the frames MobileGL attaches then carry.
        export MOBILEGL_IPC_SURFACE=offscreen
        exec "$CHROME_BIN" --ozone-platform=wayland --use-gl=angle --use-angle=gles \
            --in-process-gpu --ignore-gpu-blocklist \
            --render-node-override=/dev/dri/mobilegl-no-render-node \
            --disable-features=WaylandFractionalScaleV1 "$@"
        ;;
    compositor)
        [ -x "$KWIN_BIN" ] || { echo "KWin binary missing: $KWIN_BIN" >&2; exit 1; }
        [ -r "$MOBILEGL_VENDOR_JSON" ] || { echo "MobileGL EGL vendor missing: $MOBILEGL_VENDOR_JSON" >&2; exit 1; }
        export ANLAND_MOBILEGL=1 ANLAND_SOCKET
        # Client-side MobileGL log: the compositor's own stderr goes to the
        # journal, MobileGL's diagnostics go here.
        export MOBILEGL_LOG_FILE_PATH="${MOBILEGL_LOG_FILE_PATH:-/tmp/mobilegl-compositor.log}"
        # This directory contains the rebuilt KWin library, without EGL/GL
        # aliases; EGL selection goes through the system glvnd vendor JSON.
        export LD_LIBRARY_PATH="$KWIN_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        # The compositor alone owns the server's Android Surface.
        export MOBILEGL_IPC_SURFACE=server
        export GBM_BACKEND=mobilegl MOBILEGL_GBM_NODE MOBILEGL_DEVICE_DRM_NODE
        exec "$KWIN_BIN" --anland --xwayland "$@"
        ;;
    plasma)
        # Clients render offscreen (the library's default) and present through KWin, which keeps
        # the Android Surface itself.
        export QT_QPA_PLATFORM=wayland
        unset ANLAND_MOBILEGL
        HELPER="$(readlink -f "$0")"
        UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/plasma-kwin_wayland.service.d"
        mkdir -p "$UNIT_DIR"
        # A Plasma that once saw its GL device vanish (the server died) writes Qt Quick's software
        # scene graph into kdeglobals and keeps using it in every later session. This session
        # starts with a server, so drop that choice.
        kwriteconfig6 --file kdeglobals --group QtQuickRendererSettings --key SceneGraphBackend --delete 2>/dev/null || true
        # Never another GL stack in this session: with the reachability probe off, MobileGL serves
        # every process even while the server is briefly away (it waits for it) instead of
        # declining, so glvnd/libgbm never hand KWin, plasmashell or the apps to the container's
        # own driver or a software renderer.
        export MOBILEGL_IPC_PROBE_TIMEOUT_MS=0
        # Only KWin's own needs: transport, endpoint, backend and the EGL/GLX vendor are system-wide.
        ENDPOINT_LINE=""
        if [ -n "${MOBILEGL_IPC_CONTROL:-}" ]; then
            ENDPOINT_LINE="Environment=\"MOBILEGL_IPC_CONTROL=$MOBILEGL_IPC_CONTROL\""
        fi
        cat > "$UNIT_DIR/mobilegl.conf" <<UNIT
[Service]
# The unit has BusName=org.kde.KWinWrapper: the compositor must come up
# through kwin_wayland_wrapper (it registers the name) or the service never
# leaves "starting" and systemd kills it 90s in, taking the session with it.
# Our kwin_wayland wins via PATH.
ExecStart=
ExecStart=/usr/bin/kwin_wayland_wrapper --xwayland
Environment="PATH=/opt/mobilegl/kwin/bin:/usr/local/bin:/usr/bin"
Environment="LD_LIBRARY_PATH=$KWIN_LIB_DIR"
Environment="ANLAND_MOBILEGL=1"
Environment="ANLAND_SOCKET=$ANLAND_SOCKET"
Environment="MOBILEGL_IPC_SURFACE=server"
Environment="MOBILEGL_LOG_FILE_PATH=/tmp/mobilegl-compositor.log"
Environment="MOBILEGL_IPC_PROBE_TIMEOUT_MS=0"
$ENDPOINT_LINE
Environment="KWIN_DISABLE_VULKAN=1"
Environment="KWIN_NO_TIMER_QUERY=1"
Environment="KWIN_PERSISTENT_VBO=0"
Environment="KWIN_DISABLE_UDMABUF_IMPORT=1"
Environment="GBM_BACKEND=mobilegl"
Environment="MOBILEGL_GBM_NODE=$MOBILEGL_GBM_NODE"
Environment="MOBILEGL_DEVICE_DRM_NODE=$MOBILEGL_DEVICE_DRM_NODE"
Environment="QT_LOGGING_RULES=kwin_*.info=true"
UNIT
        systemctl --user daemon-reload
        install_chrome_launcher
        # Pushed to every activated client only when it differs from what the library assumes.
        ACTIVATION_VARS="QT_QPA_PLATFORM MOBILEGL_IPC_PROBE_TIMEOUT_MS"
        if [ "$MOBILEGL_DEVICE_DRM_NODE" != /dev/dri/renderD128 ]; then
            export MOBILEGL_DEVICE_DRM_NODE
            ACTIVATION_VARS="$ACTIVATION_VARS MOBILEGL_DEVICE_DRM_NODE"
        fi
        if [ -n "${MOBILEGL_IPC_CONTROL:-}" ]; then
            ACTIVATION_VARS="$ACTIVATION_VARS MOBILEGL_IPC_CONTROL"
        fi
        dbus-update-activation-environment --systemd $ACTIVATION_VARS
        exec startplasma-wayland "$@"
        ;;
    *)
        echo "Usage: $0 [compositor|plasma|chrome|wait|sync-backend] [arguments...]" >&2
        exit 2
        ;;
esac
