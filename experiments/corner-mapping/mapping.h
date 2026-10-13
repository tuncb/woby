#pragma once

#include "profile.h"
#include <cstddef>
#include <string_view>

namespace corner_mapping {
inline size_t gatherWorkers = 1;
inline std::string_view currentPass;
inline bool validateUniqueUvs = true;

inline obj_profile::Phase begin(const char* name)
{
    currentPass = name;
    return obj_profile::begin(name);
}
}
