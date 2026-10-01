# Anland 5.x with MobileGL

The Arch backend has an opt-in `ANLAND_MOBILEGL=1` path for KWin 6.7.4.
KWin keeps the Anland display connection, input events, clipboard, output size,
and frame pacing. The Android consumer app hosts the MobileGL shared-memory
split server inside one private Anland process and hands that server's Surface
to the compositor's render session. KWin renders into the EGL window's default
framebuffer and swaps that Surface.

The consumer advertises `DATA_MSG_MOBILEGL_SURFACE` with
`ANLAND_FORMAT_MOBILEGL_SURFACE` (`0x4d474c53`). This describes one output and
has no dma-buf fd. KWin validates the marker instead of importing a gralloc
buffer into Mesa. The normal Mesa/kgsl backend is selected when
`ANLAND_MOBILEGL` is absent.

## Build and install

Use the `Arch_v5/kwin.patch` together with `anland_backend_Arch_v5` and the
current `common/` and `libdisplay_producer/` files. The patch lets EGL contexts
use the compositor window from their first `makeCurrent`, makes DRM optional,
and keeps Xwayland on its shared-memory path. The backend overlay requires
these corresponding KWin changes.

Build with the existing `build.sh`/`PKGBUILD`, or configure KWin 6.7.4 directly:

```sh
cmake -S kwin-6.7.4 -B kwin-6.7.4/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=OFF
cmake --build kwin-6.7.4/build --target kwin_wayland --parallel 6
```

For an isolated container installation, place the rebuilt executable and
`libkwin.so*` in `/opt/mobilegl/kwin/bin` and `/opt/mobilegl/kwin/lib`.
The container client is installed separately under `/opt/mobilegl` with its
GLVND vendor JSON at
`/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json`.
Install `mobilegl-startup.sh` as `/opt/mobilegl/bin/mobilegl-startup.sh`.

## Start

Enable MobileGL in the Anland Android app and start the consumer window.
Run the helper as the container's desktop user:

```sh
/opt/mobilegl/bin/mobilegl-startup.sh compositor
```

The helper connects everything to the one embedded endpoint
`MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl` with `MOBILEGL_TRANSPORT=spawn` and
`MOBILEGL_IPC_DATA=shm`. The abstract UNIX endpoint is reachable from the
Droidspaces container without a new pathname bind mount, and the embedded
server in the Anland APK schedules every session that dials it.

For the ordinary Plasma session startup:

```sh
/opt/mobilegl/bin/mobilegl-startup.sh plasma
```

This installs a user override for `plasma-kwin_wayland.service` and launches
`startplasma-wayland`. KWin keeps `MOBILEGL_IPC_SURFACE=server` because the
Android Surface is dedicated to it. Desktop applications use the same endpoint
with `MOBILEGL_IPC_SURFACE=offscreen`: their GL contexts stay offscreen and each
client presents its Wayland window through the compositor in shared memory.
Xwayland is launched with `-shm` and without KWin's MobileGL loader settings.

Plasma mode needs the normal systemd user session. In the supplied Droidspaces
image, `desktop-session.service` provides this with `User=swung0x48` and
`PAMName=login`; its `ExecStart` can be overridden to invoke the helper's
`plasma` mode. A bare `droidspaces --user=... run` command does not create a
logind session or start the user's D-Bus manager.

`KWIN_BIN`, `KWIN_LIB_DIR`, `MOBILEGL_VENDOR_JSON`, `ANLAND_SOCKET`, and
`MOBILEGL_ENDPOINT` (the `unix:@...` endpoint name, used by both modes) can
override the installation defaults. Remove
`~/.config/systemd/user/plasma-kwin_wayland.service.d/mobilegl.conf` and reload
the user manager to return the Plasma service to its packaged startup command.

## Validate

Check KWin's `supportInformation` over the session D-Bus for the Anland output
backend and the MobileGL renderer. Then start Konsole, run `eglinfo -B`, run
`glxinfo -B` through the MobileGL `libGL.so` alias, and start the complete
Plasma session. For GLX clients, scope
`LD_LIBRARY_PATH=/opt/mobilegl/lib` to the client command; KWin uses the GLVND
EGL vendor JSON and a separate directory containing only the rebuilt KWin
library. Renderer identification and rendered output both need checking.
