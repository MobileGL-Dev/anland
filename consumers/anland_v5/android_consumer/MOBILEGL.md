# Embedded MobileGL renderer

The unified Anland 5.x APK contains the Android MobileGL split runtime and owns
it in one process. It does not launch or depend on a MobileGL plugin
application, and it does not fork a renderer process per client.

* The private, same-UID `:mobilegl` service (`MobileGLWorker`) is the only
  renderer host. It loads `libMobileGL.so` and serves the single embedded
  endpoint `@anland-mobilegl`.
* Session ownership lives inside the MobileGL library: the one embedded server
  schedules every session that dials the endpoint, in arrival order. Anland
  neither forks workers nor passes a session budget, so a new client never
  changes Anland's process layout.
* Anland's UI process never loads MobileGL's GL exports. The renderer's
  process-global GL state stays in `:mobilegl`, out of the UI and out of any
  other consumer.

Anland passes its Surface to the embedded server over private Binder IPC.
MobileGL is the Surface's sole producer, using the Android vendor driver and
buffer queue directly. The existing Anland display daemon remains the
display/input/audio broker. `DATA_MSG_MOBILEGL_SURFACE` announces the output
size without exporting image fds. Its one pacing slot is an acknowledgement
channel, not a dma-buf. The matching KWin backend uses the embedded server's
default framebuffer and swaps directly into the Anland Surface. The original
CPU dequeue/queue path is disabled in this mode so it cannot compete with EGL
for the producer connection.

The native display consumer starts only after the worker reports that it has
attached the Surface. Surface wrappers received from Binder are retained per
window owner and Surface generation, so a resize updates the existing native
window lease instead of detaching a live GL session. A replaced or destroyed
Surface is detached before its Java/native references are released. Native
geometry requests are posted to Anland's `SurfaceHolder` as a one-way Binder
call; `surfaceChanged` reports the resulting buffer extent to the worker before
it stops the native transport.

## Build

Use JDK 17, Android SDK platform 36, CMake 3.22.1 and an installed Android NDK.
Build MobileGL at the matching source revision with the Android arm64 split and
in-process-server support, including the neutral `EmbeddedServer.h` C ABI
(`mobilegl_server_serve_inprocess`). Point `MOBILEGL_DIST` at a directory
containing `libMobileGL.so` and any shared runtime dependencies. The library may
be directly in that directory or under `arm64-v8a/`.

From this directory:

```sh
MOBILEGL_DIST=/path/to/mobilegl/android-dist \
./gradlew :app:assemblePlainDebug :app:testPlainDebugUnitTest \
  -PanlandNdkVersion=27.2.12479018 \
  -PmobileglApplicationId=com.anland.consumer.mobilegl
```

`-PmobileglDist=/path/to/dist` is equivalent to the environment variable. The
application ID property is optional; omit it for `com.anland.consumer`. A separate
ID lets an experiment coexist with an installed consumer signed by another key.
The APK is written to `app/build/outputs/apk/plain/debug/app-plain-debug.apk`.
An unbundled build clears generated MobileGL libraries and uses the legacy
renderer. A bundled build uses MobileGL by default; launching with the Boolean
Intent extra `mobilegl=false` selects the original Mesa/dma-buf renderer.

For Linux clients the transport configuration names the one endpoint:

```sh
MOBILEGL_TRANSPORT=spawn
MOBILEGL_IPC_DATA=shm
MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl
# KWin, which owns the Anland Surface through the embedded server:
MOBILEGL_IPC_SURFACE=server
# Every other GL process renders offscreen and presents through its compositor:
MOBILEGL_IPC_SURFACE=offscreen
```

The library and wire fingerprint must match the APK's runtime. Client WSI must
hand application frames to the Linux compositor; an offscreen session alone does
not display a Wayland or X11 window. The embedded server's log is in Anland's
private files directory as `mobilegl-server.log`. A renderer or Surface-attach
failure is also returned through Binder and shown in the Anland window.

The GPU-free protocol check is `sh tests/run_mobilegl_surface_exchange.sh` from
the repository root. It runs a real daemon, exchanges Surface metadata without
image fds, completes one frame acknowledgement, and delivers an input event.
