#!/bin/bash
set -euo pipefail
src=/root/mobilegl-unified-src
out=/root/mobilegl-unified-build
stamp=$(cat "$src/.anland-build-stamp" 2>/dev/null || echo unknown)
cmake -S "$src" -B "$out" -G Ninja -DMOBILEGL_BUILD_STAMP="$stamp" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_ENABLE_LTO=OFF
cmake --build "$out" --target MobileGL -j4
mkdir -p /opt/mobilegl/lib /opt/mobilegl/share/glvnd/egl_vendor.d
# A new inode, renamed into place: overwriting the mapped file in place crashes every
# running client of the session (KWin included) the moment the copy lands.
cp "$out/libMobileGL.so" /opt/mobilegl/lib/libMobileGL.so.new
mv -f /opt/mobilegl/lib/libMobileGL.so.new /opt/mobilegl/lib/libMobileGL.so
for lib in libEGL.so libEGL.so.1 libGL.so libGL.so.1; do ln -sf libMobileGL.so "/opt/mobilegl/lib/$lib"; done
# The glvnd GLX vendor: libGLX picks libGLX_<name>.so.0 off the library path for the
# __GLX_VENDOR_LIBRARY_NAME=mobilegl the session sets, so X11 GLX clients reach MobileGL.
ln -sf /opt/mobilegl/lib/libMobileGL.so /usr/lib/libGLX_mobilegl.so.0
printf '%s\n' '{"file_format_version":"1.0.0","ICD":{"library_path":"/opt/mobilegl/lib/libMobileGL.so"}}' >/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json
