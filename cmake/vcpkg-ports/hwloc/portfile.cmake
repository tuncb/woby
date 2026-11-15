# Use the upstream Windows CMake build with clang-cl. The Autotools Windows
# utility build adds MinGW-only flags such as -mwindows.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO open-mpi/hwloc
    REF "hwloc-${VERSION}"
    SHA512 1ed47955d4a3ecf66636f1c5a89648ef055aa4f26fac9b9bc64d6f7715d671dc823337ebf38df53d60b50d697eccadfbd48d28b4540a5153c59d7eecd347f91c
    PATCHES
        fix_shared_win_build.patch
        stdout_fileno.patch
)

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}/contrib/windows-cmake"
        OPTIONS -DHWLOC_ENABLE_TESTING=OFF)
    vcpkg_cmake_install()
    vcpkg_copy_pdbs()
    vcpkg_copy_tools(TOOL_NAMES hwloc-bind hwloc-calc hwloc-diff hwloc-distrib
        hwloc-gather-cpuid hwloc-info hwloc-patch lstopo-no-graphics lstopo lstopo-win
        AUTO_CLEAN)
    foreach(configuration IN ITEMS "" "/debug")
        if(configuration STREQUAL "/debug" AND VCPKG_BUILD_TYPE)
            continue()
        endif()
        set(pkgconfig_dir "${CURRENT_PACKAGES_DIR}${configuration}/lib/pkgconfig")
        file(MAKE_DIRECTORY "${pkgconfig_dir}")
        file(WRITE "${pkgconfig_dir}/hwloc.pc"
"prefix=${CURRENT_PACKAGES_DIR}
exec_prefix=\${prefix}
libdir=\${prefix}${configuration}/lib
includedir=\${prefix}/include

Name: hwloc
Description: Hardware locality detection and management library
Version: ${VERSION}
Libs: -L\${libdir} -lhwloc
Cflags: -I\${includedir}
")
    endforeach()
    vcpkg_fixup_pkgconfig()
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
    return()
endif()

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    set(OPTIONS ac_cv_prog_cc_c99= # To avoid the compiler check for C99 which will fail for MSVC
                --disable-plugin-dlopen) 
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    list(APPEND OPTIONS "HWLOC_LDFLAGS=-no-undefined")
elseif(VCPKG_TARGET_IS_OSX)
    list(APPEND OPTIONS "HWLOC_LDFLAGS=-framework CoreFoundation")
endif()

vcpkg_configure_make(
    SOURCE_PATH "${SOURCE_PATH}"
    AUTOCONFIG
    OPTIONS
        ${OPTIONS} 
        --disable-libxml2
        --disable-opencl
        --disable-cairo
        --disable-cuda
        --disable-libudev
        --disable-levelzero
        --disable-nvml
        --disable-rsmi
        --disable-pci
        #--disable-cpuid
        #--disable-picky
)

vcpkg_install_make()
vcpkg_fixup_pkgconfig()
vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

if(EXISTS "${CURRENT_PACKAGES_DIR}/tools/hwloc/bin/hwloc-compress-dir")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/hwloc/bin/hwloc-compress-dir" "${CURRENT_INSTALLED_DIR}" "`dirname $0`/../../.." IGNORE_UNCHANGED)
endif()
if(EXISTS "${CURRENT_PACKAGES_DIR}/tools/hwloc/debug/bin/hwloc-compress-dir")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/hwloc/debug/bin/hwloc-compress-dir" "${CURRENT_INSTALLED_DIR}" "`dirname $0`/../../../.." IGNORE_UNCHANGED)
endif()

if(EXISTS "${CURRENT_PACKAGES_DIR}/tools/hwloc/bin/hwloc-gather-topology")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/hwloc/bin/hwloc-gather-topology" "${CURRENT_INSTALLED_DIR}" "`dirname $0`/../../..")
endif()
if(EXISTS "${CURRENT_PACKAGES_DIR}/tools/hwloc/debug/bin/hwloc-gather-topology")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/hwloc/debug/bin/hwloc-gather-topology" "${CURRENT_INSTALLED_DIR}" "`dirname $0`/../../../..")
endif()

# Handle copyright
file(INSTALL "${SOURCE_PATH}/COPYING" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

file(REMOVE "${CURRENT_PACKAGES_DIR}/debug/COPYING.txt"
            "${CURRENT_PACKAGES_DIR}/debug/README.txt"
            "${CURRENT_PACKAGES_DIR}/debug/NEWS.txt"
            "${CURRENT_PACKAGES_DIR}/COPYING.txt"
            "${CURRENT_PACKAGES_DIR}/README.txt"
            "${CURRENT_PACKAGES_DIR}/NEWS.txt"
    )
