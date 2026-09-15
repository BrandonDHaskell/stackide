FROM debian:trixie AS gcc-build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential wget xz-utils ca-certificates file texinfo python3 \
    libgmp-dev libmpfr-dev libmpc-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /tmp
RUN wget -q https://sourceware.org/pub/gcc/releases/gcc-16.2.0/gcc-16.2.0.tar.xz \
    && tar xf gcc-16.2.0.tar.xz && mkdir gcc-build && cd gcc-build \
    && ../gcc-16.2.0/configure --prefix=/opt/gcc-16.2 --enable-languages=c,c++ --disable-multilib \
    && make -j"$(nproc)" && make install

FROM debian:trixie
ARG LLVM=23.1.1
ARG CMAKE=4.4.3
ARG NINJA=1.13.1
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl git xz-utils libgmp10 binutils libatomic1 \
    libmpfr6 libmpc3 libc6-dev && rm -rf /var/lib/apt/lists/*
COPY --from=gcc-build /opt/gcc-16.2 /opt/gcc-16.2
RUN set -eux; \
    curl -fsSL -o /tmp/llvm.tar.xz \
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM}/LLVM-${LLVM}-Linux-X64.tar.xz"; \
    tar xf /tmp/llvm.tar.xz -C /opt && mv "/opt/LLVM-${LLVM}-Linux-X64" "/opt/llvm-${LLVM}"; \
    curl -fsSL -o /tmp/cmake.tar.gz \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE}/cmake-${CMAKE}-linux-x86_64.tar.gz"; \
    tar xf /tmp/cmake.tar.gz -C /opt && mv "/opt/cmake-${CMAKE}-linux-x86_64" "/opt/cmake-${CMAKE}"; \
    curl -fsSL -o /tmp/ninja.zip \
      "https://github.com/ninja-build/ninja/releases/download/v${NINJA}/ninja-linux.zip"; \
    mkdir -p "/opt/ninja-${NINJA}/bin" && cd "/opt/ninja-${NINJA}/bin" \
      && busybox unzip /tmp/ninja.zip 2>/dev/null || (apt-get update && apt-get install -y unzip && unzip /tmp/ninja.zip); \
    chmod +x "/opt/ninja-${NINJA}/bin/ninja"; \
    ln -sfn "/opt/gcc-16.2" /opt/gcc; \
    ln -sfn "/opt/llvm-${LLVM}" /opt/llvm; \
    ln -sfn "/opt/cmake-${CMAKE}" /opt/cmake; \
    ln -sfn "/opt/ninja-${NINJA}" /opt/ninja; \
    for b in cmake ctest cpack; do ln -sf /opt/cmake/bin/$b /usr/local/bin/$b; done; \
    ln -sf /opt/ninja/bin/ninja /usr/local/bin/ninja; \
    for b in clangd clang-format clang-tidy; do ln -sf /opt/llvm/bin/$b /usr/local/bin/$b; done; \
    rm -rf /tmp/*