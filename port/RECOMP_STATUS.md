# The Forest – Static Recompilation Status

Technical log of the native Android (`arm64-v8a`) static recompilation of **The Forest** (Unity `5.6.5p4`, Win64, Direct3D 11, Mono 2.0/4.8 scripting, FMOD Studio).

## Lifted Win64 PE Modules

All five native Win64 binaries are rebased to non-overlapping virtual addresses (`tools/rebase.py`), cataloged from `.pdata` (`tools/pdata_catalog.py`), lifted to portable C with `pcrecomp` (`tools/lift_module.sh` + `tools/patches/pcrecomp-theforest.patch`), post-processed (`tools/patch_gen.py`), and compiled directly into `libmain.so` via the Android NDK (`android/native/CMakeLists.txt`).

| Module | Base VA | SHA-256 | Lifted Functions | C Units | `-O2` / `-O0` |
|---|---|---|---:|---:|---|
| `TheForest.exe` | `0x140000000` | `2e93a06b8bd865d5aa0ee7257adc7cdfd3d3b8bd182584ef77d7df6bb936c943` | 231,740 | 580 | 575.5 `-O2` / 4.5 `-O0` |
| `TheForest_Data/Plugins/fmod.dll` | `0x180000000` | `76157845d72b2144c755f66c9359ba24a2f173e1518afac0d058626f70b6ec9a` | 4,196 | 11 | 11 `-O2` |
| `TheForest_Data/Plugins/fmodstudio.dll` | `0x181000000` | `cf9bbdebab60ec6212d2222788c95e326d901c6dc47b106c4e63c5f9261ad3e5` | 9,321 | 24 | 24 `-O2` |
| `TheForest_Data/Plugins/CSteamworks.dll` | `0x182000000` | `014b8d88f40584ec724c06abcdefcc40e81a4246da692a3468d7b6b6f243fc8f` | 981 | 3 | 3 `-O2` |
| `TheForest_Data/Plugins/UnityImageNativePlugin.dll` | `0x182100000` | `061ba680e7c3e63fd9bf077e3e76c207dc3a8fb22b2365dcd3472bc67c52f051` | 162 | 1 | 1 `-O2` |
| **Total** | | | **246,400** | **619** | **614.5 `-O2` (99.3%) / 4.5 `-O0`** |

### Selective `-O0` Bisect & `0.2.0` Optimizations (`config/o0_host.txt`, `tools/patch_gen.py`)

Compiling **614.5 of 619 units (99.3%)** at `-O2 -march=armv8.2-a` shrinks `libmain.so` from `708 MB` (`-O0`) → `207 MB` (`0.1.0`) → **`167 MB`** (`0.2.0`, APK `64 MB`):

- **Boehm GC Guest Stack Root Scanner (`0038..0045` freed to `-O2`):** `w64_gc_push_guest_roots()` registered via `GC_push_other_roots` scans all live guest Win64 stacks/CPU contexts lock-free, permanently fixing the `ItemDatabase.OnEnable()` `-O2` Mono GC regression (`0` exceptions on world load).
- **Narrowed NGUI/Unity `-O0` Window (`0017`, `0018[0..199]`, `0023`, `0024` freed to `-O2`):** Only `TheForest/recomp_funcs_0019..0022` plus the cold tail of `0018` (`L_000140237E10+`, wrapped with `#pragma clang optimize off`) remain `-O0` — dropping main-thread `-O0` profile time to **3.6%**.
- **Lock-Free `SYS_futex` Semaphores & `W64_NCPU=4` Android Default:** `runtime/src/k32_sync.c` replaces `pthread_mutex`/`pthread_cond` on `H_SEM` with atomic `FUTEX_WAIT_PRIVATE`/`FUTEX_WAKE_PRIVATE` + adaptive spin; capping Android default logical CPUs to `4` eliminates big.LITTLE worker contention (`ReleaseSemaphore` `19.1% → 3.4%`, kernel `sys` CPU halved).
- **Native ARM64 NEON & Intrinsic Replacements (`tools/patch_gen.py`):** 4-bone skinned mesh blending (`L_000140426D00`, `L_000140427B70`), FMOD Vorbis Huffman bit-reverse & 128-bit bitstream advance (`L_0001800055A0`, `L_0001800022D0`), direct TLS `GetGfxDevice()` (`L_0001402C1C00`), and native CRT `memcpy`/`memmove`/`memset`/`memcmp`/`fmodf`/`fmod`.
- **Runtime Fast Paths:** `-O3 -march=armv8.2-a` on Mono Boehm / `libmain.so` / DXVK, `HT_BITS=19` dispatch table, constant-folded `GVA(abs)` + hardware `op_bsf`/`op_bsr`, `NK_REG4`/`NK_REG8` + 64-entry `t_kcache` COM dispatch, and `mono_bridge` `host_fn`/`is_simple` icall fast path (98% hit rate).

