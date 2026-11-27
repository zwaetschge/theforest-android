#!/bin/sh
# Build libmain.so + libSDL3.so for arm64-v8a with the NDK and drop them into jniLibs.
#   tools/build_android_so.sh [ninja-target...]          (in hp-gfx:1, SDK at $ANDROID_SDK)
# Needs: gen/ (tools/lift_module.sh), gen/modules_table.c (tools/build_host.sh writes it),
#        build/dxvk-src (tools/build_dxvk.sh prepares it).
# env: ANDROID_SDK (default /sdk), GUEST_OPT (default "-O2;-march=armv8.2-a"), JOBS (default 14)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_SDK:-/sdk}
[ -f $ROOT/build/mono-android/mono/mini/.libs/libmini-static.a ] || sh $ROOT/tools/build_mono_android.sh
NDK=$SDK/ndk/27.3.13750724
CMAKE=$SDK/cmake/3.31.6/bin/cmake
BD=$ROOT/build/android
mkdir -p $BD
python3 $ROOT/tools/gen_modules.py $ROOT/ref/lift/layout.json $ROOT/gen/modules_table.c
$CMAKE -S $ROOT/android/native -B $BD -G Ninja -DCMAKE_MAKE_PROGRAM=$SDK/cmake/3.31.6/bin/ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release "-DGUEST_OPT=${GUEST_OPT:--O2;-march=armv8.2-a}" > $BD.cmake.log 2>&1 || { tail -30 $BD.cmake.log; exit 1; }
if [ $# -gt 0 ]; then $SDK/cmake/3.31.6/bin/ninja -C $BD -j${JOBS:-14} "$@"; exit $?; fi
$SDK/cmake/3.31.6/bin/ninja -C $BD -j${JOBS:-14}
J=$ROOT/android/app/src/main/jniLibs/arm64-v8a
mkdir -p $J
S=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
$S --strip-debug -o $J/libmain.so $BD/libmain.so
$S --strip-unneeded -o $J/libSDL3.so $(find $BD -name libSDL3.so | head -1)
ls -la $J
