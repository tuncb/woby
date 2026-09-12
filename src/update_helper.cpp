#include "update_internal.h"
#include "utf8_path.h"
#include <cstdio>
#include <stdexcept>

namespace woby {
int runUpdateHelper(int argc, char** argv)
{
    std::filesystem::path root, job;
    DeploymentOwner lock(nullptr, releaseDeploymentLock);
    bool mayRecover = false;
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") { std::printf("%s\n", WOBY_VERSION); return 0; }
        const bool recover = argc == 4 && std::string(argv[1]) == "--recover";
        const bool restart = argc == 6 && std::string(argv[5]) == "--restart";
        if (!recover && !((argc == 5 || restart) && std::string(argv[1]) == "--apply")) {
            throw std::runtime_error("Usage: woby-update-helper --recover DEPLOYMENT JOB (normally launched by woby update).");
        }
        root = pathFromUtf8(argv[2]);
        job = pathFromUtf8(argv[3]);
        // Validate the supplied layout before resolving paths, so junctions and
        // symbolic links cannot be hidden by canonicalization. Resolve both
        // paths together: Windows callers may use an 8.3 deployment path.
        validateUpdateJob(root, job);
        root = std::filesystem::canonical(root);
        job = std::filesystem::canonical(job);
        validateUpdateJob(root, job);
        if (!recover) {
            size_t consumed = 0;
            const auto parent = std::stoull(argv[4], &consumed);
            if (consumed != std::string(argv[4]).size()) { throw std::runtime_error("Invalid parent process."); }
            waitForUpdateParent(parent, job);
        }
        lock = lockDeployment(root, true);
        const auto status = readUpdateJson(root / ".woby-update/status.json");
        if (status.at("job") != pathToUtf8(job.filename())) { throw std::runtime_error("This is not the current update transaction."); }
        if (status.at("state") == "completed" || status.at("state") == "failed") {
            throw std::runtime_error("This update is already finished; recovery is not needed.");
        }
        mayRecover = true;
        if (recover) {
            if (std::filesystem::exists(job / "journal.json")) { recoverUpdateTransaction(root, job); }
            else { validateUpdatePackage(root, readPackageManifest(root)); }
            writeUpdateStatus(root, "failed", "Interrupted update rolled back; previous deployment restored.", job);
            return 0;
        }
        writeUpdateStatus(root, "applying", "Replacing deployment files; keep viewers closed.", job);
        applyUpdateTransaction(root, job);
        const auto version = readPackageManifest(root).version;
        verifyUpdatedExecutable(root, job, version);
        writeUpdateStatus(root, "completed", "Woby " + version + " installed successfully.", job);
        // A reopened viewer must be able to acquire its shared deployment lock.
        // Restart failures must never roll back an already verified installation.
        mayRecover = false;
        lock.reset();
        if (restart) {
            try { launchUpdatedViewer(root); }
            catch (const std::exception& error) {
                writeUpdateStatus(root, "completed", "Woby " + version
                    + " installed successfully, but could not restart: " + error.what() + " Open Woby manually.", job);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        if (lock && mayRecover) {
            try {
                if (std::filesystem::exists(job / "journal.json")) { recoverUpdateTransaction(root, job); }
                else { validateUpdatePackage(root, readPackageManifest(root)); }
                writeUpdateStatus(root, "failed", std::string(error.what()) + " Previous deployment restored.", job);
            } catch (const std::exception& recoveryError) {
                try { writeUpdateStatus(root, "recovery-required", std::string(error.what()) + " Recovery failed: " + recoveryError.what(), job); }
                catch (...) {}
            }
        }
        return 1;
    }
}
} // namespace woby

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    std::vector<char*> pointers;
    for (int index = 0; index < argc; ++index) { arguments.push_back(woby::pathToUtf8(std::filesystem::path(argv[index]))); }
    for (auto& argument : arguments) { pointers.push_back(argument.data()); }
    return woby::runUpdateHelper(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return woby::runUpdateHelper(argc, argv); }
#endif
