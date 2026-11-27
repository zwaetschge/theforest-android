#!/bin/sh
# Fetch the pinned third-party sources used by the build into third_party/ (never committed):
#   - pcrecomp (MIT, x86-64 -> C lifter) at efa71c9 + tools/patches/pcrecomp-theforest.patch
#   - dxvk-native (zlib, Direct3D 11 -> Vulkan) at tag native-1.9.2b
#   - Unity-Technologies/mono (MIT/LGPL, unity-5.6-mbe Boehm GC) at 1a11b91 + tools/patches/mono-unity56-android.patch
#   - SDL 3.2.12 (zlib, Android window/input/gamepad/audio)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP=$ROOT/third_party

PCRECOMP_REPO=https://github.com/sp00nznet/pcrecomp.git
PCRECOMP_COMMIT=efa71c9db36d91728d00f1077a1a5d0530d9057f
DXVK_REPO=https://github.com/Joshua-Ashton/dxvk-native.git
DXVK_TAG=native-1.9.2b
MONO_REPO=https://github.com/Unity-Technologies/mono.git
MONO_COMMIT=1a11b91fe95a72b1d5e169b1c2596489ca8c8b34

mkdir -p "$TP/dl"
cd "$TP"
get() { [ -s "$2" ] || curl -fsSL -o "$2" "$1"; }

if [ ! -d pcrecomp/.git ]; then
  git clone -q "$PCRECOMP_REPO" pcrecomp
  (cd pcrecomp && git checkout -q "$PCRECOMP_COMMIT" && git apply "$ROOT/tools/patches/pcrecomp-theforest.patch")
  echo "pcrecomp $PCRECOMP_COMMIT + theforest patch"
fi

if [ ! -d dxvk-native/.git ]; then
  git clone -q --depth 1 --branch "$DXVK_TAG" "$DXVK_REPO" dxvk-native
  echo "dxvk-native $DXVK_TAG"
fi

if [ ! -d mono/.git ]; then
  git clone -q "$MONO_REPO" mono
  (cd mono && git checkout -q "$MONO_COMMIT" && git apply "$ROOT/tools/patches/mono-unity56-android.patch")
  echo "Unity-Technologies/mono $MONO_COMMIT + unity56-android patch"
fi

[ -d SDL3-3.2.12 ] || {
  get https://github.com/libsdl-org/SDL/releases/download/release-3.2.12/SDL3-3.2.12.tar.gz dl/SDL3-3.2.12.tar.gz
  tar xzf dl/SDL3-3.2.12.tar.gz
}

echo "third_party ready"
