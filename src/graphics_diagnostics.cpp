#include "graphics_diagnostics.h"

namespace woby::graphics {
namespace {
const char* errorName(gpu::Error error)
{
    switch (error) {
    case gpu::Error::none: return "no error reported";
    case gpu::Error::unsupported: return "unsupported";
    case gpu::Error::device_lost: return "device lost";
    case gpu::Error::driver_error: return "driver error";
    }
    return "unknown error";
}

std::string apiVersion(uint32_t version)
{
    return std::to_string((version >> 22) & 0x7f) + "." + std::to_string((version >> 12) & 0x3ff)
        + "." + std::to_string(version & 0xfff);
}
} // namespace

void collectDeviceDiagnostic(void* context, const gpu::DeviceDiagnostic& diagnostic)
{
    auto& diagnostics = *static_cast<DeviceCreationDiagnostics*>(context);
    diagnostics.entries.push_back({diagnostic.kind, diagnostic.detail ? diagnostic.detail : "",
        diagnostic.device_name ? diagnostic.device_name : "", diagnostic.driver_info ? diagnostic.driver_info : "",
        diagnostic.driver_version, diagnostic.api_version, diagnostic.error, diagnostic.api_result});
}

std::string formatDeviceCreationFailure(const DeviceCreationDiagnostics& diagnostics, gpu::Error error, bool metal)
{
    bool missingCapabilities = false;
    bool initializationFailure = false;
    bool noSuitableDevice = false;
    std::string details;
    for (const auto& entry : diagnostics.entries) {
        using Kind = gpu::DeviceDiagnosticKind;
        switch (entry.kind) {
        case Kind::device:
            details += "\nGPU: " + entry.deviceName;
            if (entry.apiVersion != 0)
                details += "\n  Vulkan API: " + apiVersion(entry.apiVersion);
            if (!entry.driverInfo.empty())
                details += "\n  Driver: " + entry.driverInfo;
            if (entry.apiVersion != 0)
                details += "\n  Raw driverVersion: " + std::to_string(entry.driverVersion);
            details += "\n";
            break;
        case Kind::missing_extension:
            details += "  Missing required extension: " + entry.detail + "\n";
            missingCapabilities = true;
            break;
        case Kind::missing_feature:
            details += "  Unsupported required feature: " + entry.detail + "\n";
            missingCapabilities = true;
            break;
        case Kind::missing_requirement:
            details += "  Unsupported requirement: " + entry.detail + "\n";
            missingCapabilities = true;
            break;
        case Kind::feature_checks_skipped:
            details += "  Feature flags were not checked: " + entry.detail + "\n";
            break;
        case Kind::initialization_error:
            details += "  Initialization failed at " + entry.detail + ": " + errorName(entry.error);
            if (entry.apiResult != 0)
                details += " (VkResult " + std::to_string(entry.apiResult) + ")";
            details += "\n";
            initializationFailure = true;
            break;
        case Kind::no_suitable_device:
            noSuitableDevice = true;
            details += "\n" + entry.detail + "\n";
            break;
        case Kind::selected_device:
            details += "\nSelected GPU: " + entry.detail + "\n";
            break;
        }
    }
    std::string message = "NoGraphicsAPI renderer initialization failed (" + std::string(errorName(error)) + ").\n";
    if (noSuitableDevice && !initializationFailure)
        message += "No detected GPU meets the renderer requirements.\n";
    message += details;
    if (missingCapabilities) {
        message += "\nInstall a graphics driver that exposes the listed requirements on compatible hardware. ";
        message += metal ? "Metal 4 on macOS 26+ and the required GPU families must be supported."
            : "Vulkan 1.4 alone or a higher driver version does not guarantee support. "
              "A GPU rejected above cannot provide a fallback.";
    }
    if (initializationFailure)
        message += "\nThe initialization error above is separate from unsupported GPU capabilities. "
                   "Include the failing operation and result in a driver or support report.";
    if (diagnostics.entries.empty()) {
        message += metal && error == gpu::Error::unsupported
            ? "Metal 4 on macOS 26+ and an Apple GPU supporting MTLGPUFamilyApple7 and MTLGPUFamilyMetal4 are required."
            : "No capability details were returned. Include this error in a driver or support report.";
    }
    message += "\n\nCopy this complete diagnostic text into a support report. It is also written to standard error.";
    return message;
}

} // namespace woby::graphics
