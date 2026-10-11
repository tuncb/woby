#pragma once
// Keep strict-float and AVX2 template instantiations separate from CDT
// instantiations in woby_core. The implementation itself is unchanged.
#define predicates intersection_probe_predicates
#include <predicates.h>
#undef predicates
