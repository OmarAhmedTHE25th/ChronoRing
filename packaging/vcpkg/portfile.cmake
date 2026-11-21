vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO OmarAhmedTHE25th/ChronoRing
    REF "v${VERSION}"
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCHRONORING_BUILD_TESTS=OFF
        -DCHRONORING_BUILD_BENCHMARKS=OFF
        -DCHRONORING_BUILD_FUZZERS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME ChronoRing CONFIG_PATH lib/cmake/ChronoRing)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
