#!/bin/bash
# Run the MobileGL session, or install the KWin user service override and start
# a complete Plasma session. Every client dials the same embedded endpoint.
# Run as the container's desktop user.
set -euo pipefail

MODE="${1:-compositor}"
if [ "$#" -gt 0 ]; then shift; fi
KWIN_BIN="${KWIN_BIN:-/opt/mobilegl/kwin/bin/kwin_wayland}"
KWIN_LIB_DIR="${KWIN_LIB_DIR:-/opt/mobilegl/kwin/lib}"
MOBILEGL_VENDOR_JSON="${MOBILEGL_VENDOR_JSON:-/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json}"
# This launcher serves the MobileGL experiment only. /etc/environment leaks a
# Mesa-baseline ANLAND_SOCKET into the login session through pam_env, which
# would silently override the systemd drop-in; the experiment's daemon socket
# is therefore the default here, not /run/display.sock.
ANLAND_SOCKET="${ANLAND_MOBILEGL_SOCKET:-/run/anland-mobilegl/display.sock}"
# One endpoint for every client. The embedded server in the Anland APK owns the
# session schedule; the launcher only points clients at it.
MOBILEGL_ENDPOINT="${MOBILEGL_ENDPOINT:-unix:@anland-mobilegl}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ ! -d "$XDG_RUNTIME_DIR" ]; then
    export XDG_RUNTIME_DIR="$HOME/.local/run/anland-$(id -u)"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 0700 "$XDG_RUNTIME_DIR"
fi
if [ -S "$XDG_RUNTIME_DIR/bus" ]; then
    export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"
fi
export __EGL_VENDOR_LIBRARY_FILENAMES="$MOBILEGL_VENDOR_JSON"
# X11 clients (through Xwayland) take GLX from glvnd's libGLX, which would load the vendor
# Xwayland names (Mesa, which wants a GPU device the container's clients cannot use).
# libGLX_mobilegl.so.0 is installed by anland-build-client.sh.
export __GLX_VENDOR_LIBRARY_NAME=mobilegl
export MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=shm
# The backend every client asks the embedded server for: DirectGLES or DirectVulkan. The
# server serves one backend per process and refuses a client that asks for the other, so it
# must match the Anland app's choice (`--es mobilegl_backend`, or on a debuggable build
# `setprop debug.mobilegl.backend`). Kept in /etc/mobilegl/backend; DirectGLES when absent.
MOBILEGL_BACKEND_FILE="${MOBILEGL_BACKEND_FILE:-/etc/mobilegl/backend}"
if [ -z "${MOBILEGL_BACKEND_TYPE:-}" ] && [ -r "$MOBILEGL_BACKEND_FILE" ]; then
    MOBILEGL_BACKEND_TYPE="$(tr -d '[:space:]' < "$MOBILEGL_BACKEND_FILE")"
fi
export MOBILEGL_BACKEND_TYPE="${MOBILEGL_BACKEND_TYPE:-DirectGLES}"
export KWIN_DISABLE_VULKAN=1 KWIN_NO_TIMER_QUERY=1 KWIN_PERSISTENT_VBO=0
export KWIN_DISABLE_UDMABUF_IMPORT=1
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
        # ANGLE's GLES-on-EGL backend over the system EGL, which is MobileGL's vendor. Chrome's
        # Wayland GPU process only presents through dma-bufs from a GBM device, which neither
        # MobileGL nor this compositor takes; with the GPU in the browser process Chrome draws
        # into wl_egl_windows on its own Wayland connection instead, which MobileGL presents. It
        # is pointed at no render node so it does not set up GBM scanout buffers at all. On
        # that path Chrome never sends its fractional-scale viewport, so on this scale-2 output
        # the window would show at twice its size; with integer scaling it sends
        # wl_surface.set_buffer_scale, which the frames MobileGL attaches then carry.
        export MOBILEGL_IPC_CONTROL="$MOBILEGL_ENDPOINT" MOBILEGL_IPC_SURFACE=offscreen
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
        # aliases; EGL selection still goes through the GLVND vendor JSON.
        export LD_LIBRARY_PATH="$KWIN_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        export MOBILEGL_IPC_CONTROL="$MOBILEGL_ENDPOINT"
        export MOBILEGL_IPC_SURFACE=server
        exec "$KWIN_BIN" --anland --xwayland "$@"
        ;;
    plasma)
        # Same endpoint as the compositor: clients render offscreen and present
        # through KWin, which keeps the Android Surface itself.
        export MOBILEGL_IPC_CONTROL="$MOBILEGL_ENDPOINT"
        export MOBILEGL_IPC_SURFACE=offscreen QT_QPA_PLATFORM=wayland
        unset ANLAND_MOBILEGL
        HELPER="$(readlink -f "$0")"
        UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/plasma-kwin_wayland.service.d"
        mkdir -p "$UNIT_DIR"
        cat > "$UNIT_DIR/mobilegl.conf" <<EOF
[Service]
# The unit has BusName=org.kde.KWinWrapper: the compositor must come up
# through kwin_wayland_wrapper (it registers the name) or the service never
# leaves "starting" and systemd kills it 90s in, taking the session with it.
# Our kwin_wayland wins via PATH.
ExecStart=
ExecStart=/usr/bin/kwin_wayland_wrapper --xwayland
Environment="PATH=/opt/mobilegl/kwin/bin:/usr/local/bin:/usr/bin"
Environment="LD_LIBRARY_PATH=$KWIN_LIB_DIR"
Environment="__EGL_VENDOR_LIBRARY_FILENAMES=$MOBILEGL_VENDOR_JSON"
Environment="ANLAND_MOBILEGL=1"
Environment="ANLAND_SOCKET=$ANLAND_SOCKET"
Environment="MOBILEGL_TRANSPORT=spawn"
Environment="MOBILEGL_BACKEND_TYPE=$MOBILEGL_BACKEND_TYPE"
Environment="MOBILEGL_IPC_DATA=shm"
Environment="MOBILEGL_IPC_CONTROL=$MOBILEGL_ENDPOINT"
Environment="MOBILEGL_IPC_SURFACE=server"
Environment="MOBILEGL_LOG_FILE_PATH=/tmp/mobilegl-compositor.log"
Environment="KWIN_DISABLE_VULKAN=1"
Environment="KWIN_NO_TIMER_QUERY=1"
Environment="KWIN_PERSISTENT_VBO=0"
Environment="KWIN_DISABLE_UDMABUF_IMPORT=1"
Environment="QT_LOGGING_RULES=kwin_*.info=true"
EOF
        systemctl --user daemon-reload
        install_chrome_launcher
        dbus-update-activation-environment --systemd __EGL_VENDOR_LIBRARY_FILENAMES __GLX_VENDOR_LIBRARY_NAME MOBILEGL_TRANSPORT MOBILEGL_BACKEND_TYPE MOBILEGL_IPC_DATA MOBILEGL_IPC_CONTROL MOBILEGL_IPC_SURFACE QT_QPA_PLATFORM
        exec startplasma-wayland "$@"
        ;;
    *)
        echo "Usage: $0 [compositor|plasma|chrome] [arguments...]" >&2
        exit 2
        ;;
esac
