include_guard(GLOBAL)

option(WOBY_USE_PCH "Precompile stable headers for faster development builds" ON)

function(woby_enable_precompiled_headers target)
    if(WOBY_USE_PCH)
        # Keep editable project headers out of the PCH. Each target builds its own
        # PCH because the app, automation library and tests have different flags.
        target_precompile_headers(${target} PRIVATE
            <algorithm> <array> <cstdint> <filesystem> <memory>
            <optional> <string> <vector>
            ${ARGN}
        )
    endif()
endfunction()
