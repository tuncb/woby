set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/MetalBufferRegistry.cpp.in")
replace_exact(src/NoGraphicsAPIMetal.mm
    "    BufferRecord* record = nullptr;"
    "    uint64 buffer_base = 0;")
replace_exact(src/NoGraphicsAPIMetal.mm
[==[    BufferRecord buffer_records[64] = {};
    alignas(64) uint64 buffer_snapshots[4096] = {};
    uint64 buffer_snapshot = 0;
    uint64 occupied_buffers = 0;
    uint32 buffer_cursor = 1;]==]
[==[    os_unfair_lock buffer_lock = OS_UNFAIR_LOCK_INIT;
    BufferRecord* buffer_records = nullptr;
    size_t buffer_count = 0, buffer_capacity = 0;]==])
file(READ "${NGAPI_SOURCE}/src/NoGraphicsAPIMetal.mm" source)
string(FIND "${source}" "// WOBY_METAL_BUFFER_REGISTRY:" begin)
if(begin EQUAL -1)
    string(FIND "${source}" "void update_buffer_index(" begin)
endif()
string(FIND "${source}" "\n}\n}\n\nDeviceInit create_device" end)
if(begin EQUAL -1 OR end EQUAL -1 OR end LESS begin)
    message(FATAL_ERROR "Metal buffer registry patch no longer matches the pinned source")
endif()
math(EXPR end "${end} + 2") # Keep the surrounding namespace's closing brace.
string(SUBSTRING "${source}" 0 ${begin} before)
string(SUBSTRING "${source}" ${end} -1 after)
file(READ "${CMAKE_CURRENT_LIST_DIR}/MetalBufferRegistry.cpp.in" registry)
string(STRIP "${registry}" registry)
set(updated "${before}${registry}${after}")
if(NOT source STREQUAL updated)
    file(WRITE "${NGAPI_SOURCE}/src/NoGraphicsAPIMetal.mm" "${updated}")
endif()
replace_exact(src/NoGraphicsAPIMetal.mm
    "        delete[] device->queues;"
    "        delete[] device->queues;\n        assert(device->buffer_count == 0);\n        free(device->buffer_records);")
