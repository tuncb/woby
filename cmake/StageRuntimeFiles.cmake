cmake_minimum_required(VERSION 3.24)

include("${MANIFEST}")
list(LENGTH stage_sources source_count)
list(LENGTH stage_names name_count)
if(NOT source_count EQUAL name_count)
    message(FATAL_ERROR "Runtime staging sources and names must have the same length")
endif()

file(MAKE_DIRECTORY "${stage_root}")
cmake_path(ABSOLUTE_PATH stage_root NORMALIZE)
file(LOCK "${stage_state}.lock" GUARD PROCESS TIMEOUT 30)
set(previous_names)
if(EXISTS "${stage_state}")
    file(STRINGS "${stage_state}" previous_names ENCODING UTF-8)
endif()

# Only remove files previously managed by this manifest, and only within its
# destination directory. Never sweep the directory: it can contain user files.
foreach(name IN LISTS stage_names previous_names)
    set(destination "${stage_root}/${name}")
    cmake_path(NORMAL_PATH destination)
    cmake_path(IS_PREFIX stage_root "${destination}" NORMALIZE inside_root)
    if(IS_ABSOLUTE "${name}" OR NOT inside_root OR destination STREQUAL stage_root
       OR IS_DIRECTORY "${destination}")
        message(FATAL_ERROR "Invalid runtime staging destination: ${name}")
    endif()
endforeach()

# A glob-driven regeneration can leave MSBuild with the preceding project's
# tracked output list for one invocation. Recheck the current manifest before
# doing any staging, even if the build tool conservatively reruns the rule.
if(stage_stamp AND EXISTS "${stage_stamp}" AND EXISTS "${stage_state}"
   AND "${previous_names}" STREQUAL "${stage_names}"
   AND NOT "${MANIFEST}" IS_NEWER_THAN "${stage_stamp}"
   AND NOT "${CMAKE_CURRENT_LIST_FILE}" IS_NEWER_THAN "${stage_stamp}")
    set(up_to_date TRUE)
    if(source_count GREATER 0)
        math(EXPR last_source "${source_count} - 1")
        foreach(index RANGE ${last_source})
            list(GET stage_sources ${index} source)
            list(GET stage_names ${index} name)
            if(NOT EXISTS "${source}" OR NOT EXISTS "${stage_root}/${name}"
               OR "${source}" IS_NEWER_THAN "${stage_stamp}")
                set(up_to_date FALSE)
                break()
            endif()
        endforeach()
    endif()
    if(up_to_date)
        return()
    endif()
endif()

if(source_count GREATER 0)
    math(EXPR last_source "${source_count} - 1")
    foreach(index RANGE ${last_source})
        list(GET stage_sources ${index} source)
        list(GET stage_names ${index} name)
        set(destination "${stage_root}/${name}")
        cmake_path(GET destination PARENT_PATH parent)
        file(MAKE_DIRECTORY "${parent}")
        # Repair deleted destinations and copy only changed bytes.
        file(COPY_FILE "${source}" "${destination}" ONLY_IF_DIFFERENT)
        if(stage_refresh_timestamps)
            # MSBuild compares the whole output group to its newest input.
            # Use the current manifest here: after glob-driven regeneration,
            # the invoking build may still have the old list of outputs.
            file(TOUCH "${destination}")
        endif()
    endforeach()
endif()

foreach(name IN LISTS previous_names)
    if(NOT name IN_LIST stage_names)
        file(REMOVE "${stage_root}/${name}")
    endif()
endforeach()

string(JOIN "\n" state_content ${stage_names})
set(previous_content)
if(EXISTS "${stage_state}")
    file(READ "${stage_state}" previous_content)
endif()
if(NOT EXISTS "${stage_state}" OR NOT state_content STREQUAL previous_content)
    file(WRITE "${stage_state}" "${state_content}")
endif()
if(stage_stamp)
    file(TOUCH "${stage_stamp}")
endif()
