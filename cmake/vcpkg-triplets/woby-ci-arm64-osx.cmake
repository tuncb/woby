include("${CMAKE_CURRENT_LIST_DIR}/woby-arm64-osx.cmake")
list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_DIR}/woby-arm64-osx.cmake")
set(VCPKG_BUILD_TYPE release)
