include("${CMAKE_CURRENT_LIST_DIR}/woby-x64-windows.cmake")
list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_DIR}/woby-x64-windows.cmake")
set(VCPKG_BUILD_TYPE release)
