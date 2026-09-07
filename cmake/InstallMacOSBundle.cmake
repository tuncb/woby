# Run only at installation time, after the complete .app has been copied.
include(BundleUtilities)
fixup_bundle("${woby_bundle}" "" "${woby_bundle_library_directories}")

# Dependency rewriting invalidates Mach-O signatures. Sign nested binaries
# before the app, using an ad-hoc identity (no developer certificate required).
find_program(WOBY_CODESIGN codesign REQUIRED)
get_bundle_all_executables("${woby_bundle}" bundle_executables)
file(GLOB_RECURSE bundle_libraries "${woby_bundle}/Contents/Frameworks/*.dylib")
foreach(binary IN LISTS bundle_libraries bundle_executables)
    execute_process(COMMAND "${WOBY_CODESIGN}" --force --sign - "${binary}"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(COMMAND "${WOBY_CODESIGN}" --force --sign - "${woby_bundle}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${WOBY_CODESIGN}" --verify --deep --strict "${woby_bundle}"
    COMMAND_ERROR_IS_FATAL ANY)
