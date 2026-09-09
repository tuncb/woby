include_guard(GLOBAL)

function(woby_enable_project_warnings target)
    # Windows icon resources use RC, which does not accept C++ compiler flags.
    if(MSVC)
        target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/EHs>")
    else()
        target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>")
    endif()
endfunction()

function(woby_enable_enum_switch_warnings target)
    if(MSVC)
        target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/we4061;/we4062>")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wswitch-enum;-Werror=switch-enum>")
    endif()
endfunction()
