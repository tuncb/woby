include_guard(GLOBAL)

# Keep the bundle layout shared by local builds, release packaging, and fixtures.
function(woby_configure_macos_bundle target runtime_directory resource_directory)
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Woby"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.woby.viewer"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    set(${resource_directory} "${runtime_directory}/${target}.app/Contents/Resources" PARENT_SCOPE)

    install(TARGETS ${target} BUNDLE DESTINATION . COMPONENT Runtime)
    # The installed executable has its build RPATH removed. Give fixup_bundle
    # the original library directories so it can resolve @rpath dependencies.
    set(bundle_script "${CMAKE_CURRENT_BINARY_DIR}/${target}-bundle-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${bundle_script}" CONTENT
"set(woby_bundle \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${target}.app\")
set(woby_bundle_library_directories [==[${ARGN}]==])
include([==[${CMAKE_CURRENT_FUNCTION_LIST_DIR}/InstallMacOSBundle.cmake]==])
")
    install(SCRIPT "${bundle_script}" COMPONENT Runtime)
endfunction()
