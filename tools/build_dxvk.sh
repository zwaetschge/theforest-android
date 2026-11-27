#!/bin/sh
# Prepare DXVK native 1.9.2b (gfx/dxvk/prepare.py) and build the static library.
#   tools/build_dxvk.sh host      -> build/dxvk-host/libhp_dxvk.a   (Linux x86-64, in hp-gfx:1)
#   tools/build_dxvk.sh android   -> build/dxvk-android/libhp_dxvk.a (NDK arm64)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
T=${1:-host}
git config --global --add safe.directory '*' 2>/dev/null || true
SRC=build/dxvk-src
if [ ! -f $SRC/PREPARE_SHA256 ] || [ "$(cat $SRC/PREPARE_SHA256)" != "$(sha256sum gfx/dxvk/prepare.py | cut -d' ' -f1)" ]; then
  python3 gfx/dxvk/prepare.py third_party/dxvk-native $SRC
  python3 gfx/dxvk/shaders.py $SRC
fi
if [ "$T" = host ]; then
  cmake -S gfx/dxvk -B build/dxvk-host -G Ninja -DDXVK_SOURCE=$ROOT/$SRC -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
  ninja -C build/dxvk-host
else
  NDK=${ANDROID_NDK:-$(ls -d /sdk/ndk/* | tail -1)}
  cmake -S gfx/dxvk -B build/dxvk-android -G Ninja -DDXVK_SOURCE=$ROOT/$SRC -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DANDROID_STL=c++_static >/dev/null
  ninja -C build/dxvk-android
fi
