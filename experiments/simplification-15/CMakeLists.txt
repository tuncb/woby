set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/scene_queries.cpp")
file(READ "${PROJECT_SOURCE_DIR}/src/scene_queries.cpp" queries_source)
string(REPLACE "\r\n" "\n" queries_source "${queries_source}")
string(FIND "${queries_source}" "void updateAnnotationProjection(" projection_start)
string(FIND "${queries_source}" "void appendSceneAnnotationParts(" projection_end)
if(projection_start LESS 0 OR projection_end LESS projection_start)
    message(FATAL_ERROR "Annotation projection source anchors changed")
endif()
math(EXPR projection_length "${projection_end} - ${projection_start}")
string(SUBSTRING "${queries_source}" ${projection_start} ${projection_length} projection_source)
string(REPLACE "updateAnnotationProjection(" "floatAnnotationProjection(" float_projection "${projection_source}")
string(REPLACE "double" "float" float_projection "${float_projection}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/projection_float.inc" "${float_projection}")
function(replace_projection_anchor variable old new)
    string(FIND "${${variable}}" "${old}" first)
    if(first LESS 0)
        message(FATAL_ERROR "Annotation slope-cache source anchor changed: ${old}")
    endif()
    string(LENGTH "${old}" old_length)
    math(EXPR rest_start "${first} + ${old_length}")
    string(SUBSTRING "${${variable}}" ${rest_start} -1 rest)
    string(FIND "${rest}" "${old}" second)
    if(NOT second LESS 0)
        message(FATAL_ERROR "Annotation slope-cache source anchor is ambiguous: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" replaced "${${variable}}")
    set(${variable} "${replaced}" PARENT_SCOPE)
endfunction()
set(cached_projection "${projection_source}")
replace_projection_anchor(cached_projection "updateAnnotationProjection(" "slopeCachedProjection(")
replace_projection_anchor(cached_projection "    vertices.reserve(lines.size() * 6);" [=[
    SlopeAllocation slopeAllocation;
    using SlopeEntry = std::pair<const uint64_t, std::array<double,2>>;
    using SlopeMap = boost::unordered_flat_map<uint64_t, std::array<double,2>, std::hash<uint64_t>,
        std::equal_to<uint64_t>, SlopeAllocator<SlopeEntry>>;
    SlopeMap depthSlopes(0, std::hash<uint64_t>{}, std::equal_to<uint64_t>{}, SlopeAllocator<SlopeEntry>(slopeAllocation));
    size_t availableFaces = 0;
    for (const auto& source : sources) { availableFaces += parts[source.partIndex].indexCount / 3; }
    depthSlopes.reserve(std::min(lines.size(), availableFaces));
    vertices.reserve(lines.size() * 6);]=])
replace_projection_anchor(cached_projection "            const auto& transform = sources[segment.source].transform;" [=[
            const uint64_t key = (uint64_t(segment.source) << 32u) | segment.triangle;
            const auto [slope, inserted] = depthSlopes.try_emplace(key, std::array<double,2>{0,0});
            if (inserted) {
            const auto& transform = sources[segment.source].transform;]=])
replace_projection_anchor(cached_projection [=[
        }
        auto a = annotationTransform(vp, {line.a[0], line.a[1], line.a[2], 1});]=] [=[
            slope->second = {slopeX, slopeY};
            }
            slopeX = slope->second[0]; slopeY = slope->second[1];
        }
        auto a = annotationTransform(vp, {line.a[0], line.a[1], line.a[2], 1});]=])
replace_projection_anchor(cached_projection [=[
    }
}

]=] [=[
    }
    slopeMetrics = {depthSlopes.size(), slopeAllocation.peak, slopeAllocation.calls};
}

]=])
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/projection_cached.inc" "${cached_projection}")
add_executable(woby_simplification_15 main.cpp)
target_include_directories(woby_simplification_15 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(woby_simplification_15 PRIVATE woby_core nlohmann_json::nlohmann_json)
woby_enable_project_warnings(woby_simplification_15)
if(BUILD_TESTING)
    add_test(NAME woby_simplification_15_contract COMMAND woby_simplification_15 --self-test)
    set_tests_properties(woby_simplification_15_contract PROPERTIES LABELS "unit;cpu")
endif()
