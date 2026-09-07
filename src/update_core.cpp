#include "update_internal.h"
#include "utf8_path.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <stdexcept>

namespace woby {
namespace {
std::string folded(std::string value)
{
    for (auto& ch : value) { if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch + ('a' - 'A')); } }
    return value;
}

bool validDigest(const std::string& value)
{
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void validateFileSet(const std::vector<PackageFile>& files)
{
    std::set<std::string> names;
    if (files.empty() || files.size() > 10000) { throw std::runtime_error("Invalid package file count."); }
    uint64_t total = 0;
    for (const auto& file : files) {
        const auto name = folded(file.path);
        if (!validPackagePath(file.path) || name == packageManifestName
            || name == ".woby-update" || name.starts_with(".woby-update/")
            || !validDigest(file.sha256) || file.size > 512ull * 1024 * 1024
            || !names.insert(name).second) {
            throw std::runtime_error("Invalid or duplicate package file: " + file.path);
        }
        total += file.size;
    }
    if (total > 2ull * 1024 * 1024 * 1024) { throw std::runtime_error("Package is too large."); }
    for (const auto& name : names) {
        for (auto slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
            if (names.contains(name.substr(0, slash))) { throw std::runtime_error("Conflicting package paths."); }
        }
    }
}

void copyPackageFile(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::permissions(destination, std::filesystem::status(source).permissions());
    syncUpdateFile(destination);
}
} // namespace

std::array<uint32_t, 3> parseUpdateVersion(const std::string& value)
{
    std::array<uint32_t, 3> result{};
    size_t start = 0;
    for (size_t index = 0; index < result.size(); ++index) {
        const size_t end = value.find('.', start);
        if ((index < 2) != (end != std::string::npos)) { throw std::runtime_error("Expected MAJOR.MINOR.PATCH version."); }
        const auto part = value.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || (part.size() > 1 && part.front() == '0')) { throw std::runtime_error("Invalid version."); }
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) { throw std::runtime_error("Invalid version."); }
        start = end == std::string::npos ? value.size() : end + 1;
    }
    return result;
}

bool validPackagePath(const std::string& value)
{
    if (value.empty() || value.size() > 240 || value.front() == '/' || value.back() == '/') { return false; }
    size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('/', begin);
        const auto part = value.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') { return false; }
        for (const unsigned char ch : part) {
            if (ch < 32 || ch >= 127 || std::string_view("\\:*?\"<>|").find(static_cast<char>(ch)) != std::string_view::npos) { return false; }
        }
        const auto stem = folded(part.substr(0, part.find('.')));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul"
            || (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9')) { return false; }
        if (end == std::string::npos) { break; }
        begin = end + 1;
    }
    return true;
}

std::string updatePlatform()
{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#else
    throw std::runtime_error("No release package is available for this platform.");
#endif
}

std::string updateExecutableName(bool helper)
{
    std::string name = helper ? "woby-update-helper" : "woby";
#ifdef _WIN32
    name += ".exe";
#endif
    return name;
}

ReleaseUpdate parseUpdateRelease(const std::string& json, const std::string& platform)
{
    const auto data = nlohmann::json::parse(json);
    if (data.at("draft").get<bool>() || data.at("prerelease").get<bool>()) { throw std::runtime_error("Expected a stable published release."); }
    const auto tag = data.at("tag_name").get<std::string>();
    if (!tag.starts_with('v')) { throw std::runtime_error("Release tag must start with v."); }
    ReleaseUpdate release;
    release.version = tag.substr(1);
    (void)parseUpdateVersion(release.version);
    if (platform != "windows-x64" && platform != "linux-x64" && platform != "macos-arm64") { throw std::runtime_error("Unsupported release platform."); }
    const auto assetName = "woby-" + platform + (platform == "windows-x64" ? ".zip" : ".tar.gz");
    for (const auto& asset : data.at("assets")) {
        if (asset.at("name") != assetName) { continue; }
        if (!release.url.empty()) { throw std::runtime_error("Duplicate release asset."); }
        release.url = asset.at("browser_download_url").get<std::string>();
        const auto digest = asset.at("digest").get<std::string>();
        release.size = asset.at("size").get<uint64_t>();
        if (release.url != "https://github.com/tuncb/woby/releases/download/" + tag + "/" + assetName
            || !digest.starts_with("sha256:") || !validDigest(digest.substr(7))
            || release.size == 0 || release.size > 512ull * 1024 * 1024) {
            throw std::runtime_error("Release asset has invalid URL, size, or SHA-256 digest.");
        }
        release.sha256 = digest.substr(7);
    }
    if (release.url.empty()) { throw std::runtime_error("Latest release has no " + assetName + " asset."); }
    return release;
}

