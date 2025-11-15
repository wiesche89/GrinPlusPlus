# =========================
# syntax=docker/dockerfile:1.4
ARG TARGETARCH
ARG DOCKCROSS_PLATFORM=linux/amd64
ARG GRINPP_REPO=https://github.com/wiesche89/GrinPlusPlus.git
ARG GRINPP_REF=master

# =========================
# Stage 1: Controller-Builder (Qt)
# =========================
FROM ubuntu:24.04 AS builder-controller

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools qt6-websockets-dev \
    libqt6sql6 libqt6sql6-sqlite \
    build-essential git ca-certificates \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --branch main --single-branch https://github.com/wiesche89/grin-node-controller.git .
RUN qmake6 grin-node-controller.pro && make -j"$(nproc)"

# =========================
# Stage 2: Grin-Builder (Rust via rustup)
# =========================
FROM ubuntu:24.04 AS builder-grin

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config cmake clang git curl ca-certificates openssl \
    libssl-dev libcurl4-openssl-dev \
    libncurses-dev libncursesw6 \
 && rm -rf /var/lib/apt/lists/*

# Rust via rustup
ENV RUST_VERSION=stable
RUN set -eux; \
    curl -fsSL https://sh.rustup.rs -o /tmp/rustup.sh; \
    sh /tmp/rustup.sh -y --no-modify-path --profile minimal --default-toolchain "${RUST_VERSION}"; \
    echo 'export PATH=/root/.cargo/bin:$PATH' >/etc/profile.d/cargo.sh
ENV PATH="/root/.cargo/bin:${PATH}"

RUN rustc -V && cargo -V && curl -I https://index.crates.io/config.json

ARG GRIN_REPO=https://github.com/wiesche89/grin.git
ARG GRIN_REF=master
WORKDIR /build/grin
RUN git clone --single-branch "${GRIN_REPO}" . && git checkout "${GRIN_REF}"

RUN cargo build -p grin --release \
 && strip target/release/grin \
 && install -D -m 0755 target/release/grin /out/grin

# =========================
# Stage 3a: Grin++-Builder ARM64 (dockcross)
# =========================
FROM --platform=${DOCKCROSS_PLATFORM} dockcross/linux-arm64 AS builder-grinpp-arm64

ARG GRINPP_REPO
ARG GRINPP_REF

ENV DEBIAN_FRONTEND=noninteractive
ENV VCPKG_FORCE_SYSTEM_BINARIES=1 \
    VCPKG_OVERLAY_PORTS=/work/vcpkg/custom_ports \
    VCPKG_OVERLAY_TRIPLETS=/work/vcpkg/custom_triplets \
    VCPKG_DEFAULT_TRIPLET=arm64-unknown-linux-static \
    VCPKG_FEATURE_FLAGS=-compilertracking \
    VCPKG_BUILD_TYPE=release \
    VCPKG_MAX_CONCURRENCY=8 \
    GRINPP_CHAINLOAD_TOOLCHAIN=/usr/xcc/aarch64-unknown-linux-gnu/Toolchain.cmake

WORKDIR /
RUN rm -rf /work && \
    git clone "${GRINPP_REPO}" /work && \
    git -C /work checkout "${GRINPP_REF}" && \
    git config --global --add safe.directory /work && \
    git -C /work submodule update --init --recursive
WORKDIR /work

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

ARG VCPKG_REF=2024.09.30
RUN git clone https://github.com/microsoft/vcpkg /vcpkg && \
    cd /vcpkg && git checkout ${VCPKG_REF} && \
    ./bootstrap-vcpkg.sh -disableMetrics

RUN git config --global --add safe.directory /work && \
    git -C /work submodule update --init --recursive

RUN env -u VCPKG_OVERLAY_PORTS \
    /vcpkg/vcpkg install --debug \
      --overlay-triplets=${VCPKG_OVERLAY_TRIPLETS} \
      --triplet ${VCPKG_DEFAULT_TRIPLET} \
      rocksdb

RUN /vcpkg/vcpkg install --debug \
      --overlay-ports=${VCPKG_OVERLAY_PORTS} \
      --overlay-triplets=${VCPKG_OVERLAY_TRIPLETS} \
      --triplet ${VCPKG_DEFAULT_TRIPLET} \
      @/work/vcpkg/packages.txt

RUN rm -rf /work/build && mkdir -p /work/build && \
    cmake -S /work -B /work/build -G Ninja \
        -D CMAKE_BUILD_TYPE=Release \
        -D GRINPP_TESTS=OFF \
        -D GRINPP_TOOLS=ON \
        -D CMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -D VCPKG_TARGET_TRIPLET=${VCPKG_DEFAULT_TRIPLET} && \
    cmake --build /work/build --config Release --parallel && \
    cmake --build /work/build --config Release --target GrinNode && \
    install -Dm755 /work/bin/Release/GrinNode /out/grinpp && \
    ls -lh /out/grinpp

# =========================
# Stage 3b: Grin++-Builder AMD64 (Ubuntu + vcpkg)
# =========================
FROM ubuntu:24.04 AS builder-grinpp-amd64

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git curl ca-certificates openssl pkg-config \
    ninja-build unzip zip tar xz-utils python3 \
    autoconf automake libtool m4 linux-libc-dev \
    libreadline-dev file \
 && rm -rf /var/lib/apt/lists/*

ENV CMAKE_VERSION=3.30.3
ENV PATH="/usr/local/bin:${PATH}"
RUN set -eux; \
    curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" -o /tmp/cmake.tgz; \
    tar -C /opt -xzf /tmp/cmake.tgz; \
    CMAKE_DIR="$(tar tzf /tmp/cmake.tgz | head -1 | cut -d/ -f1)"; \
    ln -sf "/opt/${CMAKE_DIR}/bin/cmake" /usr/local/bin/cmake; \
    ln -sf "/opt/${CMAKE_DIR}/bin/ctest" /usr/local/bin/ctest; \
    cmake --version

ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_FEATURE_FLAGS=-manifests,-compilertracking
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
ENV VCPKG_DEFAULT_BINARY_CACHE=/opt/vcpkg_cache
ENV VCPKG_BINARY_SOURCES="default,readwrite"
ENV VCPKG_MAX_CONCURRENCY=8
ENV VCPKG_BUILD_TYPE=release

RUN mkdir -p /opt/vcpkg_cache && chmod 777 /opt/vcpkg_cache

RUN git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" \
 && git -C "${VCPKG_ROOT}" checkout 2024.09.30 \
 && "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

ARG GRINPP_REPO
ARG GRINPP_REF
WORKDIR /build
RUN git clone "${GRINPP_REPO}" grinpp && \
    git -C grinpp checkout "${GRINPP_REF}" && \
    git config --global --add safe.directory /build/grinpp && \
    git -C grinpp submodule update --init --recursive
WORKDIR /build/grinpp

ENV VCPKG_OVERLAY_PORTS=/build/grinpp/vcpkg/custom_ports
ENV VCPKG_OVERLAY_TRIPLETS=/build/grinpp/vcpkg/custom_triplets
ENV VCPKG_DEFAULT_TRIPLET=x64-linux

RUN env -u VCPKG_OVERLAY_PORTS -u VCPKG_OVERLAY_TRIPLETS \
      "${VCPKG_ROOT}/vcpkg" install "rocksdb:${VCPKG_DEFAULT_TRIPLET}" --clean-after-build

RUN BASE_INSTALLS=" \
      libsodium:${VCPKG_DEFAULT_TRIPLET} \
      zlib:${VCPKG_DEFAULT_TRIPLET} \
      civetweb:${VCPKG_DEFAULT_TRIPLET} \
      asio:${VCPKG_DEFAULT_TRIPLET} \
      fmt:${VCPKG_DEFAULT_TRIPLET} \
      catch2:${VCPKG_DEFAULT_TRIPLET} \
      openssl:${VCPKG_DEFAULT_TRIPLET} \
      mio:${VCPKG_DEFAULT_TRIPLET} \
      libuuid:${VCPKG_DEFAULT_TRIPLET} \
    "; \
    "${VCPKG_ROOT}/vcpkg" install $BASE_INSTALLS \
      --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
      --clean-after-build && \
    "${VCPKG_ROOT}/vcpkg" install "roaring:${VCPKG_DEFAULT_TRIPLET}" \
      --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
      --clean-after-build && \
    ( "${VCPKG_ROOT}/vcpkg" install "minizip:${VCPKG_DEFAULT_TRIPLET}" \
        --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
        --clean-after-build \
      || \
      "${VCPKG_ROOT}/vcpkg" install "minizip-ng:${VCPKG_DEFAULT_TRIPLET}" \
        --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
        --clean-after-build ) && \
    "${VCPKG_ROOT}/vcpkg" install "secp256k1-zkp:${VCPKG_DEFAULT_TRIPLET}" \
      --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
      --clean-after-build

RUN cmake -S /build/grinpp -B /build/grinpp/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
      -DVCPKG_TARGET_TRIPLET=${VCPKG_DEFAULT_TRIPLET} \
      -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
      -DBUILD_TESTING=OFF \
      -DGRINPP_TESTS=OFF \
      -DGRINPP_TOOLS=OFF && \
    cmake --build /build/grinpp/build --parallel && \
    install -Dm755 /build/grinpp/bin/Release/GrinNode /out/grinpp && \
    ls -lh /out/grinpp

# =========================
# Stage 4a: Select Grin++ builder per Architektur
# =========================
FROM builder-grinpp-${TARGETARCH} AS builder-grinpp-selected

# =========================
# Stage 4b: Runtime
# =========================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV CONTROLLER_PORT=8080

RUN apt-get update && apt-get install -y --no-install-recommends \
    libqt6core6 libqt6network6 libqt6sql6 libqt6sql6-sqlite \
    libstdc++6 libgcc-s1 zlib1g libssl3 ca-certificates \
    libuuid1 uuid-runtime \
    xz-utils tar unzip dpkg curl \
    libqt6gui6 libqt6widgets6 \
 && rm -rf /var/lib/apt/lists/*

# Grin (Rust)
WORKDIR /opt/nodes/grin-rust
COPY --from=builder-grin /out/grin /opt/nodes/grin-rust/grin
RUN chmod 0755 /opt/nodes/grin-rust/grin

# Grin++ (aus Builder-Stage)
WORKDIR /opt/nodes/grinpp
COPY --from=builder-grinpp-selected /out/grinpp /opt/nodes/grinpp/grin
RUN chmod 0755 /opt/nodes/grinpp/grin

# Pfade fuer Controller
ENV GRIN_RUST_BIN="/opt/nodes/grin-rust/grin" \
    GRIN_RUST_DATADIR="/data/grin-rust" \
    GRINPP_BIN="/opt/nodes/grinpp/grin" \
    GRINPP_DATADIR="/data/grinpp"

# Controller
WORKDIR /opt/app
COPY --from=builder-controller /src/grin-node-controller /opt/app/grin-node-controller
RUN chmod +x /opt/app/grin-node-controller

# User + Datenverzeichnisse
RUN useradd -m appuser && mkdir -p /data/grin-rust /data/grinpp && chown -R appuser:appuser /opt /data
USER appuser

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s \
  CMD curl -fsS "http://127.0.0.1:${CONTROLLER_PORT}/health" || exit 1

CMD ["/opt/app/grin-node-controller"]