## Runtime & Subsystems

- **Native ARM64 Unity Mono (`unity-5.6-mbe`)**: Cross-compiled static ARM64 Mono 4.8 runtime with Boehm GC (`tools/build_mono_android.sh`, `tools/patches/mono-unity56-android.patch`) bridged to the lifted Unity 5.6 player via `runtime/src/mono_bridge.c` and ARM64 Win64-ABI trampolines (`runtime/src/mono_tramp.S`). Includes a patched open-source `mscorlib.dll` (`System.Threading.Thread.Start(object)` compatibility with `AstarPath` / `mutantController` worker threads), bundled in `assets/mono_bcl/`.
- **Direct3D 11 → Vulkan 1.1**: `gfx/hp_d3d11.cpp` bridges lifted Win64 COM calls into **DXVK Native 1.9.2b** (`gfx/dxvk/prepare.py`, `gfx/dxvk/shaders.py`) with Android WSI, `dxvk-async` (2 compiler threads), and persistent Adreno `VkPipelineCache` (`vk_pipeline.cache`) + `app_process64.dxvk-cache`.
- **Audio (FMOD Studio + WASAPI → SDL3)**: Lifted `fmod.dll` and `fmodstudio.dll` output through our Win32 WASAPI `IAudioClient` / `IAudioRenderClient` implementation (`runtime/src/wasapi.c`) directly to an SDL3 48 kHz stereo stream (`0 of 1000 blocks short`).
- **Input & Android Shell**: Physical controllers map to XInput slot 0 (`runtime/src/xinput.c`); `PadView.java` provides a toggleable on-screen gamepad plus right-half touch-look and menu tap-to-click. `Launcher.java` / `GameImporter.java` support SAF folder import, manual ZIP/ZIP64 import, and automatic Plum Hub ZIP64 unpacking from `Android/obb/ch.plum.theforest/`.
- **Offline Steamworks Stub**: `runtime/src/steam.c` stubs `CSteamworks` / `SteamAPI` for full offline singleplayer operation.

## Hardware Verification — Samsung Galaxy Tab S8 Ultra (`Adreno 730`, Snapdragon 8 Gen 1, `0.2.0`)

Measured on signed release APK `out/TheForest-Android-0.2.0.apk` (`64 MB`, SHA-256 `f8a3e089a81a4e35ee95a31187055b193f65e921c0ca2a0ce0437f5bd06293d7`) with `HP_STUTTER=1;HP_SYNC_INTERVAL=0` at default `1280×800` render resolution and default `W64_NCPU=4`:

| Check | Result |
|---|---|
| **In-Place Upgrade (`0.1.0 → 0.2.0`)** | `pm install -r` succeeded cleanly with matching `work/release.keystore` signature |
| **Auto-Seeded Crisp Mobile Preset** | `QualityPreset.ensureDefault()` seeded 35 Unity `PlayerPrefs` keys (`FullRes` textures + `FXAA` + mobile shadows/physics) into `registry.dat` |
| **Title Screen (3D Cave + Menu)** | **60.4 fps**, `p50 = 16.5–16.7 ms`, `p95 = 18.9–19.8 ms`, `0` frames ≥ 30 ms steady-state, `gp = 0(0ms)` |
| **New Game → `ForestMain_v08` Load** | `Load ForestMain_v08: 14.42 s`, `Activation: 7.93 s`, **`0` Astar/Mono/ItemDatabase exceptions** |
| **Open-World `ForestMain_v08` (PC Medium, pure-CPU)** | `9.8 fps` (`102.1 ms` in `0.1.0`) → **19–21 fps** (`47.9–50.1 ms` in `0.2.0` — **2.13× pure-CPU speedup**) |
| **Open-World `ForestMain_v08` (`0.2.0` Default Preset)** | **30.9–34.2 fps** (`p50 = 27.5–32.6 ms`, `p95 = 37.2–38.4 ms`, `pres p50 = 0.4–0.5 ms`, `gp = 0(0ms)` — **3.3×–3.6× faster** than `0.1.0`!) |
| **In-Game Memory** | `2.98 GB` PSS (`2,979,586 KB`, Native Heap `203 MB`, Graphics `1,378 MB`) |
| **Background / Resume Lifecycle** | `KEYCODE_HOME` → `[android] paused (surface detached)` → relaunch resumes **same PID (`25419`)** cleanly at `p50 = 27.5 ms` |

## Hardware Verification — AYN Odin 3 (`Adreno 830`, Android 15, `0.1.0` Baseline)

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

- **Multiplayer / Steamworks:** Stubbed offline (`runtime/src/steam.c`); singleplayer (`Normal`, `Hard`, `Hard Survival`, `Peaceful`) works fully offline.
- **GPU Coverage:** Verified on Qualcomm Adreno 730 and Adreno 830; Mali and Xclipse GPUs are untested.
