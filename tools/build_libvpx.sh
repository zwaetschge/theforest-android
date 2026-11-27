#!/bin/sh
# Native libvpx (VP8 decoder only, NEON) for Android arm64: build/libvpx-android/{libvpx.a,include}.
# Used by runtime/src/vp8native.c to replace the game's lifted libvpx 1.3.0 VP8 decoder (the TV video).
#   tools/build_libvpx.sh            (in hp-gfx:1, NDK under /sdk/ndk)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
VER=v1.15.0
SRC=third_party/libvpx-$VER
OUT=build/libvpx-android
[ -d $SRC ] || git clone --depth 1 -b $VER https://github.com/webmproject/libvpx.git $SRC
NDK=${ANDROID_NDK:-$(ls -d /sdk/ndk/* | tail -1)}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p $OUT/obj && cd $OUT/obj
if [ ! -f Makefile ]; then
  CC=$TC/aarch64-linux-android29-clang CXX=$TC/aarch64-linux-android29-clang++ AR=$TC/llvm-ar \
  AS=$TC/aarch64-linux-android29-clang LD=$TC/aarch64-linux-android29-clang STRIP=$TC/llvm-strip NM=$TC/llvm-nm \
  CROSS=aarch64-linux-android- \
  $ROOT/$SRC/configure --target=arm64-android-gcc --disable-examples --disable-tools --disable-docs \
    --disable-unit-tests --disable-vp9 --disable-vp8-encoder --enable-vp8-decoder --enable-pic \
    --disable-webm-io --disable-libyuv --enable-multithread --disable-debug --enable-optimizations
fi
make -j${JOBS:-16} libvpx.a
cp libvpx.a ../libvpx.a
mkdir -p ../include/vpx && cp $ROOT/$SRC/vpx/*.h ../include/vpx/
echo "libvpx $VER -> $OUT/libvpx.a"
