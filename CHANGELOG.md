# Changelog

## 0.2.1 – Physical Controller & NGUI Menu Fix

- **Direct Android Hardware Controller Driver (`ForestActivity.java`, `android_main.c`):**
  - Captures physical gamepads (`dispatchKeyEvent` + `dispatchGenericMotionEvent`) directly for `KEYCODE_BUTTON_*`, `KEYCODE_DPAD_*`, `AXIS_X/Y/Z/RZ/RX/RY`, `AXIS_LTRIGGER`/`AXIS_BRAKE`, `AXIS_RTRIGGER`/`AXIS_GAS`, and `AXIS_HAT_X/Y` with smooth `0.12` deadzone into XInput slot 0, bypassing SDL3's strict `SOURCE_JOYSTICK` filter and duplicated trigger axis mapping (e.g., AYN Odin 3 built-in `Xbox Wireless Controller`).
  - Disables duplicate SDL3 gamepad events once the Java hardware pad driver is active.
- **Full Controller Menu Navigation (`user32.c`, `k32_file.c`, `tf_compat.c`, `xinput.c`):**
  - Exposes our virtual XInput gamepad (`VID_045E&PID_028E&IG_00`) through RawInput (`GetRawInputDeviceList` / `GetRawInputDeviceInfoW`) and implements Unity 5.6's HID button pipeline (`CreateFileW`, `ReadFile` → `ERROR_IO_PENDING`, `GetOverlappedResult` → `ERROR_IO_INCOMPLETE`/report, `HidD_GetPreparsedData`, `HidD_GetProductString`, `HidP_GetCaps`, `HidP_GetButtonCaps`, `HidP_GetData`, `HidP_MaxDataListLength`).
  - Fixes `UnityEngine.Input.GetKeyDown(KeyCode.JoystickButton0..9)` used by NGUI `UICamera` so `A` (Submit), `B` (Back), `Y` (Skip Intro), and `Start` (Pause / Resume) work natively across all menus without touch taps.
- **Auto-Hiding On-Screen Pad & Menu Touch Polish (`PadView.java`, `android_main.c`, `user32.c`):**
  - Automatically hides the on-screen `PadView` when a physical controller is detected at startup or used (still toggleable via the top-right `Pad` button).
  - Adds an optional Right-Stick virtual cursor ring in menus (auto-hides on Left-Stick / D-Pad input) and a 1-frame hover settle + 1 px nudge so switching from controller back to touch clicks reliably on the first tap.
- **Verified (`Samsung Galaxy Tab S8 Ultra`, `0.2.1`):**
  - 100% controller-only flow verified end-to-end: Title Screen → `SINGLE PLAYER (A)` → `NEW GAME (A)` → `B` (`BACK`) → `SINGLE PLAYER (A)` → `NEW GAME (A)` → `NORMAL (A)` → `ForestMain_v08` load → `Y` crash-intro skip → `Start` Pause menu → `CONTINUE (A)` resume; FMOD audio `0 of 1000 blocks short`.

## 0.2.0 – 3.3×–3.6× Open-World Performance Overhaul & Crisp Mobile Preset

- **99.3% `-O2` Coverage (`614.5 / 619` units, APK `79.8 MB → 64 MB`):**
  - Added a lock-free Boehm GC guest stack root scanner (`w64_gc_push_guest_roots` via `GC_push_other_roots` in `core.c` / `mono_bridge.c`), eliminating the `-O2` Mono deserialization regression and freeing all 8 units `TheForest/recomp_funcs_0038..0045` to `-O2`.
  - Narrowed the NGUI/Unity `-O0` window from 8 units (`0017..0024`) down to **4.5 units** (`0019..0022` + cold tail of `0018` from `L_000140237E10` via `#pragma clang optimize off` in `tools/patch_gen.py`), dropping main-thread `-O0` time to **3.6%**.
- **Lock-Free Linux `SYS_futex` Semaphores & Mobile Worker Cap:**
  - Replaced `pthread_mutex`/`pthread_cond` on `H_SEM` (`runtime/src/k32_sync.c`) with lock-free `_Atomic int32_t` + direct `FUTEX_WAIT_PRIVATE`/`FUTEX_WAKE_PRIVATE` + adaptive spin.
  - Defaulted logical CPU count on Android to `4` (overridable via `W64_NCPU`) to eliminate big.LITTLE scheduler contention, cutting `ReleaseSemaphore` overhead from `19.1%` to `3.4%` and halving kernel `sys` CPU time.
