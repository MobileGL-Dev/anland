#!/system/bin/sh
# Overlay the recorded glslang (gitlink d89cf443) over the container source's
# dirty-submodule copy, PRESERVING External/ (nested SPIRV-Tools checkout that
# glslang's own git does not track), then rebuild the client.
set -e
SRC=/mnt/Droidspaces/arch-kde-mgl/root/mobilegl-unified-src
GLSLANG="$SRC/3rdparty/glslang"
find "$GLSLANG" -mindepth 1 -maxdepth 1 ! -name External -exec rm -rf {} +
tar -xzf /data/local/tmp/glslang-clean.tar.gz -C "$GLSLANG"
# git archive stamps the commit date on every file; ninja would skip the
# rebuild against newer dirty-tree objects. Touch everything instead.
find "$GLSLANG" -type f -exec touch {} +
echo "[overlay] glslang reset to recorded gitlink; External preserved: $(ls "$GLSLANG/External" 2>/dev/null | tr '\n' ' ')"
