#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>

namespace detector_profile {
// inspectIntersections traverses synchronously. No shared atomic counter is
// needed in this experimental copy of its narrow phase.
struct PredicateCounts {
    uint64_t calls = 0, fastSeparated = 0, adaptiveFalse = 0, adaptiveTrue = 0, rational = 0;
    uint64_t nodeBoxes = 0, faceBoxes = 0;
};
inline thread_local PredicateCounts predicates;
inline nlohmann::json predicateData() {
    return {{"classifier_calls",predicates.calls},{"fast_separated",predicates.fastSeparated},
        {"adaptive_false",predicates.adaptiveFalse},{"adaptive_true",predicates.adaptiveTrue},
        {"rational_fallback",predicates.rational},{"node_box_tests",predicates.nodeBoxes},{"face_box_tests",predicates.faceBoxes}};
}
} // namespace detector_profile
