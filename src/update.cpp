#include "update_internal.h"
#include "utf8_path.h"

#include <curl/curl.h>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace woby {
namespace {
struct DownloadSink {
    std::ostream* output = nullptr;
    uint64_t limit = 0;
    uint64_t written = 0;
};

size_t writeDownload(char* data, size_t size, size_t count, void* user) noexcept
{
    auto& sink = *static_cast<DownloadSink*>(user);
    if (size != 0 && count > SIZE_MAX / size) { return 0; }
    const auto bytes = size * count;
    if (bytes > sink.limit - sink.written) { return 0; }
    try {
        sink.output->write(data, static_cast<std::streamsize>(bytes));
        if (!*sink.output) { return 0; }
    } catch (...) { return 0; }
    sink.written += bytes;
    return bytes;
}

void download(const std::string& url, std::ostream& output, uint64_t limit)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { throw std::runtime_error("Cannot initialize HTTPS."); }
    const auto cleanup = [](void*) { curl_global_cleanup(); };
    const std::unique_ptr<void, decltype(cleanup)> global(reinterpret_cast<void*>(1), cleanup);
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> client(curl_easy_init(), curl_easy_cleanup);
    if (!client) { throw std::runtime_error("Cannot initialize download."); }
    DownloadSink sink{&output, limit, 0};
    const auto option = [&](auto name, auto value) {
        if (curl_easy_setopt(client.get(), name, value) != CURLE_OK) { throw std::runtime_error("Cannot configure HTTPS download."); }
    };
    option(CURLOPT_URL, url.c_str());
    option(CURLOPT_PROTOCOLS_STR, "https");
    option(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    option(CURLOPT_FOLLOWLOCATION, 1L);
    option(CURLOPT_MAXREDIRS, 5L);
    option(CURLOPT_SSL_VERIFYPEER, 1L);
    option(CURLOPT_SSL_VERIFYHOST, 2L);
#ifndef _WIN32
    // Use the deployed machine's trust store, not a vcpkg build-host path.
    for (const auto* bundle : {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem", "/etc/pki/tls/certs/ca-bundle.crt"}) {
        if (std::filesystem::is_regular_file(bundle)) { option(CURLOPT_CAINFO, bundle); break; }
    }
#endif
    option(CURLOPT_USERAGENT, "woby-updater");
    option(CURLOPT_FAILONERROR, 1L);
    option(CURLOPT_CONNECTTIMEOUT, 20L);
    option(CURLOPT_TIMEOUT, 300L);
    option(CURLOPT_LOW_SPEED_LIMIT, 1024L);
    option(CURLOPT_LOW_SPEED_TIME, 30L);
    option(CURLOPT_NOSIGNAL, 1L);
    option(CURLOPT_WRITEFUNCTION, &writeDownload);
    option(CURLOPT_WRITEDATA, &sink);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        curl_slist_append(nullptr, "Accept: application/vnd.github+json"), curl_slist_free_all);
    if (!headers) { throw std::runtime_error("Cannot configure GitHub headers."); }
    option(CURLOPT_HTTPHEADER, headers.get());
    const auto result = curl_easy_perform(client.get());
    long status = 0;
    curl_easy_getinfo(client.get(), CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK || status != 200) {
        throw std::runtime_error("GitHub download failed (HTTP " + std::to_string(status) + "): " + curl_easy_strerror(result));
    }
}

void printResult(const nlohmann::json& result, bool json)
{
    const auto text = json ? result.dump() : result.at("message").get<std::string>();
    std::printf("%s\n", text.c_str());
}

std::filesystem::path createJob(const std::filesystem::path& root)
{
    std::random_device random;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string name = "job-";
        for (int index = 0; index < 32; ++index) { name += "0123456789abcdef"[random() & 15u]; }
        const auto job = root / ".woby-update" / name;
        requireUpdatePath(root, ".woby-update/" + name);
        if (std::filesystem::create_directory(job)) {
#ifndef _WIN32
            std::filesystem::permissions(job, std::filesystem::perms::owner_all);
#endif
            return job;
        }
    }
    throw std::runtime_error("Cannot create an update transaction directory.");
}

void copyHelper(const std::filesystem::path& root, const std::filesystem::path& job, const PackageManifest& installed)
{
    const auto helper = job / "helper";
    std::filesystem::create_directory(helper);
    for (const auto& file : installed.files) {
        const auto path = pathFromUtf8(file.path);
        if (path.has_parent_path()) { continue; }
        const auto name = pathToUtf8(path.filename());
        if (name != updateExecutableName(true) && !name.ends_with(".dll") && !name.ends_with(".dylib")
            && name.find(".so") == std::string::npos) { continue; }
        std::filesystem::copy_file(root / path, helper / path);
        std::filesystem::permissions(helper / path, std::filesystem::status(root / path).permissions());
    }
}
} // namespace

