#include "hash_utils.h"

#include <cstring>

namespace woby {

uint32_t floatBits(float value)
{
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void hashCombine(uint64_t& seed, uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

void hashFloat(uint64_t& seed, float value)
{
    hashCombine(seed, floatBits(value));
}

void hashBool(uint64_t& seed, bool value)
{
    hashCombine(seed, value ? 1u : 0u);
}

void hashArray3(uint64_t& seed, const std::array<float, 3>& values)
{
    hashFloat(seed, values[0]);
    hashFloat(seed, values[1]);
    hashFloat(seed, values[2]);
}

void hashCamera(uint64_t& seed, const SceneCamera& camera)
{
    hashArray3(seed, camera.target);
    hashFloat(seed, camera.yawRadians);
    hashFloat(seed, camera.pitchRadians);
    hashFloat(seed, camera.rollRadians);
    hashFloat(seed, camera.distance);
    hashFloat(seed, camera.verticalFovDegrees);
    hashFloat(seed, camera.nearPlane);
}

void hashBounds(uint64_t& seed, const Bounds& bounds)
{
    hashArray3(seed, bounds.min);
    hashArray3(seed, bounds.max);
    hashFloat(seed, bounds.radius);
}

void hashFileSettings(uint64_t& seed, const UiFileSettings& settings)
{
    hashBool(seed, settings.visible);
    hashFloat(seed, settings.scale);
    hashFloat(seed, settings.opacity);
    hashArray3(seed, settings.center);
    hashArray3(seed, settings.translation);
    hashArray3(seed, settings.rotationDegrees);
}

void hashGroupSettings(uint64_t& seed, const UiGroupState& settings)
{
    hashBool(seed, settings.visible);
    hashBool(seed, settings.showVertices);
    hashFloat(seed, settings.scale);
    hashFloat(seed, settings.opacity);
    hashFloat(seed, settings.vertexSizeScale);
    hashArray3(seed, settings.center);
    hashArray3(seed, settings.translation);
    hashArray3(seed, settings.rotationDegrees);
}

} // namespace woby
