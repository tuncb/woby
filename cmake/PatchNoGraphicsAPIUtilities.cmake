# Keep utility failures explicit so upstream's no-exceptions/no-RTTI build is
# safe. Normalize dependency trees configured with the former throwing patch.
foreach(path IN ITEMS
        utility/include/NoGraphicsAPIUtility/heap_allocator.hpp
        utility/include/NoGraphicsAPIUtility/texture_allocator.hpp
        utility/include/NoGraphicsAPIUtility/delete_queue.hpp
        utility/include/NoGraphicsAPIUtility/upload_queue.hpp
        utility/src/heap_allocator.cpp utility/src/texture_allocator.cpp
        utility/src/delete_queue.cpp utility/src/upload_queue.cpp)
    file(READ "${NGAPI_SOURCE}/${path}" original)
    string(REPLACE "/* Woby: allocation can throw. */" "noexcept" updated "${original}")
    string(REPLACE "#include <stdexcept>\n" "" updated "${updated}")
    string(REPLACE "    try { free_nodes = new NodeIndex[node_capacity]; }\n    catch (...) { delete[] nodes; throw; }"
        "    free_nodes = new NodeIndex[node_capacity];" updated "${updated}")
    string(REPLACE "        throw std::runtime_error(\"GPU texture creation failed: insufficient CPU/GPU memory or a driver error.\");"
        "        return {.status = TextureAllocationStatus::failed};" updated "${updated}")
    string(REPLACE "    if (!state_.heap.owner) throw std::runtime_error(state_.heap.error);\n    try {\n" "" updated "${updated}")
    string(REPLACE "    if (!state_.completion.semaphore) throw std::runtime_error(\"GPU upload timeline allocation failed.\");\n" "" updated "${updated}")
    string(REPLACE "        if (!batch.pool) throw std::runtime_error(\"GPU upload command pool allocation failed.\");\n" "" updated "${updated}")
    string(REPLACE "    } catch (...) { destroy(); throw; }\n" "" updated "${updated}")
    if(NOT original STREQUAL updated)
        file(WRITE "${NGAPI_SOURCE}/${path}" "${updated}")
    endif()
endforeach()

configure_file("${CMAKE_CURRENT_LIST_DIR}/ngapi/utility_allocation.hpp"
    "${NGAPI_SOURCE}/utility/include/NoGraphicsAPIUtility/utility_allocation.hpp" COPYONLY)

foreach(name IN ITEMS heap_allocator texture_allocator delete_queue upload_queue)
    replace_exact(utility/include/NoGraphicsAPIUtility/${name}.hpp
        "#pragma once" "#pragma once\n\n#include <NoGraphicsAPIUtility/utility_allocation.hpp>")
    replace_exact(utility/include/NoGraphicsAPIUtility/${name}.hpp
        "public:" "public:\n    const char* error = nullptr; // Static initialization error; use create_utility.")
endforeach()

replace_exact(utility/src/heap_allocator.cpp
[=[    nodes = new Node[node_capacity];
    free_nodes = new NodeIndex[node_capacity];]=]
[=[    nodes = detail::allocate_utility_array<Node>(node_capacity);
    if (!nodes) return;
    free_nodes = detail::allocate_utility_array<NodeIndex>(node_capacity);
    if (!free_nodes) {
        detail::free_utility_array(nodes);
        nodes = nullptr;
        return;
    }]=])
# Every destruction path must pair with the checked allocation, including move
# assignment. One replacement already occurs in constructor cleanup above.
file(READ "${NGAPI_SOURCE}/utility/src/heap_allocator.cpp" original)
string(REPLACE "delete[] nodes;" "detail::free_utility_array(nodes);" updated "${original}")
string(REPLACE "delete[] free_nodes;" "detail::free_utility_array(free_nodes);" updated "${updated}")
if(NOT original STREQUAL updated)
    file(WRITE "${NGAPI_SOURCE}/utility/src/heap_allocator.cpp" "${updated}")
endif()
replace_exact(utility/src/heap_allocator.cpp
[=[    : storage_(storage), ranges_(validate_storage(storage), max_allocations, alignment)
{
}]=]
[=[    : storage_(storage), ranges_(validate_storage(storage), max_allocations, alignment)
{
    if (!ranges_.nodes) error = "Insufficient CPU memory for graphics heap bookkeeping.";
}]=])

