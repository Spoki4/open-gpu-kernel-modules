#!/bin/bash
# Builds the patched kernel modules and the test tools (no root needed).
#
#   ./build.sh          # modules + tools
#   ./build.sh tools    # only the test tools (vram-hog, gbm-hog, compr-test, nvt-game)
set -euo pipefail

KVER=${KVER:-$(uname -r)}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
VK_HEADERS=${VK_HEADERS:-$HERE/.vulkan-headers}
GLSLANG_DIR=${GLSLANG_DIR:-$HERE/.glslang}
GLSLANG_VER=16.6.0
LIBDIR=/usr/lib/x86_64-linux-gnu

if [[ ${1:-} != tools ]]; then
    nice make -C "$HERE/.." modules -j"$(nproc)" SYSSRC="/lib/modules/$KVER/build"
fi

if [[ ! -d $VK_HEADERS/include/vulkan ]]; then
    git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git "$VK_HEADERS"
fi
gcc -O2 -Wall -pthread -I"$VK_HEADERS/include" -o "$HERE/vram-hog" "$HERE/vram-hog.c" \
    "$LIBDIR/libvulkan.so.1" -ldl
gcc -O2 -Wall -o "$HERE/gbm-hog" "$HERE/gbm-hog.c" "$LIBDIR/libgbm.so.1" -ldl
gcc -O2 -Wall -I"$VK_HEADERS/include" -o "$HERE/compr-test" "$HERE/compr-test.c" "$LIBDIR/libvulkan.so.1"

# nvt-game's compute shader: compiled to SPIR-V and embedded as a C array.
if [[ ! -x $GLSLANG_DIR/bin/glslang ]]; then
    mkdir -p "$GLSLANG_DIR"
    curl -sSfL "https://github.com/KhronosGroup/glslang/releases/download/$GLSLANG_VER/glslang-$GLSLANG_VER-linux-x86_64-release.tar.gz" \
        | tar xz -C "$GLSLANG_DIR"
fi
"$GLSLANG_DIR/bin/glslang" -V -S comp --vn nvt_game_spv -o "$HERE/nvt-game.spv.h" "$HERE/nvt-game.comp" >/dev/null
"$GLSLANG_DIR/bin/glslang" -V -S vert --vn nvt_vert_spv -o "$HERE/nvt-game.vert.spv.h" "$HERE/nvt-game.vert" >/dev/null
"$GLSLANG_DIR/bin/glslang" -V -S frag --vn nvt_frag_spv -o "$HERE/nvt-game.frag.spv.h" "$HERE/nvt-game.frag" >/dev/null
gcc -O2 -Wall -I"$VK_HEADERS/include" -I"$HERE" -o "$HERE/nvt-game" "$HERE/nvt-game.c" \
    "$LIBDIR/libvulkan.so.1" -ldl -lm

if [[ ${1:-} != tools ]]; then
    echo "built: $(modinfo -F version "$HERE/../kernel-open/nvidia.ko") for $KVER"
fi
echo "built: $HERE/vram-hog $HERE/gbm-hog $HERE/compr-test $HERE/nvt-game"