nlohmann::json readUpdateJson(const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 4 * 1024 * 1024) {
        throw std::runtime_error("Missing or oversized update metadata: " + pathToUtf8(path));
    }
    std::ifstream input(path, std::ios::binary);
    return nlohmann::json::parse(input);
}

void writeUpdateJson(const std::filesystem::path& path, const nlohmann::json& value)
{
    const auto temporary = std::filesystem::path(path).concat(".tmp");
    requireUpdatePath(path.parent_path(), pathToUtf8(path.filename()));
    requireUpdatePath(path.parent_path(), pathToUtf8(temporary.filename()));
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << value.dump(2) << '\n';
    output.close();
    if (!output) { throw std::runtime_error("Cannot write update metadata."); }
    commitUpdateFile(temporary, path);
}

PackageManifest readPackageManifest(const std::filesystem::path& root)
{
    requireUpdatePath(root, packageManifestName);
    const auto data = readUpdateJson(root / packageManifestName);
    if (data.at("schema") != 1) { throw std::runtime_error("Unsupported package manifest schema."); }
    PackageManifest manifest;
    manifest.version = data.at("version").get<std::string>();
    (void)parseUpdateVersion(manifest.version);
    manifest.platform = data.at("platform").get<std::string>();
    if (manifest.platform != updatePlatform()) { throw std::runtime_error("Package architecture does not match this executable."); }
    for (const auto& entry : data.at("files")) {
        manifest.files.push_back({entry.at("path").get<std::string>(), entry.at("sha256").get<std::string>(),
            entry.at("size").get<uint64_t>(), entry.at("executable").get<bool>()});
    }
    validateFileSet(manifest.files);
    const auto shader = manifest.platform == "windows-x64" ? "dx11" : manifest.platform == "macos-arm64" ? "metal" : "glsl";
    std::vector<std::string> requiredFiles{updateExecutableName(), updateExecutableName(true), "assets/fonts/RobotoMonoNerdFont-Regular.ttf"};
    for (const auto* stage : {"vs", "fs"}) {
        for (const auto* name : {"mesh", "color", "comparison", "imgui", "point_sprite"}) {
            requiredFiles.push_back(std::string("assets/shaders/") + shader + "/" + stage + "_" + name + ".bin");
        }
    }
    for (const auto& required : requiredFiles) {
        if (std::none_of(manifest.files.begin(), manifest.files.end(), [&](const auto& file) { return file.path == required; })) {
            throw std::runtime_error("Package is missing " + required);
        }
    }
    for (const auto& file : manifest.files) {
        if ((file.path == updateExecutableName() || file.path == updateExecutableName(true)) && !file.executable) {
            throw std::runtime_error("Package does not mark its binaries executable.");
        }
    }
    return manifest;
}

void validateUpdatePackage(const std::filesystem::path& root, const PackageManifest& manifest)
{
    validateFileSet(manifest.files);
    for (const auto& file : manifest.files) {
        requireUpdatePath(root, file.path);
        const auto path = root / pathFromUtf8(file.path);
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) != file.size
            || updateSha256(path) != file.sha256) { throw std::runtime_error("Package file is missing or modified: " + file.path); }
#ifndef _WIN32
        const bool executable = (std::filesystem::status(path).permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
        if (file.executable != executable) { throw std::runtime_error("Package executable permissions do not match: " + file.path); }
#endif
    }
}

void validateUpdateJob(const std::filesystem::path& root, const std::filesystem::path& job)
{
    const auto name = pathToUtf8(job.filename());
    if (!root.is_absolute() || job.parent_path() != root / ".woby-update" || !name.starts_with("job-")
        || name.size() != 36 || name.substr(4).find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw std::runtime_error("Invalid update transaction directory.");
    }
    requireUpdatePath(root, ".woby-update/" + name);
}

void writeUpdateStatus(const std::filesystem::path& root, const std::string& state,
    const std::string& message, const std::filesystem::path& job)
{
    requireUpdatePath(root, ".woby-update/status.json");
    writeUpdateJson(root / ".woby-update/status.json", {{"state", state}, {"message", message},
        {"job", job.empty() ? std::string{} : pathToUtf8(job.filename())}});
}

