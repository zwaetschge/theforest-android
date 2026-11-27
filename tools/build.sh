#!/bin/sh
# One-shot build: your own The Forest PC copy -> out/TheForest-Android-<version>.apk
#   tools/build.sh "/path/to/The Forest"   (folder containing TheForest.exe + TheForest_Data/)
#
# Run inside the build image (docker/Dockerfile), or on a Linux host with the same tools and
# ANDROID_SDK pointing at an SDK with NDK 27.3.13750724 and CMake 3.31.6.
# Optional signing env vars: TF_KEYSTORE, TF_KEYSTORE_PASSWORD, TF_KEY_ALIAS, TF_KEY_PASSWORD
# (otherwise a persistent release keystore in work/release.keystore is generated and reused).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

IN=${1:?usage: tools/build.sh </path/to/The Forest>}
[ -f "$IN/TheForest.exe" ] || { echo "TheForest.exe not found in: $IN"; exit 1; }
[ -d "$IN/TheForest_Data/Plugins" ] || { echo "TheForest_Data/Plugins not found in: $IN"; exit 1; }

check_hash() {
  local rel="$1" want="$2"
  local have=$(sha256sum "$IN/$rel" | cut -d' ' -f1)
  if [ "$have" != "$want" ]; then
    echo "$rel SHA-256: $have"
    echo "Expected:          $want"
    [ -n "$FORCE" ] || { echo "Other builds will most likely not work; set FORCE=1 to try anyway."; exit 1; }
  fi
}
check_hash "TheForest.exe"                                  2e93a06b8bd865d5aa0ee7257adc7cdfd3d3b8bd182584ef77d7df6bb936c943
check_hash "TheForest_Data/Plugins/fmod.dll"                76157845d72b2144c755f66c9359ba24a2f173e1518afac0d058626f70b6ec9a
check_hash "TheForest_Data/Plugins/fmodstudio.dll"          cf9bbdebab60ec6212d2222788c95e326d901c6dc47b106c4e63c5f9261ad3e5
check_hash "TheForest_Data/Plugins/CSteamworks.dll"         014b8d88f40584ec724c06abcdefcc40e81a4246da692a3468d7b6b6f243fc8f
check_hash "TheForest_Data/Plugins/UnityImageNativePlugin.dll" 061ba680e7c3e63fd9bf077e3e76c207dc3a8fb22b2365dcd3472bc67c52f051

SDK=${ANDROID_SDK:-/sdk}
[ -d "$SDK/ndk/27.3.13750724" ] || { echo "ANDROID_SDK ($SDK) has no ndk/27.3.13750724"; exit 1; }

echo "== 1/7 third-party sources"
sh tools/fetch_deps.sh

echo "== 2/7 rebase & stage the 5 Win64 PE modules"
mkdir -p ref/lift work/stage work/cat gen
cp "$IN/TheForest.exe" work/stage/TheForest.exe
cp "$IN/TheForest_Data/Plugins/fmod.dll" work/stage/fmod.dll
cp "$IN/TheForest_Data/Plugins/fmodstudio.dll" work/stage/fmodstudio.dll
cp "$IN/TheForest_Data/Plugins/CSteamworks.dll" work/stage/CSteamworks.dll
cp "$IN/TheForest_Data/Plugins/UnityImageNativePlugin.dll" work/stage/UnityImageNativePlugin.dll
python3 tools/rebase.py work/stage ref/lift \
  TheForest.exe=0x140000000 \
  fmod.dll=0x180000000 \
  fmodstudio.dll=0x181000000 \
  CSteamworks.dll=0x182000000 \
  UnityImageNativePlugin.dll=0x182100000
cat > ref/lift/layout.json <<'EOF'
{
  "TheForest.exe": "0x140000000",
  "TheForest_Data/Plugins/fmod.dll": "0x180000000",
  "TheForest_Data/Plugins/fmodstudio.dll": "0x181000000",
  "TheForest_Data/Plugins/CSteamworks.dll": "0x182000000",
  "TheForest_Data/Plugins/UnityImageNativePlugin.dll": "0x182100000"
}
EOF

echo "== 3/7 function catalogs & x86-64 -> C lift (a few minutes)"
for M in TheForest fmod fmodstudio CSteamworks UnityImageNativePlugin; do
  PE="ref/lift/$M.dll"; [ -f "$PE" ] || PE="ref/lift/$M.exe"
  python3 tools/pdata_catalog.py "$PE" "work/cat/$M.txt"
  sh tools/lift_module.sh "$M"
done
python3 tools/patch_gen.py

echo "== 4/7 prepare DXVK Native D3D11 & compile built-in Vulkan shaders"
SRC=build/dxvk-src
if [ ! -f "$SRC/PREPARE_SHA256" ] || [ "$(cat "$SRC/PREPARE_SHA256")" != "$(sha256sum gfx/dxvk/prepare.py | cut -d' ' -f1)" ]; then
  git config --global --add safe.directory '*' 2>/dev/null || true
  python3 gfx/dxvk/prepare.py third_party/dxvk-native "$SRC"
  python3 gfx/dxvk/shaders.py "$SRC"
fi

echo "== 5/7 cross-compile Unity Mono 4.8 (Boehm GC) for Android arm64-v8a + BCL"
if [ ! -f third_party/mono/configure ]; then
  (cd third_party/mono && NOCONFIGURE=1 ./autogen.sh)
fi
sh tools/build_mono_android.sh
if [ ! -f android/app/src/main/assets/mono_bcl/mscorlib.dll ]; then
  sh tools/build_mono_bcl.sh
fi

echo "== 6/7 compile native Android libraries (arm64-v8a)"
sh tools/build_android_so.sh

echo "== 7/7 build & sign APK"
if [ -z "$TF_KEYSTORE" ] && [ -z "$HP_KEYSTORE" ]; then
  KS="$ROOT/work/release.keystore"
  if [ ! -f "$KS" ]; then
    keytool -genkeypair -v -keystore "$KS" -storepass android -keypass android \
      -alias theforest -keyalg RSA -keysize 2048 -validity 10000 \
      -dname "CN=TheForestAndroid, O=LocalBuild" >/dev/null 2>&1
  fi
  export TF_KEYSTORE="$KS" TF_KEYSTORE_PASSWORD=android TF_KEY_ALIAS=theforest TF_KEY_PASSWORD=android
fi
echo "sdk.dir=$SDK" > android/local.properties
(cd android && sh ./gradlew -q --no-daemon assembleRelease)
VER=$(grep -o 'versionName = "[^"]*"' android/app/build.gradle.kts | cut -d'"' -f2)
mkdir -p out
cp android/app/build/outputs/apk/release/app-release.apk "out/TheForest-Android-$VER.apk"
ls -lh "out/TheForest-Android-$VER.apk"
sha256sum "out/TheForest-Android-$VER.apk"
echo "done. Install with: adb install -r out/TheForest-Android-$VER.apk"
