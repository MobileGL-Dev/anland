#!/bin/bash
# anland-sync-source.sh -- sync the MobileGL worktree into the arch-kde-mgl
# clone's /root/mobilegl-unified-src and kick an incremental client build.
#
# Run from Git Bash on Windows: bash anland-sync-source.sh [build]
#   no arg : sync only
#   build  : sync, then run the in-container incremental build (nohup)
#
# Excludes are ANCHORED to the repo root: a broad `build`/`test` exclusion
# once dropped 3rdparty/xxHash/build/cmake and spirv-tools test data and
# broke the in-container build. tools/trace_replay is 789MB of game capture
# fixtures the container build never reads.
set -euo pipefail
# adb is a Windows exe reached from Git Bash; stop MSYS from mangling
# /data/... and /tmp/... remote paths into C:\Program Files\Git\...
export MSYS2_ARG_CONV_EXCL='*'
REPO="${MOBILEGL_REPO:-/c/Users/Swung0x48/repos/FoldCraftLauncher/MobileGL}"
TOOLS="$(cd "$(dirname "$0")" && pwd)"
TAR="${TMPDIR:-/tmp}/anland-mobilegl-source.tar.gz"
APPLY="${TMPDIR:-/tmp}/anland-apply-source.sh"
SERIAL="${ANLAND_SERIAL:-HA27Q3LQ}"

cd "$REPO"
echo "[sync] tarring worktree (HEAD $(git rev-parse --short HEAD) + WIP)..."
tar --exclude='./.git' --exclude='./.cxx' --exclude='./build' \
    --exclude='./tools/trace_replay' \
    --exclude='./android-plugin/app/build' --exclude='./android-plugin/.gradle' \
    -czf "$TAR" .
ls -la "$TAR"

echo "[sync] pushing to device..."
adb -s "$SERIAL" push "$TAR" /data/local/tmp/anland-mobilegl-source.tar.gz

cat > "$APPLY" <<'EOS'
#!/system/bin/sh
set -e
DEST=/mnt/Droidspaces/arch-kde-mgl/root/mobilegl-unified-src
mkdir -p "$DEST.new"
tar -xzf /data/local/tmp/anland-mobilegl-source.tar.gz -C "$DEST.new"
rm -rf "$DEST.old"
[ -d "$DEST" ] && mv "$DEST" "$DEST.old"
mv "$DEST.new" "$DEST"
rm -rf "$DEST.old"
echo "[apply] source swapped"
EOS
adb -s "$SERIAL" push "$APPLY" /data/local/tmp/anland-apply-source.sh >/dev/null
adb -s "$SERIAL" shell "su -c 'sh /data/local/tmp/anland-apply-source.sh'"

# A worktree whose 3rdparty/glslang is a dirty submodule (missing patches the
# recorded gitlink has) builds a client that SIGSEGVs in shader compilation.
# Overlay the recorded revision, preserving the nested External/ checkouts
# glslang's own git does not track. Regenerate the archive with:
#   git -C 3rdparty/glslang archive --format=tar.gz -o /tmp/glslang-clean.tar.gz \
#       $(git ls-tree HEAD 3rdparty/glslang | awk '{print $3}')
GLSLANG_TAR="${GLSLANG_CLEAN_TAR:-${TMPDIR:-/tmp}/glslang-clean.tar.gz}"
if [ -f "$GLSLANG_TAR" ]; then
    echo "[sync] overlaying recorded glslang..."
    adb -s "$SERIAL" push "$GLSLANG_TAR" /data/local/tmp/glslang-clean.tar.gz >/dev/null
    adb -s "$SERIAL" push "$TOOLS/anland-overlay-glslang.sh" /data/local/tmp/ >/dev/null
    adb -s "$SERIAL" shell "su -c 'sh /data/local/tmp/anland-overlay-glslang.sh'"
fi

if [ "${1:-}" = "build" ]; then
    echo "[sync] launching incremental client build in clone..."
    adb -s "$SERIAL" push "$TOOLS/anland-launch-client-build.sh" /data/local/tmp/ >/dev/null
    adb -s "$SERIAL" shell "su -c 'sh /data/local/tmp/anland-launch-client-build.sh'"
fi
echo "[sync] done."
