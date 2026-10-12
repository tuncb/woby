# The uploader may flush during ring reservation. Synchronize only submission,
# never the surrounding staging copy or GPU wait.
replace_exact(utility/include/NoGraphicsAPIUtility/upload_queue.hpp
    "    static constexpr uint32 operation_limit = 4096;"
    "    void* submit_context = nullptr;\n    SubmitResult (*submit_function)(void*, Device*, const SubmitDesc&, uint32) noexcept = nullptr;\n    static constexpr uint32 operation_limit = 4096;")
replace_exact(utility/src/upload_queue.cpp
    "const auto submitted = submit(state.device, {.commands = {state.commands}, .completion = completion}, state.queue_index);"
    "const auto submitted = submit_function\n        ? submit_function(submit_context, state.device, {.commands = {state.commands}, .completion = completion}, state.queue_index)\n        : submit(state.device, {.commands = {state.commands}, .completion = completion}, state.queue_index);")
replace_exact(utility/src/upload_queue.cpp
    "    other.state_ = {};"
    "    submit_context = other.submit_context;\n    submit_function = other.submit_function;\n    other.state_ = {};"
    "    submit_context = other.submit_context;")