int runUpdateCommand(const UpdateArguments& arguments, const std::string& currentVersion)
{
    try {
        const auto root = updateExecutablePath().parent_path();
        requireUpdatePath(root, ".woby-update/status.json");
        if (arguments.command == UpdateCommand::status) {
            if (!std::filesystem::exists(root / ".woby-update/status.json")) {
                printResult({{"state", "none"}, {"message", "No update has been started for this deployment."}}, arguments.json);
                return 0;
            }
            auto status = readUpdateJson(root / ".woby-update/status.json");
            const auto state = status.at("state").get<std::string>();
            if (state == "pending" || state == "applying" || state == "recovery-required") {
                const auto job = root / ".woby-update" / pathFromUtf8(status.at("job").get<std::string>());
                validateUpdateJob(root, job);
                status["recoveryHelper"] = pathToUtf8(job / "helper" / updateExecutableName(true));
                status["deployment"] = pathToUtf8(root);
                status["message"] = status.at("message").get<std::string>()
                    + " If interrupted, run: \"" + pathToUtf8(job / "helper" / updateExecutableName(true))
                    + "\" --recover \"" + pathToUtf8(root) + "\" \"" + pathToUtf8(job) + "\"";
            }
            printResult(status, arguments.json);
            return state == "completed" || state == "none" ? 0 : state == "failed" || state == "recovery-required" ? 1 : 2;
        }
        DeploymentOwner lock(nullptr, releaseDeploymentLock);
        PackageManifest installed;
        if (arguments.command == UpdateCommand::install) {
            if (!std::filesystem::exists(root / packageManifestName)) {
                throw std::runtime_error("This is not a managed portable deployment. Manually install a release containing woby-manifest.json before using update. Build directories cannot update themselves.");
            }
            lock = lockDeployment(root, true);
            std::filesystem::create_directories(root / ".woby-update");
            if (std::filesystem::exists(root / ".woby-update/status.json")) {
                const auto previous = readUpdateJson(root / ".woby-update/status.json");
                const auto state = previous.at("state").get<std::string>();
                if (state != "completed" && state != "failed" && state != "none") {
                    throw std::runtime_error("An update is incomplete. Run 'woby update --status' and recover it before retrying.");
                }
            }
            installed = readPackageManifest(root);
            if (installed.version != currentVersion) { throw std::runtime_error("Installed manifest and executable versions disagree."); }
            validateUpdatePackage(root, installed);
        }
        std::ostringstream metadata;
        download("https://api.github.com/repos/tuncb/woby/releases/latest", metadata, 4 * 1024 * 1024);
        const auto release = parseUpdateRelease(metadata.str(), updatePlatform());
        const bool newer = parseUpdateVersion(release.version) > parseUpdateVersion(currentVersion);
        nlohmann::json result = {{"current", currentVersion}, {"latest", release.version}, {"deployment", pathToUtf8(root)},
            {"updateAvailable", newer}, {"state", newer ? "available" : "current"},
            {"message", newer ? "Woby " + release.version + " is available (installed " + currentVersion + ")." : "Woby " + currentVersion + " is up to date; no downgrade will be installed."}};
        if (!newer || arguments.command == UpdateCommand::check) { printResult(result, arguments.json); return 0; }
        const auto job = createJob(root);
        try {
            if (!arguments.json) { std::fprintf(stderr, "Downloading Woby %s...\n", release.version.c_str()); }
            const auto archive = job / "download";
            std::ofstream output(archive, std::ios::binary);
            download(release.url, output, release.size);
            output.close();
            if (!output || std::filesystem::file_size(archive) != release.size || updateSha256(archive) != release.sha256) {
                throw std::runtime_error("Downloaded release failed size or SHA-256 verification.");
            }
            extractUpdateArchive(archive, job / "package", updatePlatform());
            if (readPackageManifest(job / "package").version != release.version) { throw std::runtime_error("Package version does not match GitHub release tag."); }
            copyHelper(root, job, installed);
            writeUpdateStatus(root, "pending", "Installing Woby " + release.version + ".", job);
            launchUpdateHelper(root, job);
        } catch (const std::exception& error) {
            writeUpdateStatus(root, "failed", error.what(), job);
            throw;
        }
        result["state"] = "pending";
        result["message"] = "Update handed to helper. Run 'woby update --status' to confirm completion.";
        printResult(result, arguments.json);
        return 2;
    } catch (const std::exception& error) {
        if (arguments.json) { printResult({{"state", "failed"}, {"message", error.what()}, {"error", error.what()}}, true); }
        else { std::fprintf(stderr, "%s\n", error.what()); }
        return 1;
    }
}
} // namespace woby
