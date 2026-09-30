#include "graphics.h"
#include "graphics_diagnostics.h"

#include <doctest/doctest.h>
#include <cstring>

namespace {
namespace g = woby::graphics;
using Kind = gpu::DeviceDiagnosticKind;

void report(g::DeviceCreationDiagnostics& diagnostics, Kind kind, const char* detail,
    gpu::Error error = gpu::Error::none, int32_t result = 0)
{
    g::collectDeviceDiagnostic(&diagnostics,
        {.kind = kind, .detail = detail, .error = error, .api_result = result});
}

void reportGpu(g::DeviceCreationDiagnostics& diagnostics, const char* name, const char* driver = "597.16")
{
    g::collectDeviceDiagnostic(&diagnostics, {.kind = Kind::device, .device_name = name,
        .driver_info = driver, .driver_version = 12345, .api_version = (1u << 22) | (4u << 12) | 329u});
}
} // namespace

TEST_CASE("GPU diagnostics name the observed laptop blocker and every rejected GPU")
{
    g::DeviceCreationDiagnostics diagnostics;
    reportGpu(diagnostics, "NVIDIA RTX 3500 Ada Generation Laptop GPU");
    report(diagnostics, Kind::missing_extension, "VK_KHR_device_address_commands");
    report(diagnostics, Kind::feature_checks_skipped, "required extensions are missing; unqueried flags are not evidence of unsupported features");
    reportGpu(diagnostics, "Intel RaptorLake-S", "Intel driver");
    for (const auto* extension : {"VK_EXT_descriptor_heap", "VK_KHR_device_address_commands",
             "VK_KHR_shader_untyped_pointers", "VK_EXT_mesh_shader"})
        report(diagnostics, Kind::missing_extension, extension);
    report(diagnostics, Kind::feature_checks_skipped, "required extensions are missing");
    report(diagnostics, Kind::no_suitable_device, "No detected GPU passed device selection.");

    const auto message = g::formatDeviceCreationFailure(diagnostics, gpu::Error::unsupported);
    CHECK(message.find("No detected GPU meets the renderer requirements.") != std::string::npos);
    const auto nvidia = message.find("GPU: NVIDIA RTX 3500 Ada Generation Laptop GPU");
    const auto intel = message.find("GPU: Intel RaptorLake-S");
    REQUIRE(nvidia != std::string::npos);
    REQUIRE(intel > nvidia);
    const auto nvidiaReport = message.substr(nvidia, intel - nvidia);
    CHECK(nvidiaReport.find("Missing required extension: VK_KHR_device_address_commands") != std::string::npos);
    CHECK(nvidiaReport.find("VK_EXT_descriptor_heap") == std::string::npos);
    CHECK(nvidiaReport.find("Unsupported required feature") == std::string::npos);
    CHECK(nvidiaReport.find("Vulkan API: 1.4.329") != std::string::npos);
    CHECK(nvidiaReport.find("Driver: 597.16") != std::string::npos);
    CHECK(nvidiaReport.find("Raw driverVersion: 12345") != std::string::npos);
    CHECK(nvidiaReport.find("Feature flags were not checked") != std::string::npos);
    const auto intelReport = message.substr(intel);
    for (const auto* extension : {"VK_EXT_descriptor_heap", "VK_KHR_device_address_commands",
             "VK_KHR_shader_untyped_pointers", "VK_EXT_mesh_shader"})
        CHECK(intelReport.find(std::string("Missing required extension: ") + extension) != std::string::npos);
    CHECK(message.find("Vulkan 1.4 alone or a higher driver version does not guarantee support") != std::string::npos);
    CHECK(message.find("A GPU rejected above cannot provide a fallback") != std::string::npos);
    CHECK(message.find("Copy this complete diagnostic text") != std::string::npos);
}

TEST_CASE("GPU diagnostics format exact required feature names and alternative requirements")
{
    g::DeviceCreationDiagnostics diagnostics;
    reportGpu(diagnostics, "Feature-limited GPU");
    report(diagnostics, Kind::missing_feature, "VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR::deviceAddressCommands");
    report(diagnostics, Kind::missing_feature, "VkPhysicalDeviceFeatures::sampleRateShading");
    report(diagnostics, Kind::missing_feature,
        "VkPhysicalDeviceFeatures::textureCompressionBC or VkPhysicalDeviceFeatures::textureCompressionASTC_LDR");
    report(diagnostics, Kind::missing_requirement, "Vulkan device API 1.4");
    const auto message = g::formatDeviceCreationFailure(diagnostics, gpu::Error::unsupported);
    CHECK(message.find("Unsupported required feature: VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR::deviceAddressCommands") != std::string::npos);
    CHECK(message.find("Unsupported required feature: VkPhysicalDeviceFeatures::sampleRateShading") != std::string::npos);
    CHECK(message.find("textureCompressionBC or VkPhysicalDeviceFeatures::textureCompressionASTC_LDR") != std::string::npos);
    CHECK(message.find("Unsupported requirement: Vulkan device API 1.4") != std::string::npos);
    CHECK(message.find("Initialization failed at") == std::string::npos);
}

