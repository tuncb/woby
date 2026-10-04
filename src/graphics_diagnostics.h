#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace woby::graphics {

// Own the callback's borrowed strings before the Vulkan candidate is destroyed.
struct DeviceDiagnostic {
    gpu::DeviceDiagnosticKind kind{};
    std::string detail;
    std::string deviceName;
    std::string driverInfo;
    uint32_t driverVersion = 0;
    uint32_t apiVersion = 0;
    gpu::Error error = gpu::Error::none;
    int32_t apiResult = 0;
};

struct DeviceCreationDiagnostics {
    std::vector<DeviceDiagnostic> entries;
    // The callback may run inside an exception-disabled backend. Preserve the
    // last initialization error even if owning the full diagnostics fails.
    std::array<char, 512> emergencyDetail{};
    int32_t emergencyApiResult = 0;
    bool incomplete = false;
};

void collectDeviceDiagnostic(void* context, const gpu::DeviceDiagnostic& diagnostic) noexcept;
std::string formatDeviceCreationFailure(const DeviceCreationDiagnostics& diagnostics, gpu::Error error,
    bool metal = false);

} // namespace woby::graphics
