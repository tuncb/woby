#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace woby {

// Shared by CPU import validation and the graphics upload interface. This is
// a representation limit, not a point-count or hardware memory budget.
using SceneBufferSize = uint32_t;

inline SceneBufferSize sceneBufferBytes(size_t count, size_t elementBytes)
{
    if (elementBytes == 0) { throw std::invalid_argument("GPU buffer element size must be positive."); }
    if (count > std::numeric_limits<SceneBufferSize>::max() / elementBytes) {
        throw std::runtime_error("Scene exceeds the supported 32-bit GPU buffer size.");
    }
    return static_cast<SceneBufferSize>(count * elementBytes);
}

} // namespace woby
