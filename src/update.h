#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace woby {

enum class UpdateCommand { none, install, check, status };
struct UpdateArguments {
    UpdateCommand command = UpdateCommand::none;
    bool json = false;
};
struct ReleaseUpdate {
    std::string version;
    std::string url;
    std::string sha256;
    uint64_t size = 0;
};
struct PackageFile {
    std::string path;
    std::string sha256;
    uint64_t size = 0;
    bool executable = false;
};
struct PackageManifest {
    std::string version;
    std::string platform;
    std::vector<PackageFile> files;
};
struct DeploymentLock { intptr_t handle = -1; };
void releaseDeploymentLock(DeploymentLock* lock);
using DeploymentOwner = std::unique_ptr<DeploymentLock, decltype(&releaseDeploymentLock)>;

inline constexpr const char* packageManifestName = "woby-manifest.json";
[[nodiscard]] std::array<uint32_t, 3> parseUpdateVersion(const std::string& value);
[[nodiscard]] bool validPackagePath(const std::string& value);
[[nodiscard]] std::string updatePlatform();
[[nodiscard]] std::string updateExecutableName(bool helper = false);
[[nodiscard]] ReleaseUpdate parseUpdateRelease(const std::string& json, const std::string& platform);
[[nodiscard]] PackageManifest readPackageManifest(const std::filesystem::path& root);
[[nodiscard]] std::string updateSha256(const std::filesystem::path& file);
void validateUpdatePackage(const std::filesystem::path& root, const PackageManifest& manifest);
void extractUpdateArchive(const std::filesystem::path& archive, const std::filesystem::path& destination,
    const std::string& platform);
[[nodiscard]] std::filesystem::path updateExecutablePath();
[[nodiscard]] DeploymentOwner lockDeployment(const std::filesystem::path& root, bool exclusive);
[[nodiscard]] DeploymentOwner guardViewerDeployment();
void requireUpdatePath(const std::filesystem::path& root, const std::string& relative);
void writeUpdateStatus(const std::filesystem::path& root, const std::string& state,
    const std::string& message, const std::filesystem::path& job = {});
void applyUpdateTransaction(const std::filesystem::path& root, const std::filesystem::path& job);
void recoverUpdateTransaction(const std::filesystem::path& root, const std::filesystem::path& job);
[[nodiscard]] int runUpdateCommand(const UpdateArguments& arguments, const std::string& currentVersion);
[[nodiscard]] int runUpdateHelper(int argc, char** argv);

} // namespace woby
