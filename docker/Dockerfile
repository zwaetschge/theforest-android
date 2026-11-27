# Standalone build environment for The Forest Android static recompilation.
#   docker build -t theforest-build docker/
#   docker run --rm -v "$PWD":/w -v "/path/to/The Forest":/game:ro theforest-build sh tools/build.sh /game
# Or simply run: ./build.sh "/path/to/The Forest"
#
# Contains the lifter's Python deps, clang/lld/cmake/ninja/glslang/Vulkan, autotools + mono-mcs
# for building Unity-Technologies/mono (unity-5.6-mbe) static ARM64 runtime + BCL, JDK 17, and
# the Android SDK with the exact NDK/CMake versions used by the build.
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
      clang lld cmake ninja-build make gdb git curl ca-certificates unzip xz-utils file binutils \
      python3 python3-pip pkg-config g++ openjdk-17-jdk-headless \
      autoconf automake libtool gettext bison flex mono-mcs \
      glslang-tools spirv-tools libvulkan-dev mesa-vulkan-drivers vulkan-tools \
      xvfb x11-utils imagemagick libx11-dev libxext-dev \
 && rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir --break-system-packages capstone==5.0.9 pefile==2024.8.26

ARG ANDROID=1
ENV ANDROID_SDK=/sdk ANDROID_HOME=/sdk
RUN if [ "$ANDROID" = 1 ]; then \
      mkdir -p /sdk/cmdline-tools && cd /tmp && \
      curl -sSL -o clt.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip && \
      unzip -q clt.zip && mv cmdline-tools /sdk/cmdline-tools/latest && rm clt.zip && \
      yes | /sdk/cmdline-tools/latest/bin/sdkmanager --licenses >/dev/null && \
      /sdk/cmdline-tools/latest/bin/sdkmanager "platforms;android-35" "build-tools;35.0.0" \
        "ndk;27.3.13750724" "cmake;3.31.6" "platform-tools" >/dev/null; \
    fi
ENV PATH=/sdk/platform-tools:$PATH
WORKDIR /w