- **Native ARM64 NEON & Intrinsic Hot-Path Replacements (`tools/patch_gen.py`):**
  - Replaced lifted x86-64 4-bone skinned mesh loops (`L_000140426D00`, `L_000140427B70`) with native ARM64 NEON (`vfmaq_f32`/`vfmaq_laneq_f32`/`vbslq_f32`).
  - Replaced lifted FMOD Vorbis Huffman & 128-bit bitstream jump-table loops (`L_0001800055A0`, `L_0001800022D0`) with native `__builtin_bitreverse32` and `unsigned __int128` shifts.
  - Replaced `GetGfxDevice()` (`L_0001402C1C00`) with a direct TLS read (`w64_tls_get`), plus native CRT `memcpy`/`memmove`/`memset`/`memcmp`/`fmodf`/`fmod`.
- **Runtime & Compiler Fast Paths:**
  - Enabled `-O3 -march=armv8.2-a` on Mono Boehm, `libmain.so`, and DXVK; added `HT_BITS=19` dispatch table, constant-folded `GVA(abs)` + hardware `op_bsf`/`op_bsr`, `NK_REG4`/`NK_REG8` + 64-entry `t_kcache` COM fast path, and `mono_bridge` `host_fn`/`is_simple` icall fast path (98% hit rate).
- **Crisp Mobile Graphics Preset (`QualityPreset.java` + Launcher Button):**
  - Automatically seeds `registry.dat` on first install (or once on upgrade from `0.1.0`) with full-resolution textures (`FullRes`), `FXAA`, mobile-tuned shadows/draw distance, and `LowQualityPhysics=1`, while preserving all user changes made under `OPTIONS → GRAPHICS` and offering a one-tap preset button in `Launcher`.
- **Verified Performance (`Samsung Galaxy Tab S8 Ultra`, Snapdragon 8 Gen 1 / Adreno 730, `1280×800`):**
  - Title Screen (3D Cave): **60.4 fps** (`p50 = 16.5–16.7 ms`, `0` frames ≥ 30 ms).
  - Open-World `ForestMain_v08` (PC Medium, pure-CPU comparison): `9.8 fps` (`102.1 ms`) → **19–21 fps** (`47.9–50.1 ms`, **2.13× pure-CPU speedup**).
  - Open-World `ForestMain_v08` (out-of-the-box `0.2.0` Crisp Mobile Preset): **30.9–34.2 fps** (`p50 = 27.5–32.6 ms`, `p95 = 37.2–38.4 ms`, `gp = 0 ms`, **3.3×–3.6× speedup**), `0` exceptions, same-PID background/resume verified.

## 0.1.0 – Initial Android Static Recompilation (`arm64-v8a`)

- **5-Module Static x86-64 → ARM64 Recompilation:** Lifted **246,400 functions** across `TheForest.exe` (231,740), `fmodstudio.dll` (9,321), `fmod.dll` (4,196), `CSteamworks.dll` (981), and `UnityImageNativePlugin.dll` (162) into 619 C units compiled natively into `libmain.so` (603/619 units = 97.4% at `-O2 -march=armv8.2-a`, 16 units at `-O0` for Unity/NGUI stability; APK `79.8 MB`).
- **Native ARM64 Unity Mono 4.8 (`unity-5.6-mbe`):** Cross-compiled Boehm-GC Mono runtime + ARM64 Win64-ABI trampolines and 13 bundled open-source BCL DLLs, including a `Thread.Start(object)` compatibility fix for `AstarPath` / `mutantController` (`0` exceptions on world load).
- **Direct3D 11 → Vulkan 1.1 (DXVK Native 1.9.2b):** Android WSI bridge, `dxvk-async` (2 compiler threads), and persistent Adreno `VkPipelineCache` (`vk_pipeline.cache`) + `app_process64.dxvk-cache`.
- **FMOD Studio Audio (WASAPI → SDL3):** Native `IAudioClient` / `IAudioRenderClient` bridge to 48 kHz stereo SDL3 output (`0 of 1000 blocks short`).
- **Controls & Android Launcher:** Physical XInput gamepad support, toggleable on-screen `PadView` with touch-look and menu tap-to-click, resolution scaling toggle (`1280×720` default or native), SAF folder import, and automatic ZIP64 companion data-pack extraction (`theforest-data-0.1.0.zip`, `1.99 GB`).
- **AYN Odin 3 (`Adreno 830`, Android 15, `1280×720`) Verification:**
  - Title Screen (3D Cave): **59.7–60.3 fps** (`p50 = 16.5–16.8 ms`, `p95 = 19.0–20.3 ms`, `0` frames ≥ 30 ms steady-state, `1.11 GB` PSS).
  - Open-World `ForestMain_v08`: `7.8 s` load + `9.8 s` activation (`0` exceptions), **14.1–16.3 fps** outdoor gameplay (`p50 = 68.1–71.7 ms`, `2.07 GB` PSS), background/resume verified with same PID.
