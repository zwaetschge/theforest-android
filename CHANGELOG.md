# Changelog

## 0.1.0 – Initial Android Static Recompilation (`arm64-v8a`)

- **5-Module Static x86-64 → ARM64 Recompilation:** Lifted **246,400 functions** across `TheForest.exe` (231,740), `fmodstudio.dll` (9,321), `fmod.dll` (4,196), `CSteamworks.dll` (981), and `UnityImageNativePlugin.dll` (162) into 619 C units compiled natively into `libmain.so` (603/619 units = 97.4% at `-O2 -march=armv8.2-a`, 16 units at `-O0` for Unity/NGUI stability; APK `79.8 MB`).
- **Native ARM64 Unity Mono 4.8 (`unity-5.6-mbe`):** Cross-compiled Boehm-GC Mono runtime + ARM64 Win64-ABI trampolines and 13 bundled open-source BCL DLLs, including a `Thread.Start(object)` compatibility fix for `AstarPath` / `mutantController` (`0` exceptions on world load).
- **Direct3D 11 → Vulkan 1.1 (DXVK Native 1.9.2b):** Android WSI bridge, `dxvk-async` (2 compiler threads), and persistent Adreno `VkPipelineCache` (`vk_pipeline.cache`) + `app_process64.dxvk-cache`.
- **FMOD Studio Audio (WASAPI → SDL3):** Native `IAudioClient` / `IAudioRenderClient` bridge to 48 kHz stereo SDL3 output (`0 of 1000 blocks short`).
- **Controls & Android Launcher:** Physical XInput gamepad support, toggleable on-screen `PadView` with touch-look and menu tap-to-click, resolution scaling toggle (`1280×720` default or native), SAF folder import, and automatic ZIP64 companion data-pack extraction (`theforest-data-0.1.0.zip`, `1.99 GB`).
- **AYN Odin 3 (`Adreno 830`, Android 15, `1280×720`) Verification:**
  - Title Screen (3D Cave): **59.7–60.3 fps** (`p50 = 16.5–16.8 ms`, `p95 = 19.0–20.3 ms`, `0` frames ≥ 30 ms steady-state, `1.11 GB` PSS).
  - Open-World `ForestMain_v08`: `7.8 s` load + `9.8 s` activation (`0` exceptions), **14.1–16.3 fps** outdoor gameplay (`p50 = 68.1–71.7 ms`, `2.07 GB` PSS), background/resume verified with same PID.
