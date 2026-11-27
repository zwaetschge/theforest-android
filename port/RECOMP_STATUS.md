# The Forest – Static Recompilation Status

Technical log of the native Android (`arm64-v8a`) static recompilation of **The Forest** (Unity `5.6.5p4`, Win64, Direct3D 11, Mono 2.0/4.8 scripting, FMOD Studio).

## Lifted Win64 PE Modules

All five native Win64 binaries are rebased to non-overlapping virtual addresses (`tools/rebase.py`), cataloged from `.pdata` (`tools/pdata_catalog.py`), lifted to portable C with `pcrecomp` (`tools/lift_module.sh` + `tools/patches/pcrecomp-theforest.patch`), post-processed (`tools/patch_gen.py`), and compiled directly into `libmain.so` via the Android NDK (`android/native/CMakeLists.txt`).

| Module | Base VA | SHA-256 | Lifted Functions | C Units | `-O2` / `-O0` |
|---|---|---|---:|---:|---|
| `TheForest.exe` | `0x140000000` | `2e93a06b8bd865d5aa0ee7257adc7cdfd3d3b8bd182584ef77d7df6bb936c943` | 231,740 | 580 | 564 `-O2` / 16 `-O0` |
| `TheForest_Data/Plugins/fmod.dll` | `0x180000000` | `76157845d72b2144c755f66c9359ba24a2f173e1518afac0d058626f70b6ec9a` | 4,196 | 11 | 11 `-O2` |
| `TheForest_Data/Plugins/fmodstudio.dll` | `0x181000000` | `cf9bbdebab60ec6212d2222788c95e326d901c6dc47b106c4e63c5f9261ad3e5` | 9,321 | 24 | 24 `-O2` |
| `TheForest_Data/Plugins/CSteamworks.dll` | `0x182000000` | `014b8d88f40584ec724c06abcdefcc40e81a4246da692a3468d7b6b6f243fc8f` | 981 | 3 | 3 `-O2` |
| `TheForest_Data/Plugins/UnityImageNativePlugin.dll` | `0x182100000` | `061ba680e7c3e63fd9bf077e3e76c207dc3a8fb22b2365dcd3472bc67c52f051` | 162 | 1 | 1 `-O2` |
| **Total** | | | **246,400** | **619** | **603 `-O2` (97.4%) / 16 `-O0`** |

### Selective `-O0` Bisect (`config/o0_host.txt`)

Compiling 603 of 619 units at `-O2 -march=armv8.2-a` shrinks `libmain.so` from `708 MB` to `207 MB` (APK `208 MB` → `76.1 MiB` / `79.8 MB`), raises the 3D Title Screen from `35.5 fps` to locked `60 fps`, cuts `ForestMain_v08` scene load time from `46.4 s` to `7.8 s`, and boosts open-world FPS by ~60%. Two narrow unit ranges remain at `-O0`:

- `TheForest/recomp_funcs_0017`–`0024` (8 units): Unity Camera / Collider / NGUI `UIButton` click dispatch stability under `-O2`.
- `TheForest/recomp_funcs_0038`–`0045` (8 units): Unity `TimeManager` / `StreamedBinaryRead` generic Mono deserialization (`ItemDatabase.OnEnable()` duplicate-key regression under `-O2`).

## Runtime & Subsystems