replace_exact(utility/include/NoGraphicsAPIUtility/texture_allocator.hpp
[=[struct PlacedTexture
{
    Texture* texture;
    uint32 token;
};]=]
[=[enum class TextureAllocationStatus { full, success, failed };

struct PlacedTexture
{
    Texture* texture = nullptr;
    uint32 token = 0;
    TextureAllocationStatus status = TextureAllocationStatus::full;
};]=])
replace_exact(utility/include/NoGraphicsAPIUtility/texture_allocator.hpp
    "An empty result reports exhausted heap space."
    "Status distinguishes exhausted heap space from native resource creation failure.")
replace_exact(utility/src/texture_allocator.cpp
    "    assert(heap.owner);"
    "    assert(heap.owner);\n    if (!ranges_.nodes) error = \"Insufficient CPU memory for graphics texture bookkeeping.\";")
replace_exact(utility/src/texture_allocator.cpp
    "    const HeapAllocator::Range range = ranges_.allocate"
    "    if (error) return {.status = TextureAllocationStatus::failed};\n    const HeapAllocator::Range range = ranges_.allocate")
replace_exact(utility/src/texture_allocator.cpp
[=[    return {
        .texture = create_texture(commands, desc, heap_, uint64{range.offset} * ranges_.element_size),
        .token = range.token,
    };]=]
[=[    auto texture = create_texture(commands, desc, heap_, uint64{range.offset} * ranges_.element_size);
    if (!texture) {
        ranges_.free(range.token);
        return {.status = TextureAllocationStatus::failed};
    }
    return {.texture = texture, .token = range.token, .status = TextureAllocationStatus::success};]=]
    "    auto texture = create_texture(commands, desc, heap_, uint64{range.offset} * ranges_.element_size);")
# Upgrade the former throwing version after the normalization above.
replace_exact(utility/src/texture_allocator.cpp
    "    return {.texture = texture, .token = range.token};"
    "    return {.texture = texture, .token = range.token, .status = TextureAllocationStatus::success};")

replace_exact(utility/src/delete_queue.cpp
    "entries_(new Entry[capacity])" "entries_(detail::allocate_utility_array<Entry>(capacity))")
replace_exact(utility/src/delete_queue.cpp
    "    assert(capacity != 0);"
    "    assert(capacity != 0);\n    if (!entries_) error = \"Insufficient CPU memory for graphics deletion queue.\";")
replace_exact(utility/src/delete_queue.cpp "delete[] entries_;" "detail::free_utility_array(entries_);")

replace_exact(utility/src/upload_queue.cpp
    "    state_.completion.semaphore = create_timeline_semaphore(device);"
    "    if (!state_.heap.owner) { error = state_.heap.error; destroy(); return; }\n    state_.completion.semaphore = create_timeline_semaphore(device);\n    if (!state_.completion.semaphore) { error = \"GPU upload timeline allocation failed.\"; destroy(); return; }")
replace_exact(utility/src/upload_queue.cpp
    "    state_.batches = new Batch[max_pending_batches]{};"
    "    state_.batches = detail::allocate_utility_array<Batch>(max_pending_batches);\n    if (!state_.batches) { error = \"Insufficient CPU memory for graphics upload batches.\"; destroy(); return; }")
replace_exact(utility/src/upload_queue.cpp
    "        batch.pool = create_command_pool(device, queue_index);"
    "        batch.pool = create_command_pool(device, queue_index);\n        if (!batch.pool) { error = \"GPU upload command pool allocation failed.\"; destroy(); return; }")
replace_exact(utility/src/upload_queue.cpp "delete[] state_.batches;" "detail::free_utility_array(state_.batches);")
replace_exact(utility/src/upload_queue.cpp
    "UploadQueue::UploadQueue(UploadQueue&& other) noexcept : state_(other.state_)"
    "UploadQueue::UploadQueue(UploadQueue&& other) noexcept : error(other.error), state_(other.state_)")
replace_exact(utility/src/upload_queue.cpp
    "    state_ = other.state_;" "    error = other.error;\n    state_ = other.state_;")