TEST_CASE("GPU diagnostics distinguish surface and logical-device creation errors from capability rejection")
{
    for (const auto error : {gpu::Error::driver_error, gpu::Error::device_lost, gpu::Error::unsupported}) {
        for (const auto* stage : {"creating the SDL Vulkan surface", "vkCreateDevice"}) {
            g::DeviceCreationDiagnostics diagnostics;
            reportGpu(diagnostics, "Supported GPU");
            report(diagnostics, Kind::selected_device, "Supported GPU");
            report(diagnostics, Kind::initialization_error, stage, error, -3);
            const auto message = g::formatDeviceCreationFailure(diagnostics, error);
            CHECK(message.find(std::string("Initialization failed at ") + stage) != std::string::npos);
            CHECK(message.find("Selected GPU: Supported GPU") != std::string::npos);
            CHECK(message.find("VkResult -3") != std::string::npos);
            CHECK(message.find("separate from unsupported GPU capabilities") != std::string::npos);
            CHECK(message.find("No detected GPU meets") == std::string::npos);
            CHECK(message.find("Install a graphics driver that exposes") == std::string::npos);
        }
    }
}

TEST_CASE("GPU diagnostics retain rejection details when a supported fallback fails initialization")
{
    g::DeviceCreationDiagnostics diagnostics;
    reportGpu(diagnostics, "Rejected GPU");
    report(diagnostics, Kind::missing_extension, "VK_EXT_descriptor_heap");
    reportGpu(diagnostics, "Supported fallback");
    report(diagnostics, Kind::selected_device, "Supported fallback");
    report(diagnostics, Kind::initialization_error, "vkCreateDevice", gpu::Error::driver_error);
    const auto message = g::formatDeviceCreationFailure(diagnostics, gpu::Error::driver_error);
    CHECK(message.find("GPU: Rejected GPU") != std::string::npos);
    CHECK(message.find("Selected GPU: Supported fallback") != std::string::npos);
    CHECK(message.find("Initialization failed at vkCreateDevice: driver error") != std::string::npos);
    CHECK(message.find("No detected GPU meets") == std::string::npos);
}

TEST_CASE("GPU diagnostics do not treat enumeration failures as confirmed missing capabilities")
{
    g::DeviceCreationDiagnostics diagnostics;
    reportGpu(diagnostics, "Unqueried GPU");
    report(diagnostics, Kind::initialization_error, "vkEnumerateDeviceExtensionProperties", gpu::Error::unsupported);
    report(diagnostics, Kind::no_suitable_device, "No detected GPU passed device selection.");
    const auto message = g::formatDeviceCreationFailure(diagnostics, gpu::Error::unsupported);
    CHECK(message.find("Initialization failed at vkEnumerateDeviceExtensionProperties: unsupported") != std::string::npos);
    CHECK(message.find("No detected GPU meets") == std::string::npos);
    CHECK(message.find("Missing required extension") == std::string::npos);
    CHECK(message.find("Unsupported required feature") == std::string::npos);
}

TEST_CASE("GPU diagnostics own callback strings and tolerate unavailable metadata")
{
    g::DeviceCreationDiagnostics diagnostics;
    char name[] = "Original GPU";
    char detail[] = "VK_EXT_mesh_shader";
    g::collectDeviceDiagnostic(&diagnostics, {.kind = Kind::device, .device_name = name});
    report(diagnostics, Kind::missing_extension, detail);
    std::memset(name, 'x', sizeof(name) - 1);
    std::memset(detail, 'x', sizeof(detail) - 1);
    const auto message = g::formatDeviceCreationFailure(diagnostics, gpu::Error::unsupported);
    CHECK(message.find("GPU: Original GPU") != std::string::npos);
    CHECK(message.find("VK_EXT_mesh_shader") != std::string::npos);
    CHECK(message.find("Vulkan API:") == std::string::npos);
    CHECK(message.find("Driver:") == std::string::npos);
}

TEST_CASE("GPU diagnostics preserve platform-specific fallback errors without inventing Vulkan blockers")
{
    const g::DeviceCreationDiagnostics diagnostics;
    const auto driverError = g::formatDeviceCreationFailure(diagnostics, gpu::Error::driver_error);
    CHECK(driverError.find("driver error") != std::string::npos);
    CHECK(driverError.find("No capability details were returned") != std::string::npos);
    CHECK(driverError.find("Missing required extension") == std::string::npos);
    CHECK(driverError.find("No detected GPU meets") == std::string::npos);
    const auto metalError = g::formatDeviceCreationFailure(diagnostics, gpu::Error::unsupported, true);
    CHECK(metalError.find("Metal 4 on macOS 26+") != std::string::npos);
    CHECK(metalError.find("Vulkan") == std::string::npos);
}

TEST_CASE("GPU diagnostics clear stale failures when a new initialization succeeds")
{
    namespace g = woby::graphics;
    g::shutdown();
    g::Init options;
    options.type = g::RendererType::Noop;
    REQUIRE(g::init(options));
    CHECK_FALSE(g::init(options));
    CHECK(std::string(g::initializationError()) == "Renderer already initialized");
    g::shutdown();
    REQUIRE(g::init(options));
    CHECK(std::string(g::initializationError()).empty());
    g::shutdown();
}
