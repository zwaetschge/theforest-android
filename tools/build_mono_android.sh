#!/bin/sh
# Cross-compile Unity-Technologies/mono (unity-5.6-mbe) static Boehm runtime for Android arm64-v8a.
# Unity 5.6's C++ engine requires Boehm (non-moving conservative GC), matching desktop mono.dll.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_SDK:-/sdk}
NDK=$SDK/ndk/27.3.13750724
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
BD=$ROOT/build/mono-android
SRC=$ROOT/third_party/mono

if [ ! -f "$SRC/configure" ]; then
  echo "ERROR: $SRC/configure missing (run autogen.sh first)" >&2
  exit 1
fi

mkdir -p "$BD"
cd "$BD"

export CC="$TC/aarch64-linux-android29-clang"
export CXX="$TC/aarch64-linux-android29-clang++"
export AR="$TC/llvm-ar"
export RANLIB="$TC/llvm-ranlib"
export STRIP="$TC/llvm-strip"
export LD="$TC/ld.lld"
export CFLAGS="-O2 -fPIC -fno-strict-aliasing -fwrapv -DANDROID -D__ANDROID__ -Wno-error -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-function-pointer-types"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-Wl,-z,max-page-size=16384"

if [ ! -f "$BD/libgc/Makefile" ]; then
  rm -rf "$BD"/*
  "$SRC/configure" \
    --host=aarch64-linux-android \
    --build=x86_64-pc-linux-gnu \
    --enable-boehm=yes \
    --with-gc=included \
    --with-sgen=no \
    --enable-btls=no \
    --disable-nls \
    --disable-mcs-build \
    --without-ikvm-native \
    --with-sigaltstack=no \
    --enable-static \
    --disable-shared \
    mono_cv_uscore=no \
    ac_cv_func_shm_open_working_with_mmap=no \
    ac_cv_func_getpwuid_r=no \
    ac_cv_func_getgrgid_r=no \
    ac_cv_header_sys_ipc_h=no \
    ac_cv_header_sys_sem_h=no \
    ac_cv_header_sys_shm_h=no
fi

J=${JOBS:-12}
make -C eglib -j$J
make -C libgc -j$J
make -C mono/utils -j$J
make -C mono/io-layer -j$J
make -C mono/metadata -j$J libmonoruntime-static.la libmonoruntime-config.la
make -C mono/arch -j$J
make -C mono/mini version.h cpu-arm64.h
make -C mono/mini -j$J libmini-static.la

ls -lh \
  "$BD/mono/mini/.libs/libmini-static.a" \
  "$BD/mono/metadata/.libs/libmonoruntime-static.a" \
  "$BD/mono/metadata/.libs/libmonoruntime-config.a" \
  "$BD/libgc/.libs/libmonogc-static.a" \
  "$BD/mono/io-layer/.libs/libwapi.a" \
  "$BD/mono/utils/.libs/libmonoutils.a" \
  "$BD/eglib/src/.libs/libeglib.a"
