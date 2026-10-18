#pragma once
#include "profile.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace detector_profile {
// Only generated research sources include this guard. Production code is unchanged.
struct Scope {
    obj_profile::Phase phase;
    obj_profile::Json data = obj_profile::Json::object();
    explicit Scope(const char* name) : phase(obj_profile::begin(name)) {}
    ~Scope() { data["thread_id"] = GetCurrentThreadId(); obj_profile::end(phase,std::move(data)); }
};
} // namespace detector_profile
