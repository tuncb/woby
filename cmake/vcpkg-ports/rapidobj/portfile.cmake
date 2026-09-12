# Pin the benchmarked revision with fixes covered by Woby's OBJ loader tests.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO guybrush77/rapidobj
    REF fe4c779314b0daed19530c8b5aa323c789d08f81
    SHA512 a084a6eb761468143fbd3128ddf1cda80863b419a324c85a5b9143c0f26b1b45a1290af6cd0afaf74b56bc9345e28b3797f7e353bd57bec065fb65145816e2e1
    PATCHES fix-paths-and-winding.patch
)

set(VCPKG_BUILD_TYPE release) # Header-only library.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DRAPIDOBJ_CMAKE_DIR=share
        -DRAPIDOBJ_BuildTests=OFF
        -DRAPIDOBJ_BuildTools=OFF
        -DRAPIDOBJ_BuildExamples=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME RapidObj CONFIG_PATH share/RapidObj)
vcpkg_fixup_pkgconfig()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
