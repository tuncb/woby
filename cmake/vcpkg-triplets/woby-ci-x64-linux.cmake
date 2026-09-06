include("${CMAKE_CURRENT_LIST_DIR}/woby-x64-linux-dynamic.cmake")
list(APPEND VCPKG_HASH_ADDITIONAL_FILES
    "${CMAKE_CURRENT_LIST_DIR}/woby-x64-linux-dynamic.cmake")
set(VCPKG_BUILD_TYPE release)
