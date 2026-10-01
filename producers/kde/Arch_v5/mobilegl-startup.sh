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
ANLAND_SOCKET="${ANLAND_SOCKET:-/run/display.sock}"
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
export MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=shm
export KWIN_DISABLE_VULKAN=1 KWIN_NO_TIMER_QUERY=1 KWIN_PERSISTENT_VBO=0
export KWIN_DISABLE_UDMABUF_IMPORT=1
unset MESA_LOADER_DRIVER_OVERRIDE GALLIUM_DRIVER FD_FORCE_KGSL ANLAND_DRM_DEVICE
unset XWAYLAND_GBM_DEVICE ANLAND_SKIP_IMPLICIT_SYNC_WAIT

case "$MODE" in
    compositor)
        [ -x "$KWIN_BIN" ] || { echo "KWin binary missing: $KWIN_BIN" >&2; exit 1; }
        [ -r "$MOBILEGL_VENDOR_JSON" ] || { echo "MobileGL EGL vendor missing: $MOBILEGL_VENDOR_JSON" >&2; exit 1; }
        export ANLAND_MOBILEGL=1 ANLAND_SOCKET
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
ExecStart=
ExecStart="$HELPER" compositor
Environment="KWIN_BIN=$KWIN_BIN"
Environment="KWIN_LIB_DIR=$KWIN_LIB_DIR"
Environment="MOBILEGL_VENDOR_JSON=$MOBILEGL_VENDOR_JSON"
Environment="ANLAND_SOCKET=$ANLAND_SOCKET"
Environment="MOBILEGL_ENDPOINT=$MOBILEGL_ENDPOINT"
EOF
        systemctl --user daemon-reload
        dbus-update-activation-environment --systemd __EGL_VENDOR_LIBRARY_FILENAMES MOBILEGL_TRANSPORT MOBILEGL_IPC_DATA MOBILEGL_IPC_CONTROL MOBILEGL_IPC_SURFACE QT_QPA_PLATFORM
        exec startplasma-wayland "$@"
        ;;
    *)
        echo "Usage: $0 [compositor|plasma] [arguments...]" >&2
        exit 2
        ;;
esac