- **Native ARM64 Unity Mono (`unity-5.6-mbe`)**: Cross-compiled static ARM64 Mono 4.8 runtime with Boehm GC (`tools/build_mono_android.sh`, `tools/patches/mono-unity56-android.patch`) bridged to the lifted Unity 5.6 player via `runtime/src/mono_bridge.c` and ARM64 Win64-ABI trampolines (`runtime/src/mono_tramp.S`). Includes a patched open-source `mscorlib.dll` (`System.Threading.Thread.Start(object)` compatibility with `AstarPath` / `mutantController` worker threads), bundled in `assets/mono_bcl/`.
- **Direct3D 11 → Vulkan 1.1**: `gfx/hp_d3d11.cpp` bridges lifted Win64 COM calls into **DXVK Native 1.9.2b** (`gfx/dxvk/prepare.py`, `gfx/dxvk/shaders.py`) with Android WSI, `dxvk-async` (2 compiler threads), and persistent Adreno `VkPipelineCache` (`vk_pipeline.cache`) + `app_process64.dxvk-cache`.
- **Audio (FMOD Studio + WASAPI → SDL3)**: Lifted `fmod.dll` and `fmodstudio.dll` output through our Win32 WASAPI `IAudioClient` / `IAudioRenderClient` implementation (`runtime/src/wasapi.c`) directly to an SDL3 48 kHz stereo stream (`0 of 1000 blocks short`).
- **Input & Android Shell**: Physical controllers map to XInput slot 0 (`runtime/src/xinput.c`); `PadView.java` provides a toggleable on-screen gamepad plus right-half touch-look and menu tap-to-click. `Launcher.java` / `GameImporter.java` support SAF folder import, manual ZIP/ZIP64 import, and automatic Plum Hub ZIP64 unpacking from `Android/obb/ch.plum.theforest/`.
- **Offline Steamworks Stub**: `runtime/src/steam.c` stubs `CSteamworks` / `SteamAPI` for full offline singleplayer operation.

## Hardware Verification — AYN Odin 3 (`Adreno 830`, Android 15)

Measured on release APK `out/TheForest-Android-0.1.0.apk` (`79,791,910` bytes, SHA-256 `5a50e034acc8bb8d5d3a9548d91113e7e1c8993b9bd79210d5ddaa3b91194662`) with `HP_STUTTER=1` at default `1280×720` render resolution:

| Check | Result |
|---|---|
| **ZIP64 Data Pack Auto-Unpack** | `theforest-data-0.1.0.zip` (`1,991,080,142` B, 277 files) unpacked automatically from `Android/obb/ch.plum.theforest/`, all 5 PE SHA-256 verified, ZIP cleaned |
| **Title Screen (3D Cave + Menu)** | **59.7–60.3 fps**, `p50 = 16.5–16.8 ms`, `p95 = 19.0–20.3 ms`, `p99 = 20.1–21.5 ms`, `0` frames ≥ 30 ms steady-state, `gp = 0(0ms)` |
| **Title Screen Memory** | `1.11 GB` PSS (`1,114,842 KB`) |
| **New Game → `ForestMain_v08` Load** | `Load ForestMain_v08: 7.83 s` (`7,833 ms`), `Activation: 9.77 s` (`9,767 ms`), **`0` Astar/Mono/ItemDatabase exceptions** |
| **3D Open-World Forest Gameplay** | **14.1–16.3 fps** (`p50 = 68.1–71.7 ms`, `p95 = 74.9–81.5 ms`, `p99 = 78.0–88.0 ms`, `gp = 0(0ms)` steady-state), waking up in crashed plane, walking outside into sunlit pine forest, red plane axe, touch-look & HUD verified |
| **In-Game Memory** | `2.07 GB` PSS (`2,065,703 KB`, Graphics `204 MB`, Native Heap `332 MB`) |
| **FMOD WASAPI Audio** | Live stereo ambient & SFX (`peak 0.053–0.163`, `0 of 1000 blocks short`) |
| **Background / Resume Lifecycle** | `KEYCODE_HOME` → `[android] paused (surface detached)` + `vk_pipeline.cache` (`7.2 MB`) & `app_process64.dxvk-cache` (`201 KB`) saved → relaunch resumes **same PID** (`14.4–14.6 fps`, audio active) |

## Known Limitations

- **Open-World Framerate (`14–16 fps` at default settings):** Unity 5.6's CPU-heavy terrain/foliage/shader pipeline in `ForestMain_v08` runs at ~14–16 fps on Odin 3 at `1280×720` default quality; lowering in-game graphics options (shadows, grass, draw distance) improves performance. Further `-O2` narrowing of the remaining 16 `-O0` units and Unity render-thread hotspots is future work.
- **Multiplayer / Steamworks:** Stubbed offline (`runtime/src/steam.c`); singleplayer (`Normal`, `Hard`, `Hard Survival`, `Peaceful`) works fully offline.
- **First-Run Title Cave Lighting:** On the very first boot before `PlayerPreferences` / shader cache initializes, the title screen background cave may appear dark until navigating a menu or starting a game; subsequent launches show the full lit 3D cave immediately.
- **GPU Coverage:** Verified on Qualcomm Adreno 830; Mali and Xclipse GPUs are untested.
