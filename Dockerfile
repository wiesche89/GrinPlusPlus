# ============================================================
# GrinPlusPlus ARM64 Build (vcpkg arm64-linux) + static Tor
# Basis: dockcross/linux-arm64
# ============================================================
FROM dockcross/linux-arm64

ENV DEBIAN_FRONTEND=noninteractive
ENV VCPKG_FORCE_SYSTEM_BINARIES=1 \
    VCPKG_OVERLAY_PORTS=/work/vcpkg/custom_ports \
    VCPKG_OVERLAY_TRIPLETS=/work/vcpkg/custom_triplets \
    VCPKG_DEFAULT_TRIPLET=arm64-unknown-linux-static \
    VCPKG_FEATURE_FLAGS=-compilertracking

# ------------------------------------------------------------
# Projektquellen ins Image kopieren
# Erwartete Struktur:
#   /work/CMakeLists.txt
#   /work/vcpkg/packages.txt
#   /work/vcpkg/custom_ports/...
# ------------------------------------------------------------
WORKDIR /work
COPY . /work/

# ------------------------------------------------------------
# System-Pakete für vcpkg-Ports, CMake, Autotools, Tor-Build
# ------------------------------------------------------------
RUN apt-get update && apt-get upgrade -y && \
    apt-get install -y --no-install-recommends \
        git curl unzip tar \
        build-essential pkg-config \
        cmake ninja-build \
        autoconf automake libtool m4 \
        libpthread-stubs0-dev \
        autopoint po4a \
        ca-certificates \
    && apt-get autoremove -y && \
    rm -rf /var/lib/apt/lists/*

# ------------------------------------------------------------
# vcpkg holen & bootstrappen
# Commit wie im CI (lukka/run-vcpkg vcpkgGitCommitId)
# ------------------------------------------------------------
ARG VCPKG_REF=2024.09.30
RUN git clone https://github.com/microsoft/vcpkg /vcpkg && \
    cd /vcpkg && git checkout ${VCPKG_REF} && \
    ./bootstrap-vcpkg.sh -disableMetrics

# RocksDB nutzt den Upstream-Port (ohne Overlays), sonst schlagen die
# Arch-spezifischen CRC-Optimierungen fehl.
RUN env -u VCPKG_OVERLAY_PORTS \
    /vcpkg/vcpkg install --debug \
      --overlay-triplets=${VCPKG_OVERLAY_TRIPLETS} \
      --triplet ${VCPKG_DEFAULT_TRIPLET} \
      rocksdb

# ------------------------------------------------------------
# vcpkg Dependencies wie im CI
#   - nutzt /work/vcpkg/packages.txt
#   - nutzt /work/vcpkg/custom_ports als Overlay
#   - nutzt /work/vcpkg/custom_triplets/arm64-unknown-linux-static
# ------------------------------------------------------------
RUN /vcpkg/vcpkg install --debug \
      --overlay-ports=${VCPKG_OVERLAY_PORTS} \
      --overlay-triplets=${VCPKG_OVERLAY_TRIPLETS} \
      --triplet ${VCPKG_DEFAULT_TRIPLET} \
      @/work/vcpkg/packages.txt

# ------------------------------------------------------------
# GrinNode / GrinPlusPlus Backend bauen (CMake + Ninja)
# entspricht grob lukka/run-cmake im CI, nur für arm64-linux
# ------------------------------------------------------------
RUN rm -rf /work/build && mkdir -p /work/build && \
    cmake -S /work -B /work/build -G Ninja \
        -D CMAKE_BUILD_TYPE=Release \
        -D GRINPP_TESTS=OFF \
        -D GRINPP_TOOLS=ON \
        -D CMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -D VCPKG_TARGET_TRIPLET=${VCPKG_DEFAULT_TRIPLET} && \
    cmake --build /work/build --config Release --parallel && \
    cmake --build /work/build --config Release --target GrinNode && \
    install -Dm755 /work/build/bin/GrinNode /work/bin/Release/GrinNode && \
    echo ">>> Build output:" && \
    ls -lh /work/bin || true && \
    ls -lh /work/bin/Release || true && \
    file /work/bin/Release/GrinNode || true
