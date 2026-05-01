if(NOT UUID_ROOT AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(UUID_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

find_path(UUID_INCLUDE_DIR
    NAMES uuid/uuid.h
    PATHS
        ${UUID_ROOT}
        ${UUID_ROOT}/include
        /usr/include
        /usr/local/include
)

find_library(UUID_LIBRARY
    NAMES uuid
    PATHS
        ${UUID_ROOT}
        ${UUID_ROOT}/lib
        /usr/lib
        /usr/local/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(UUID
    REQUIRED_VARS UUID_LIBRARY UUID_INCLUDE_DIR)

if(UUID_FOUND AND NOT TARGET UUID::UUID)
    add_library(UUID::UUID UNKNOWN IMPORTED)
    set_target_properties(UUID::UUID PROPERTIES
        IMPORTED_LOCATION "${UUID_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${UUID_INCLUDE_DIR}"
    )
endif()
