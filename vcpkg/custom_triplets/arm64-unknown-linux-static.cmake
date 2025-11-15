set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
# Force vcpkg to pick the dockcross toolchain so CMake gets the correct compiler paths.
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "/usr/xcc/aarch64-unknown-linux-gnu/Toolchain.cmake")
