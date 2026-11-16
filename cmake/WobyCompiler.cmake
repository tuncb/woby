include_guard(GLOBAL)
file(READ "${CMAKE_CURRENT_LIST_DIR}/llvm-toolchain.json" woby_llvm_manifest)
string(JSON WOBY_LLVM_VERSION GET "${woby_llvm_manifest}" version)
get_property(woby_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
foreach(language IN ITEMS C CXX OBJC OBJCXX)
    if(language IN_LIST woby_languages)
        if(NOT CMAKE_${language}_COMPILER_ID STREQUAL "Clang"
            OR NOT CMAKE_${language}_COMPILER_VERSION VERSION_EQUAL WOBY_LLVM_VERSION)
            message(FATAL_ERROR "Woby requires upstream Clang ${WOBY_LLVM_VERSION} for ${language}; found ${CMAKE_${language}_COMPILER_ID} ${CMAKE_${language}_COMPILER_VERSION}. Use a Clang preset and a new build directory.")
        endif()
    endif()
endforeach()

if(APPLE)
    # The toolchain pairs upstream Clang with Apple's libc++ headers/runtime.
    # Apple supplies no std module units, but Clang discovers LLVM's bundled
    # units. CMake 4.5 scans them even when the source does not import std.
    # Clear that incompatible metadata after project() discovers the compiler.
    # CMake also forwards this value to its compiler and library probes.
    set(CMAKE_CXX_STDLIB_MODULES_JSON "")
endif()
