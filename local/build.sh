#!/bin/bash
# Builds the patched kernel modules and the test tools (no root needed).
#
#   ./build.sh          # modules + tools
#   ./build.sh tools    # only the test tools (vram-hog, gbm-hog, compr-test, nvt-game)
set -euo pipefail

KVER=${KVER:-$(uname -r)}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
T=$HERE/tests
VK_HEADERS=${VK_HEADERS:-$HERE/.vulkan-headers}
GLSLANG_DIR=${GLSLANG_DIR:-$HERE/.glslang}
GLSLANG_VER=16.6.0
# Debian/Ubuntu multiarch dir, else /usr/lib (Arch, CachyOS: the test VM guest).
LIBDIR=${LIBDIR:-/usr/lib/x86_64-linux-gnu}
[[ -e $LIBDIR/libvulkan.so.1 ]] || LIBDIR=/usr/lib

if [[ ${1:-} != tools ]]; then
    # A kernel built with clang (CachyOS) needs its linker; the modules
    # themselves are still compiled with gcc.
    extra=()
    grep -q '^CONFIG_CC_IS_CLANG=y' "/lib/modules/$KVER/build/.config" 2>/dev/null && extra=(LD=ld.lld IGNORE_CC_MISMATCH=1)
    nice make -C "$HERE/.." modules -j"$(nproc)" SYSSRC="/lib/modules/$KVER/build" "${extra[@]}"
fi

if [[ ! -d $VK_HEADERS/include/vulkan ]]; then
    git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git "$VK_HEADERS"
fi
gcc -O2 -Wall -pthread -I"$VK_HEADERS/include" -o "$T/vram-hog" "$T/vram-hog.c" \
    "$LIBDIR/libvulkan.so.1" -ldl
gcc -O2 -Wall -o "$T/gbm-hog" "$T/gbm-hog.c" "$LIBDIR/libgbm.so.1" -ldl
gcc -O2 -Wall -I"$VK_HEADERS/include" -o "$T/compr-test" "$T/compr-test.c" "$LIBDIR/libvulkan.so.1"

# nvt-game's compute shader: compiled to SPIR-V and embedded as a C array.
if [[ ! -x $GLSLANG_DIR/bin/glslang ]]; then
    mkdir -p "$GLSLANG_DIR"
    curl -sSfL "https://github.com/KhronosGroup/glslang/releases/download/$GLSLANG_VER/glslang-$GLSLANG_VER-linux-x86_64-release.tar.gz" \
        | tar xz -C "$GLSLANG_DIR"
fi
"$GLSLANG_DIR/bin/glslang" -V -S comp --vn nvt_game_spv -o "$T/nvt-game.spv.h" "$T/nvt-game.comp" >/dev/null
"$GLSLANG_DIR/bin/glslang" -V -S vert --vn nvt_vert_spv -o "$T/nvt-game.vert.spv.h" "$T/nvt-game.vert" >/dev/null
"$GLSLANG_DIR/bin/glslang" -V -S frag --vn nvt_frag_spv -o "$T/nvt-game.frag.spv.h" "$T/nvt-game.frag" >/dev/null
gcc -O2 -Wall -I"$VK_HEADERS/include" -I"$T" -o "$T/nvt-game" "$T/nvt-game.c" \
    "$LIBDIR/libvulkan.so.1" -ldl -lm

if [[ ${1:-} != tools ]]; then
    echo "built: $(modinfo -F version "$HERE/../kernel-open/nvidia.ko") for $KVER"
fi
echo "built: $T/vram-hog $T/gbm-hog $T/compr-test $T/nvt-game"
