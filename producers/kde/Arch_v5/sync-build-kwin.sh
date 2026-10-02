#!/bin/bash
# Sync the canonical anland backend sources into the container KWin tree,
# rebuild incrementally with ninja, and install into /opt/mobilegl/kwin.
# Never restarts the running compositor; installs via rename so the running
# process keeps its old inode mapped.
set -euo pipefail

SRC=/root/mobilegl-build/kwin-6.7.4
BUILD=$SRC/build
DST=$SRC/src/backends/anland
SYNC=/run/anland-mobilegl/kwin-sync
TS=$(date +%Y%m%d-%H%M%S)

echo "== preflight =="
if pgrep -x ninja >/dev/null 2>&1 || pgrep -x cc1plus >/dev/null 2>&1 || pgrep -x cmake >/dev/null 2>&1; then
    echo "ABORT: a build is already running"
    exit 9
fi
ps -eo pid,user,etime,args | grep -E 'kwin_wayland|desktop-session' | grep -v grep || true

echo "== backup current container backend dir =="
BK=/root/mobilegl-build/anland-backup-$TS
mkdir -p "$BK"
cp -a "$DST/." "$BK/"
cp -a "$SYNC/misc/kwin.patch" "$BK/kwin.patch.staged"
echo "backup -> $BK"

echo "== sync backend sources =="
install -m644 "$SYNC"/anland/anland_backend.cpp "$SYNC"/anland/anland_backend.h \
    "$SYNC"/anland/anland_egl_backend.cpp "$SYNC"/anland/anland_egl_backend.h \
    "$SYNC"/anland/anland_input.cpp "$SYNC"/anland/anland_input.h \
    "$SYNC"/anland/anland_logging.cpp "$SYNC"/anland/anland_logging.h \
    "$SYNC"/anland/anland_output.cpp "$SYNC"/anland/anland_output.h \
    "$SYNC"/anland/CMakeLists.txt "$DST"/

echo "== verify byte-identical =="
for f in "$SYNC"/anland/anland_backend.cpp "$SYNC"/anland/anland_backend.h \
         "$SYNC"/anland/anland_egl_backend.cpp "$SYNC"/anland/anland_egl_backend.h \
         "$SYNC"/anland/anland_input.cpp "$SYNC"/anland/anland_input.h \
         "$SYNC"/anland/anland_logging.cpp "$SYNC"/anland/anland_logging.h \
         "$SYNC"/anland/anland_output.cpp "$SYNC"/anland/anland_output.h \
         "$SYNC"/anland/CMakeLists.txt; do
    b=$(basename "$f")
    if cmp -s "$f" "$DST/$b"; then echo "OK       $b"; else echo "MISMATCH $b"; exit 1; fi
done

echo "== canonical markers present =="
grep -n "failPendingFrame" "$DST/anland_output.h" "$DST/anland_output.cpp" "$DST/anland_backend.cpp" || true
grep -n "count < 1" "$DST/anland_backend.cpp" || true

echo "== patch state (reverse dry-run of staged patch) =="
cd "$SRC"
patch -p1 --dry-run -R --forward < "$SYNC/misc/kwin.patch" 2>&1 | grep -E 'FAILED|succeeded|ignored|Hunk' || true

echo "== build =="
date
START=$(date +%s)
ninja -C "$BUILD" -j4
END=$(date +%s)
echo "build_wall_seconds=$((END-START))"

echo "== install (atomic rename) =="
TMP=$(mktemp -d /opt/mobilegl/kwin/.install-XXXXXX)
install -m755 "$BUILD/bin/kwin_wayland" "$TMP/kwin_wayland"
cp -a "$BUILD/bin/libkwin.so.6.7.4" "$TMP/libkwin.so.6.7.4"
mv -f "$TMP/kwin_wayland" /opt/mobilegl/kwin/bin/kwin_wayland
mv -f "$TMP/libkwin.so.6.7.4" /opt/mobilegl/kwin/lib/libkwin.so.6.7.4
ln -sfn libkwin.so.6.7.4 /opt/mobilegl/kwin/lib/libkwin.so.6
ln -sfn libkwin.so.6 /opt/mobilegl/kwin/lib/libkwin.so
rmdir "$TMP"
# KWin loads its plugins from the system plugin path, so the patched screencast plugin (no dma-buf
# probe without a DRM device: a window screencast, e.g. a task-manager thumbnail, crashed KWin)
# replaces the distro one in place; the original is kept beside it.
SCREENCAST=/usr/lib/qt6/plugins/kwin/plugins/screencast.so
[ -f "$SCREENCAST.orig" ] || cp -a "$SCREENCAST" "$SCREENCAST.orig"
install -m755 "$BUILD/bin/kwin/plugins/screencast.so" "$SCREENCAST.new" && mv -f "$SCREENCAST.new" "$SCREENCAST"
install -m755 "$SYNC/misc/mobilegl-startup.sh" /opt/mobilegl/bin/mobilegl-startup.sh

echo "== installed =="
ls -la --time-style=full-iso /opt/mobilegl/kwin/bin/kwin_wayland /opt/mobilegl/kwin/lib/
ls -la /opt/mobilegl/bin/mobilegl-startup.sh
echo "md5 build vs installed:"
md5sum "$BUILD/bin/libkwin.so.6.7.4" /opt/mobilegl/kwin/lib/libkwin.so.6.7.4

echo "== symbols in installed lib =="
nm -C /opt/mobilegl/kwin/lib/libkwin.so.6.7.4 2>/dev/null | grep -c "failPendingFrame" || true

echo "== ldd (LD_LIBRARY_PATH=/opt/mobilegl/kwin/lib) =="
LD_LIBRARY_PATH=/opt/mobilegl/kwin/lib ldd /opt/mobilegl/kwin/bin/kwin_wayland | grep -i "not found" && echo "MISSING" || echo "kwin_wayland: no missing libs"
LD_LIBRARY_PATH=/opt/mobilegl/kwin/lib ldd /opt/mobilegl/kwin/lib/libkwin.so.6.7.4 | grep -i "not found" && echo "MISSING" || echo "libkwin: no missing libs"
LD_LIBRARY_PATH=/opt/mobilegl/kwin/lib ldd /opt/mobilegl/kwin/bin/kwin_wayland | grep -E 'libkwin|libEGL|libGL\.'

echo "== version check as swung0x48 =="
runuser -u swung0x48 -- env LD_LIBRARY_PATH=/opt/mobilegl/kwin/lib /opt/mobilegl/kwin/bin/kwin_wayland --version

echo "== compositor still running? =="
ps -eo pid,user,etime,args | grep kwin_wayland | grep -v grep || echo "no kwin_wayland running"

echo "== ALL DONE =="
