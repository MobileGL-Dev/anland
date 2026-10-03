#!/bin/bash
set -euo pipefail
src=/root/mobilegl-unified-src
out=/root/mobilegl-unified-build
stamp=$(cat "$src/.anland-build-stamp" 2>/dev/null || echo unknown)
cmake -S "$src" -B "$out" -G Ninja -DMOBILEGL_BUILD_STAMP="$stamp" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_ENABLE_LTO=OFF
# mobilegl_gbm: the GBM backend whose buffers are MobileGL server images (MobileGL/MG_Gbm);
# needs the GBM loader's headers (gbm.h, gbm_backend_abi.h), which the container has.
cmake --build "$out" --target MobileGL mobilegl_gbm -j4
mkdir -p /opt/mobilegl/lib /opt/mobilegl/share/glvnd/egl_vendor.d
# A new inode, renamed into place: overwriting the mapped file in place crashes every
# running client of the session (KWin included) the moment the copy lands.
cp "$out/libMobileGL.so" /opt/mobilegl/lib/libMobileGL.so.new
mv -f /opt/mobilegl/lib/libMobileGL.so.new /opt/mobilegl/lib/libMobileGL.so
for lib in libEGL.so libEGL.so.1 libGL.so libGL.so.1; do ln -sf libMobileGL.so "/opt/mobilegl/lib/$lib"; done
# The glvnd GLX vendor: libGLX picks libGLX_<name>.so.0 off the library path for
# __GLX_VENDOR_LIBRARY_NAME=mobilegl (set system-wide below), so X11 GLX clients reach MobileGL.
ln -sf /opt/mobilegl/lib/libMobileGL.so /usr/lib/libGLX_mobilegl.so.0
vendor='{"file_format_version":"1.0.0","ICD":{"library_path":"/opt/mobilegl/lib/libMobileGL.so"}}'
# Per-process forcing (__EGL_VENDOR_LIBRARY_FILENAMES=<this file>) keeps working off the /opt copy.
printf '%s\n' "$vendor" >/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json
# System-wide vendor: 10_ sorts ahead of the system's own 50_ vendor, so stock libglvnd asks
# MobileGL first and moves on to the other vendor whenever MobileGL declines (no server reachable).
mkdir -p /usr/share/glvnd/egl_vendor.d
printf '%s\n' "$vendor" >/usr/share/glvnd/egl_vendor.d/10_mobilegl.json.new
mv -f /usr/share/glvnd/egl_vendor.d/10_mobilegl.json.new /usr/share/glvnd/egl_vendor.d/10_mobilegl.json
# Client settings every process reads when its environment does not say otherwise (the backend
# comes from /etc/mobilegl/backend).
mkdir -p /etc/mobilegl
cat >/etc/mobilegl/client.conf <<'CONF'
# MobileGL client configuration, read by every process that loads libMobileGL.so.
# KEY=value lines; a MOBILEGL_* variable in a process's environment overrides the line here.
# The backend is /etc/mobilegl/backend. Written by xdeploy.sh / anland-build-client.sh.
MOBILEGL_TRANSPORT=spawn
MOBILEGL_IPC_DATA=shm
MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl
CONF
# What glvnd's libGLX and libgbm cannot infer: environment.d reaches the systemd user manager (every
# Plasma service and what it launches, terminals included), profile.d reaches login shells.
mkdir -p /etc/environment.d /etc/profile.d
printf '%s\n' '__GLX_VENDOR_LIBRARY_NAME=mobilegl' 'GBM_BACKEND=mobilegl' >/etc/environment.d/10-mobilegl.conf
printf '%s\n' 'export __GLX_VENDOR_LIBRARY_NAME=mobilegl' 'export GBM_BACKEND=mobilegl' >/etc/profile.d/mobilegl.sh
# libgbm loads <name>_gbm.so from /usr/lib/gbm for GBM_BACKEND=<name> (set above). Renamed into
# place like the library: the compositor has it mapped.
mkdir -p /usr/lib/gbm
cp "$out/MobileGL/MG_Gbm/mobilegl_gbm.so" /usr/lib/gbm/mobilegl_gbm.so.new
mv -f /usr/lib/gbm/mobilegl_gbm.so.new /usr/lib/gbm/mobilegl_gbm.so
