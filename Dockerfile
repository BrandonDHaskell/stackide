# Dockerfile
#
# stackide CI toolchain image.
# Publishes GCC 16, LLVM/Clang, CMake and Ninja at the same absolute paths used
# by CMakePresets.json on lovelace: /opt/gcc, /opt/llvm, /opt/cmake, /opt/ninja.
#
# Build:  docker build -t ghcr.io/<owner>/stackide-toolchain:gcc16-llvm23 .
# Verify: docker run --rm <image> stackide-toolchain-check

# ---------------------------------------------------------------------------
# Stage 1: build GCC from source.
# ---------------------------------------------------------------------------
FROM debian:trixie AS gcc-build

ARG GCC=16.2.0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential wget xz-utils ca-certificates file texinfo python3 \
        libgmp-dev libmpfr-dev libmpc-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
RUN set -eux; \
    wget -q "https://sourceware.org/pub/gcc/releases/gcc-${GCC}/gcc-${GCC}.tar.xz"; \
    tar xf "gcc-${GCC}.tar.xz"; \
    mkdir gcc-build && cd gcc-build; \
    "../gcc-${GCC}/configure" \
        --prefix="/opt/gcc-${GCC}" \
        --enable-languages=c,c++ \
        --disable-multilib \
        --disable-nls; \
    make -j"$(nproc)"; \
    make install-strip

# ---------------------------------------------------------------------------
# Stage 2: runtime image.
# ---------------------------------------------------------------------------
FROM debian:trixie

ARG GCC=16.2.0
ARG LLVM=23.1.1
ARG CMAKE=4.4.3
ARG NINJA=1.13.1

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git xz-utils unzip binutils libc6-dev libatomic1 \
        pkg-config gdb \
        libgmp10 libmpfr6 libmpc3 zlib1g libzstd1 \
        \
        # Vulkan runtime. lavapipe is the software ICD that makes the
        # --offscreen smoke test runnable on a runner with no GPU.
        libvulkan1 mesa-vulkan-drivers vulkan-tools \
        \
        # SDL3 build dependencies. Required whether SDL arrives via
        # FetchContent or the system package; see docs/toolchain.md.
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
        libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols \
        libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libdrm-dev libgbm-dev \
        libasound2-dev libpulse-dev libudev-dev \
        \
        # GLSL to SPIR-V compiler for M1's shader-embedding build step. A leaf
        # build tool, not a pinned toolchain component like GCC/LLVM/CMake/
        # Ninja above: apt-managed, version recorded (not pinned) below.
        glslang-tools \
    && rm -rf /var/lib/apt/lists/*

COPY --from=gcc-build /opt/gcc-${GCC} /opt/gcc-${GCC}

RUN set -eux; \
    curl -fsSL -o /tmp/llvm.tar.xz \
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM}/LLVM-${LLVM}-Linux-X64.tar.xz"; \
    tar xf /tmp/llvm.tar.xz -C /opt; \
    mv "/opt/LLVM-${LLVM}-Linux-X64" "/opt/llvm-${LLVM}"; \
    \
    curl -fsSL -o /tmp/cmake.tar.gz \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE}/cmake-${CMAKE}-linux-x86_64.tar.gz"; \
    tar xf /tmp/cmake.tar.gz -C /opt; \
    mv "/opt/cmake-${CMAKE}-linux-x86_64" "/opt/cmake-${CMAKE}"; \
    \
    curl -fsSL -o /tmp/ninja.zip \
      "https://github.com/ninja-build/ninja/releases/download/v${NINJA}/ninja-linux.zip"; \
    mkdir -p "/opt/ninja-${NINJA}/bin"; \
    unzip -q /tmp/ninja.zip -d "/opt/ninja-${NINJA}/bin"; \
    chmod +x "/opt/ninja-${NINJA}/bin/ninja"; \
    \
    ln -sfn "/opt/gcc-${GCC}"    /opt/gcc; \
    ln -sfn "/opt/llvm-${LLVM}"  /opt/llvm; \
    ln -sfn "/opt/cmake-${CMAKE}" /opt/cmake; \
    ln -sfn "/opt/ninja-${NINJA}" /opt/ninja; \
    \
    for b in cmake ctest cpack; do ln -sf "/opt/cmake/bin/$b" "/usr/local/bin/$b"; done; \
    ln -sf /opt/ninja/bin/ninja /usr/local/bin/ninja; \
    for b in clangd clang-format clang-tidy; do ln -sf "/opt/llvm/bin/$b" "/usr/local/bin/$b"; done; \
    \
    rm -rf /tmp/*

# GitHub Actions container jobs run as root over a checkout owned by another
# uid. Without this, every git invocation fails with "dubious ownership".
RUN git config --system --add safe.directory '*'

# Headless by default. The --offscreen smoke test creates no window, so the
# dummy video driver is sufficient and no X or Wayland server is needed.
ENV SDL_VIDEODRIVER=dummy

# Fail the image build, not a downstream job, when a component is missing or
# the wrong version.
RUN printf '%s\n' \
    '#!/bin/sh' \
    'set -eux' \
    '/opt/gcc/bin/g++ --version' \
    '/opt/llvm/bin/clang++ --version' \
    'cmake --version' \
    'ninja --version' \
    'clangd --version' \
    'vulkaninfo --summary' \
    'glslang --version' \
    > /usr/local/bin/stackide-toolchain-check \
    && chmod +x /usr/local/bin/stackide-toolchain-check \
    && stackide-toolchain-check

# Recorded, not pinned (see docs/buildouts/M1-textured-quad.md decision 1):
# the resolved glslang-tools version becomes part of this image's published
# tag (see .github/workflows/image.yml), so it's captured here in the build
# log rather than guessed in the Dockerfile.
RUN dpkg-query -W -f='glslang-tools version: ${Version}\n' glslang-tools