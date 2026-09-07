# FindLibArchive creates an UNKNOWN imported target whose .lib is invisible to
# TARGET_RUNTIME_DLLS. Describe vcpkg's dynamic Windows package explicitly, and
# select the matching configuration instead of linking Debug into Release.
set(woby_archive_target LibArchive::LibArchive)
if(WIN32 AND DEFINED VCPKG_TARGET_TRIPLET)
    set(woby_archive_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    if(EXISTS "${woby_archive_root}/bin/archive.dll")
        add_library(woby_archive SHARED IMPORTED)
        set_target_properties(woby_archive PROPERTIES
            IMPORTED_LOCATION "${woby_archive_root}/bin/archive.dll"
            IMPORTED_IMPLIB "${woby_archive_root}/lib/archive.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${LibArchive_INCLUDE_DIRS}")
        if(EXISTS "${woby_archive_root}/debug/bin/archive.dll")
            set_target_properties(woby_archive PROPERTIES
                IMPORTED_LOCATION_DEBUG "${woby_archive_root}/debug/bin/archive.dll"
                IMPORTED_IMPLIB_DEBUG "${woby_archive_root}/debug/lib/archive.lib")
        endif()
        set(woby_archive_target woby_archive)
        # FindZLIB has the same UNKNOWN-target issue. Both curl and libarchive
        # depend on this DLL. Older vcpkg revisions name it zlib1/zlibd1.
        find_file(woby_zlib_dll NAMES z.dll zlib1.dll PATHS "${woby_archive_root}/bin" NO_DEFAULT_PATH REQUIRED)
        find_file(woby_zlib_debug_dll NAMES zd.dll zlibd1.dll zlib1.dll PATHS "${woby_archive_root}/debug/bin" NO_DEFAULT_PATH)
        add_library(woby_zlib SHARED IMPORTED)
        set_target_properties(woby_zlib PROPERTIES
            IMPORTED_LOCATION "${woby_zlib_dll}"
            IMPORTED_IMPLIB "${ZLIB_LIBRARY_RELEASE}")
        if(woby_zlib_debug_dll AND ZLIB_LIBRARY_DEBUG)
            set_target_properties(woby_zlib PROPERTIES
                IMPORTED_LOCATION_DEBUG "${woby_zlib_debug_dll}"
                IMPORTED_IMPLIB_DEBUG "${ZLIB_LIBRARY_DEBUG}")
        endif()
        set_property(TARGET woby_archive APPEND PROPERTY INTERFACE_LINK_LIBRARIES woby_zlib)
    endif()
endif()
