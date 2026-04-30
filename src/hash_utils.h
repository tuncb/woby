#pragma once

#include "camera.h"
#include "ui_state.h"

#include <array>
#include <cstdint>

namespace woby {

[[nodiscard]] uint32_t floatBits(float value);
void hashCombine(uint64_t& seed, uint64_t value);
void hashFloat(uint64_t& seed, float value);
void hashBool(uint64_t& seed, bool value);
void hashArray3(uint64_t& seed, const std::array<float, 3>& values);
void hashCamera(uint64_t& seed, const SceneCamera& camera);
void hashBounds(uint64_t& seed, const Bounds& bounds);
void hashFileSettings(uint64_t& seed, const UiFileSettings& settings);
void hashGroupSettings(uint64_t& seed, const UiGroupState& settings);

} // namespace woby
