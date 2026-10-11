#pragma once
#include "mesh_intersections.h"
#include <array>
#include <vector>

namespace intersection_probe {
using Position = std::array<double,3>;
using Triangle = std::array<Position,3>;
using Ids = std::array<size_t,3>;
struct Pair { Triangle a,b; Ids aIds,bIds; };
struct Counters {
    size_t calls=0, filtered=0, rational=0, guarded=0, signs2d=0, signs3d=0;
    std::array<size_t,4> rationalShared{};
};
inline Counters counters;
inline bool useCandidate=false, forceRational=false;
inline std::vector<Pair>* capturedPairs=nullptr;
inline size_t captureLimit=20000;
bool predicate(const Triangle& a,const Triangle& b,const Ids& aIds,const Ids& bIds);
void enteredRational(const Triangle& a,const Triangle& b,const Ids& aIds,const Ids& bIds);
}
namespace woby {
bool probeBaseline(const intersection_probe::Triangle& a,const intersection_probe::Triangle& b,
    const intersection_probe::Ids& aIds,const intersection_probe::Ids& bIds);
bool probeCollapsed(const intersection_probe::Triangle& points);
MeshIntersections probeInspect(const AnalysisGeometry& geometry,IntersectionSettings settings,
    std::stop_token stop,IntersectionLimits limits);
#ifdef WOBY_PROBE_NORMAL_ALLOCATOR
MeshIntersections adoption_inspectIntersections(const AnalysisGeometry& geometry,IntersectionSettings settings,
    std::stop_token stop,IntersectionLimits limits);
#endif
}
