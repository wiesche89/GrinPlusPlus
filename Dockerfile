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
# Stage 3: Grin++-Builder (CMake + vcpkg, amd64 + arm64)
# =========================
FROM ubuntu:24.04 AS builder-grinpp

ENV DEBIAN_FRONTEND=noninteractive

ARG CACHE_BREAKER=2
# buildx setzt TARGETARCH automatisch (amd64, arm64, ...)
ARG TARGETARCH
ENV TARGETARCH=${TARGETARCH}

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git curl ca-certificates openssl pkg-config \
    ninja-build unzip zip tar xz-utils python3 uuid-dev \
    autoconf automake libtool m4 linux-libc-dev \
    libreadline-dev file \
 && rm -rf /var/lib/apt/lists/*

# CMake (>= 3.21 fuer vcpkg)
ENV CMAKE_VERSION=3.30.3
ENV PATH="/usr/local/bin:${PATH}"

RUN set -eux; \
    if [ "$TARGETARCH" = "arm64" ]; then CMAKE_ARCH="aarch64"; else CMAKE_ARCH="x86_64"; fi; \
    curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${CMAKE_ARCH}.tar.gz" -o /tmp/cmake.tgz; \
    tar -C /opt -xzf /tmp/cmake.tgz; \
    CMAKE_DIR="$(tar tzf /tmp/cmake.tgz | head -1 | cut -d/ -f1)"; \
    ln -sf "/opt/${CMAKE_DIR}/bin/cmake" /usr/local/bin/cmake; \
    ln -sf "/opt/${CMAKE_DIR}/bin/ctest" /usr/local/bin/ctest; \
    cmake --version

# vcpkg Basis-Setup
ENV VCPKG_ROOT=/opt/vcpkg
# -manifests wie vorher, zusaetzlich -compilertracking wie in deinem ARM64-File
ENV VCPKG_FEATURE_FLAGS=-manifests,-compilertracking
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
ENV VCPKG_DEFAULT_BINARY_CACHE=/opt/vcpkg_cache
ENV VCPKG_BINARY_SOURCES="default,readwrite"
ENV VCPKG_MAX_CONCURRENCY=4

RUN mkdir -p /opt/vcpkg_cache && chmod 777 /opt/vcpkg_cache

RUN git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" \
 && git -C "${VCPKG_ROOT}" checkout 2024.09.30 \
 && "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

# GrinPlusPlus Code aus dem Build-Kontext uebernehmen (Repo muss ausgecheckt sein)
WORKDIR /build/grinpp
COPY . /build/grinpp
RUN test -d .git || { echo "[ERROR] GrinPlusPlus muss vor dem Docker-Build via git ausgecheckt werden."; exit 1; } \
 && git config --global --add safe.directory /build/grinpp \
 && git submodule update --init --recursive

# Wichtige vcpkg-Pfade aus dem Repo
# (vcpkg/custom_ports, vcpkg/custom_triplets, vcpkg/packages.txt)
ENV VCPKG_OVERLAY_PORTS=/build/grinpp/vcpkg/custom_ports
ENV VCPKG_OVERLAY_TRIPLETS=/build/grinpp/vcpkg/custom_triplets
ENV VCPKG_PACKAGES_FILE=/build/grinpp/vcpkg/packages.txt

# Abhaengigkeiten + Build: verzweigt nach TARGETARCH
RUN set -eux; \
  if [ "$TARGETARCH" = "arm64" ]; then \
    echo '>>> [ARM64] Using triplet arm64-unknown-linux-static + packages.txt'; \
    export VCPKG_DEFAULT_TRIPLET=arm64-unknown-linux-static; \
    # RocksDB zuerst ohne Overlays, wie in deinem ARM-File
    env -u VCPKG_OVERLAY_PORTS \
      "${VCPKG_ROOT}/vcpkg" install --debug \
        --overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}" \
        --triplet "${VCPKG_DEFAULT_TRIPLET}" \
        rocksdb; \
    # Restliche Dependencies via packages.txt
    "${VCPKG_ROOT}/vcpkg" install --debug \
      --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
      --overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}" \
      --triplet "${VCPKG_DEFAULT_TRIPLET}" \
      @"${VCPKG_PACKAGES_FILE}"; \
    # CMake-Konfiguration wie in deinem ARM64 Dockerfile
    cmake -S /build/grinpp -B /build/grinpp/build -G Ninja \
        -D CMAKE_BUILD_TYPE=Release \
        -D GRINPP_TESTS=OFF \
        -D GRINPP_TOOLS=ON \
        -D CMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
        -D VCPKG_TARGET_TRIPLET=${VCPKG_DEFAULT_TRIPLET}; \
  else \
    echo '>>> [AMD64] Using triplet x64-linux + bisherige Portliste'; \
    export VCPKG_DEFAULT_TRIPLET=x64-linux; \
    TRIP=${VCPKG_DEFAULT_TRIPLET}; \
    # RocksDB ohne Overlays, wie vorher bei dir - stabil fuer amd64
    env -u VCPKG_OVERLAY_PORTS -u VCPKG_OVERLAY_TRIPLETS \
      "${VCPKG_ROOT}/vcpkg" install "rocksdb:${TRIP}" --clean-after-build; \
    BASE_INSTALLS=" \
      libsodium:${TRIP} \
      zlib:${TRIP} \
      civetweb:${TRIP} \
      asio:${TRIP} \
      fmt:${TRIP} \
      catch2:${TRIP} \
      openssl:${TRIP} \
      mio:${TRIP} \
    "; \
    "${VCPKG_ROOT}/vcpkg" install $BASE_INSTALLS \
      --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
      ${VCPKG_OVERLAY_TRIPLETS:+--overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}"} \
      --clean-after-build; \
    "${VCPKG_ROOT}/vcpkg" install "roaring:${TRIP}" \
      --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
      ${VCPKG_OVERLAY_TRIPLETS:+--overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}"} \
      --clean-after-build; \
    ( "${VCPKG_ROOT}/vcpkg" install "minizip:${TRIP}" \
        --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
        ${VCPKG_OVERLAY_TRIPLETS:+--overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}"} \
        --clean-after-build \
      || \
      "${VCPKG_ROOT}/vcpkg" install "minizip-ng:${TRIP}" \
        --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
        ${VCPKG_OVERLAY_TRIPLETS:+--overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}"} \
        --clean-after-build ); \
    "${VCPKG_ROOT}/vcpkg" install "secp256k1-zkp:${TRIP}" \
      --overlay-ports="${VCPKG_OVERLAY_PORTS}" \
      ${VCPKG_OVERLAY_TRIPLETS:+--overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}"} \
      --clean-after-build; \
    # CMake-Konfiguration wie vorher fuer amd64
    cmake -S /build/grinpp -B /build/grinpp/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
      -DVCPKG_TARGET_TRIPLET=${VCPKG_DEFAULT_TRIPLET} \
      -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
      -DBUILD_TESTING=OFF \
      -DGRINPP_TESTS=OFF \
      -DGRINPP_TOOLS=OFF; \
  fi; \
  cmake --build /build/grinpp/build --parallel

# --- Executable finden & exportieren ---
RUN set -eux; \
  BIN_PATH="/build/grinpp/bin/Release/GrinNode"; \
  echo "===== [INFO] Pruefe Binary unter: $BIN_PATH ====="; \
  if [ -x "$BIN_PATH" ]; then \
    echo "[INFO] Gefundenes Binary: $BIN_PATH"; \
    install -D -m 0755 "$BIN_PATH" /out/grinpp; \
    echo "[INFO] Kopiert nach: /out/grinpp"; \
    echo "[INFO] Groesse:"; ls -lh /out/grinpp; \
  else \
    echo "[ERROR] Binary nicht gefunden!"; \
    echo "--- Verzeichnisinhalt von /build/grinpp/bin ---"; \
    find /build/grinpp/bin -type f -perm -u+x -print || true; \
    exit 1; \
  fi

# =========================
# Stage 4: Runtime
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
COPY --from=builder-grinpp /out/grinpp /opt/nodes/grinpp/grin
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
