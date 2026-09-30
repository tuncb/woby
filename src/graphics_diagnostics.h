#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <cstdint>
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
};

void collectDeviceDiagnostic(void* context, const gpu::DeviceDiagnostic& diagnostic);
std::string formatDeviceCreationFailure(const DeviceCreationDiagnostics& diagnostics, gpu::Error error,
    bool metal = false);

} // namespace woby::graphics
