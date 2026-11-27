# Third-Party Notices

This repository contains only original runtime, bridge, build, and Android wrapper code.
No game assets, binaries, or proprietary data from *The Forest* are included.

The build fetches and links the following open-source third-party projects (see `tools/fetch_deps.sh`):

- **pcrecomp** — MIT License — <https://github.com/sp00nznet/pcrecomp>  
  Pinned commit `efa71c9db36d91728d00f1077a1a5d0530d9057f` with `tools/patches/pcrecomp-theforest.patch`.
- **DXVK Native** — zlib License — <https://github.com/Joshua-Ashton/dxvk-native>  
  Pinned tag `native-1.9.2b` with Android WSI and pipeline-cache adaptations in `gfx/dxvk/prepare.py`.
- **Unity-Technologies/mono (`unity-5.6-mbe`)** — MIT / LGPL-2.1 (runtime & Boehm GC) — <https://github.com/Unity-Technologies/mono>  
  Pinned commit `1a11b91fe95a72b1d5e169b1c2596489ca8c8b34` with `tools/patches/mono-unity56-android.patch`.
- **SDL 3.2.12** — zlib License — <https://github.com/libsdl-org/SDL>

*The Forest* is © Endnight Games Ltd. You must provide your own legally purchased PC copy to build and run this port.
