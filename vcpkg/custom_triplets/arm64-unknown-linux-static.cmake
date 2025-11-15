set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
# Optional chainload support: keep dockcross path for legacy images, but don't
# force it when the file is missing (e.g. native arm64 buildx runners).
set(_GRINPP_ARM_CHAINLOAD $ENV{GRINPP_CHAINLOAD_TOOLCHAIN})
if(NOT _GRINPP_ARM_CHAINLOAD)
    set(_GRINPP_ARM_CHAINLOAD "/usr/xcc/aarch64-unknown-linux-gnu/Toolchain.cmake")
endif()
if(EXISTS "${_GRINPP_ARM_CHAINLOAD}")
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${_GRINPP_ARM_CHAINLOAD}")
endif()