void recoverUpdateTransaction(const std::filesystem::path& root, const std::filesystem::path& job)
{
    validateUpdateJob(root, job);
    requireUpdatePath(job, "journal.json");
    const auto journal = readUpdateJson(job / "journal.json");
    if (journal.at("schema") != 1 || journal.at("root") != pathToUtf8(root)) { throw std::runtime_error("Invalid recovery journal."); }
    const auto& files = journal.at("files");
    requireUpdatePath(job, "backup-complete.json");
    if (!std::filesystem::exists(job / "backup-complete.json")) {
        validateUpdatePackage(root, readPackageManifest(root));
        return;
    }
    // Validate every path before performing any recovery operation.
    for (const auto& file : files) {
        const auto name = file.at("path").get<std::string>();
        if (folded(name).starts_with(".woby-update")) { throw std::runtime_error("Invalid recovery path."); }
        requireUpdatePath(root, name);
        requireUpdatePath(job, "backup/" + name);
    }
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        const auto name = it->at("path").get<std::string>();
        const auto target = root / pathFromUtf8(name);
        const auto backup = job / "backup" / pathFromUtf8(name);
        if (it->at("existed").get<bool>()) {
            if (!std::filesystem::is_regular_file(backup)) { throw std::runtime_error("Recovery backup is missing: " + name); }
            if (!std::filesystem::is_regular_file(target) || std::filesystem::file_size(target) != std::filesystem::file_size(backup)
                || updateSha256(target) != updateSha256(backup)
                || std::filesystem::status(target).permissions() != std::filesystem::status(backup).permissions()) {
                copyPackageFile(backup, target);
            }
        } else if (std::filesystem::exists(target)) {
            if (!std::filesystem::is_regular_file(target)) { throw std::runtime_error("Recovery target is no longer a regular file."); }
            std::filesystem::remove(target);
        }
    }
    validateUpdatePackage(root, readPackageManifest(root));
}

void applyUpdateTransaction(const std::filesystem::path& root, const std::filesystem::path& job)
{
    validateUpdateJob(root, job);
    requireUpdatePath(job, "package");
    const auto package = job / "package";
    const auto oldManifest = readPackageManifest(root);
    const auto newManifest = readPackageManifest(package);
    if (parseUpdateVersion(newManifest.version) <= parseUpdateVersion(oldManifest.version)) { throw std::runtime_error("Update must be newer than the installed version."); }
    validateUpdatePackage(root, oldManifest);
    validateUpdatePackage(package, newManifest);
    uint64_t reserve = 16ull * 1024 * 1024;
    for (const auto& file : oldManifest.files) { reserve += file.size; }
    for (const auto& file : newManifest.files) { reserve += file.size; }
    if (std::filesystem::space(root).available < reserve) { throw std::runtime_error("Insufficient free space for update and rollback backup."); }
    std::set<std::string> oldFiles, newFiles;
    for (const auto& file : oldManifest.files) { oldFiles.insert(file.path); }
    for (const auto& file : newManifest.files) { newFiles.insert(file.path); }
    oldFiles.insert(packageManifestName);
    newFiles.insert(packageManifestName);
    auto allFiles = oldFiles;
    allFiles.insert(newFiles.begin(), newFiles.end());
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& name : allFiles) {
        requireUpdatePath(root, name);
        const auto path = root / pathFromUtf8(name);
        if (std::filesystem::exists(path) && !oldFiles.contains(name)) { throw std::runtime_error("Update would overwrite an unrelated file: " + name); }
        entries.push_back({{"path", name}, {"existed", std::filesystem::exists(path)}});
    }
    requireUpdatePath(job, "journal.json");
    if (std::filesystem::exists(job / "journal.json")) { throw std::runtime_error("This update transaction already has a journal; recover it first."); }
    writeUpdateJson(job / "journal.json", {{"schema", 1}, {"root", pathToUtf8(root)}, {"files", entries}});
    for (const auto& name : allFiles) {
        const auto target = root / pathFromUtf8(name);
        requireUpdatePath(job, "backup/" + name);
        if (oldFiles.contains(name)) {
            const auto backup = job / "backup" / pathFromUtf8(name);
            std::filesystem::create_directories(backup.parent_path());
            // Copy first so a crash never consumes the only recoverable copy.
            copyPackageFile(target, backup);
        }
    }
    // No deployment file is touched until the complete backup exists.
    writeUpdateJson(job / "backup-complete.json", {{"complete", true}});
    for (const auto& name : allFiles) {
        const auto target = root / pathFromUtf8(name);
        if (newFiles.contains(name)) { copyPackageFile(package / pathFromUtf8(name), target); }
        else { std::filesystem::remove(target); }
    }
    validateUpdatePackage(root, newManifest);
}
} // namespace woby
