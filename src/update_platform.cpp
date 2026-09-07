#include "update_internal.h"
#include "utf8_path.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/evp.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace woby {
namespace {
#ifdef _WIN32
std::wstring quoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
#else
bool waitChild(pid_t child, int& status, int seconds)
{
    for (int tick = 0; tick < seconds * 20; ++tick) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) { return true; }
        if (result < 0 && errno != EINTR) { throw std::runtime_error("Cannot wait for update child process."); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return false;
}
#endif
} // namespace

std::filesystem::path updateExecutablePath()
{
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) { throw std::runtime_error("Cannot locate Woby executable."); }
    buffer.resize(count);
    return std::filesystem::canonical(buffer);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) { throw std::runtime_error("Cannot locate Woby executable."); }
    return std::filesystem::canonical(buffer.c_str());
#else
    return std::filesystem::canonical("/proc/self/exe");
#endif
}

void requireUpdatePath(const std::filesystem::path& root, const std::string& relative)
{
    if (!validPackagePath(relative)) { throw std::runtime_error("Unsafe update path: " + relative); }
    auto path = root;
    const auto check = [](const auto& candidate) {
        std::error_code error;
        const auto state = std::filesystem::symlink_status(candidate, error);
        if (error && error != std::errc::no_such_file_or_directory) { throw std::runtime_error("Cannot inspect update path: " + pathToUtf8(candidate)); }
        if (std::filesystem::is_symlink(state)) { throw std::runtime_error("Update paths cannot contain symbolic links."); }
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("Update paths cannot contain junctions or reparse points.");
        }
#endif
    };
    check(root);
    for (const auto& component : pathFromUtf8(relative)) { path /= component; check(path); }
}

std::string updateSha256(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) { throw std::runtime_error("Cannot hash " + pathToUtf8(file)); }
    std::array<unsigned char, 32> digest{};
    std::array<char, 65536> buffer{};
#ifdef _WIN32
    BCRYPT_HASH_HANDLE raw = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &raw, nullptr, 0, nullptr, 0, 0) < 0) { throw std::runtime_error("Cannot initialize SHA-256."); }
    const auto cleanup = [](void* hash) { BCryptDestroyHash(hash); };
    std::unique_ptr<void, decltype(cleanup)> hash(raw, cleanup);
#else
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) { throw std::runtime_error("Cannot initialize SHA-256."); }
#endif
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
#ifdef _WIN32
        if (BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) { throw std::runtime_error("SHA-256 failed."); }
#else
        if (EVP_DigestUpdate(hash.get(), buffer.data(), static_cast<size_t>(count)) != 1) { throw std::runtime_error("SHA-256 failed."); }
#endif
    }
    if (!input.eof()) { throw std::runtime_error("Cannot read file for SHA-256."); }
#ifdef _WIN32
    if (BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) { throw std::runtime_error("SHA-256 failed."); }
#else
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(hash.get(), digest.data(), &size) != 1 || size != digest.size()) { throw std::runtime_error("SHA-256 failed."); }
#endif
    std::string result;
    for (const auto byte : digest) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}

void syncUpdateFile(const std::filesystem::path& path)
{
#ifdef _WIN32
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { throw std::runtime_error("Cannot flush update file."); }
    const auto ok = FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) { throw std::runtime_error("Cannot flush update file."); }
#else
    const auto file = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) { throw std::runtime_error("Cannot flush update file."); }
    const auto result = fsync(file);
    ::close(file);
    if (result != 0) { throw std::runtime_error("Cannot flush update file."); }
#endif
}

void commitUpdateFile(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
    syncUpdateFile(temporary);
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot commit update metadata.");
    }
#else
    std::filesystem::rename(temporary, destination);
    const auto directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
    if (directory < 0) { throw std::runtime_error("Cannot flush update directory."); }
    const auto result = fsync(directory);
    ::close(directory);
    if (result != 0) { throw std::runtime_error("Cannot flush update directory."); }
#endif
}

void releaseDeploymentLock(DeploymentLock* lock)
{
    if (!lock) { return; }
    if (lock->handle != -1) {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(lock->handle));
#else
        ::close(static_cast<int>(lock->handle));
#endif
    }
    delete lock;
}

DeploymentOwner lockDeployment(const std::filesystem::path& root, bool exclusive)
{
    // Locks live in per-user runtime storage, so viewing a read-only portable
    // deployment does not require creating files beside its executable.
    std::filesystem::path directory;
    std::string identity;
#ifdef _WIN32
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) { throw std::runtime_error("Cannot locate updater lock directory."); }
    directory = std::filesystem::path(local) / "woby" / "update-locks";
    CoTaskMemFree(local);
    const auto rootHandle = CreateFileW(root.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (rootHandle == INVALID_HANDLE_VALUE) { throw std::runtime_error("Cannot identify deployment directory."); }
    BY_HANDLE_FILE_INFORMATION info{};
    const auto identified = GetFileInformationByHandle(rootHandle, &info);
    CloseHandle(rootHandle);
    if (!identified) { throw std::runtime_error("Cannot identify deployment directory."); }
    identity = std::to_string(info.dwVolumeSerialNumber) + "-" + std::to_string(info.nFileIndexHigh) + "-" + std::to_string(info.nFileIndexLow);
#else
    const auto* home = std::getenv("HOME");
    if (!home || !*home) { throw std::runtime_error("Cannot locate updater lock directory."); }
#ifdef __APPLE__
    directory = pathFromUtf8(home) / "Library/Application Support/woby/update-locks";
#else
    const auto* runtime = std::getenv("XDG_RUNTIME_DIR");
    directory = runtime && *runtime ? pathFromUtf8(runtime) / "woby-update-locks" : pathFromUtf8(home) / ".local/state/woby/update-locks";
#endif
    struct stat info{};
    if (::stat(root.c_str(), &info) != 0) { throw std::runtime_error("Cannot identify deployment directory."); }
    identity = std::to_string(info.st_dev) + "-" + std::to_string(info.st_ino);
#endif
    std::filesystem::create_directories(directory);
#ifndef _WIN32
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
#endif
    requireUpdatePath(directory, identity + ".lock");
    DeploymentOwner lock(new DeploymentLock, releaseDeploymentLock);
    const auto file = directory / (identity + ".lock");
#ifdef _WIN32
    const auto handle = CreateFileW(file.c_str(), exclusive ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
        exclusive ? 0 : FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) { lock->handle = reinterpret_cast<intptr_t>(handle); }
#else
    const int handle = ::open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (handle >= 0) {
        if (flock(handle, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0) { lock->handle = handle; }
        else { ::close(handle); }
    }
#endif
    if (lock->handle == -1) {
        throw std::runtime_error(exclusive ? "Close all viewers using this deployment and retry; another update or insufficient write permissions can also block installation."
            : "This deployment is being updated or its update lock is not writable. Try again after the update completes.");
    }
    return lock;
}

DeploymentOwner guardViewerDeployment()
{
    const auto root = updateExecutablePath().parent_path();
    if (!std::filesystem::exists(root / packageManifestName) && !std::filesystem::exists(root / ".woby-update/status.json")) {
        return {nullptr, releaseDeploymentLock};
    }
    auto lock = lockDeployment(root, false);
    requireUpdatePath(root, ".woby-update/status.json");
    if (std::filesystem::exists(root / ".woby-update/status.json")) {
        const auto status = readUpdateJson(root / ".woby-update/status.json");
        const auto state = status.at("state").get<std::string>();
        if (state == "pending" || state == "applying" || state == "recovery-required") {
            throw std::runtime_error("Update is incomplete. Run 'woby update --status' for the recovery helper path.");
        }
    }
    return lock;
}

void launchUpdateHelper(const std::filesystem::path& root, const std::filesystem::path& job)
{
    const auto executable = job / "helper" / updateExecutableName(true);
#ifdef _WIN32
    auto command = quoteArgument(executable.wstring()) + L" --apply " + quoteArgument(root.wstring())
        + L" " + quoteArgument(job.wstring()) + L" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        job.c_str(), &startup, &process)) { throw std::runtime_error("Cannot start update helper."); }
    CloseHandle(process.hThread);
#else
    const auto parent = std::to_string(getpid());
    const auto child = fork();
    if (child < 0) { throw std::runtime_error("Cannot start update helper."); }
    if (child == 0) {
        if (chdir(job.c_str()) != 0) { _exit(127); }
        execl(executable.c_str(), executable.c_str(), "--apply", root.c_str(), job.c_str(), parent.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    bool childExited = false;
#endif
    bool ready = false;
    for (int tick = 0; tick < 200; ++tick) {
        if (std::filesystem::exists(job / "ready.json")) { ready = true; break; }
#ifdef _WIN32
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) { break; }
#else
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child) { childExited = true; break; }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#ifdef _WIN32
    if (!ready) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
    CloseHandle(process.hProcess);
#else
    if (!ready && !childExited) { kill(child, SIGKILL); int status = 0; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
#endif
    if (!ready) { throw std::runtime_error("Update helper failed to start; deployment was not changed."); }
}

void waitForUpdateParent(uint64_t pid, const std::filesystem::path& job)
{
#ifdef _WIN32
    if (pid == 0 || pid > MAXDWORD) { throw std::runtime_error("Invalid update parent process."); }
    const auto parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!parent) { throw std::runtime_error("Cannot open update parent process."); }
    try { writeUpdateJson(job / "ready.json", {{"ready", true}}); }
    catch (...) { CloseHandle(parent); throw; }
    const auto result = WaitForSingleObject(parent, 60000);
    CloseHandle(parent);
    if (result != WAIT_OBJECT_0) { throw std::runtime_error("Timed out waiting for the update command to exit."); }
#else
    if (static_cast<uint64_t>(getppid()) != pid) { throw std::runtime_error("Invalid update parent process."); }
    writeUpdateJson(job / "ready.json", {{"ready", true}});
    for (int tick = 0; tick < 1200; ++tick) {
        if (static_cast<uint64_t>(getppid()) != pid) { return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("Timed out waiting for the update command to exit.");
#endif
}

void verifyUpdatedExecutable(const std::filesystem::path& root, const std::filesystem::path& job, const std::string& version)
{
    const auto executable = root / updateExecutableName();
    const auto output = job / "version.txt";
    requireUpdatePath(job, "version.txt");
    bool success = false;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto file = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); }
        if (input != INVALID_HANDLE_VALUE) { CloseHandle(input); }
        throw std::runtime_error("Cannot capture updated version.");
    }
    auto command = quoteArgument(executable.wstring()) + L" --version";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = file; startup.hStdError = file; startup.hStdInput = input;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process)) {
        if (WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0) {
            DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); success = code == 0;
        } else { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    CloseHandle(file); CloseHandle(input);
#else
    const auto child = fork();
    if (child < 0) { throw std::runtime_error("Cannot run updated version check."); }
    if (child == 0) {
        const auto file = ::open(output.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (file < 0 || chdir(root.c_str()) != 0 || dup2(file, STDOUT_FILENO) < 0 || dup2(file, STDERR_FILENO) < 0) { _exit(127); }
        ::close(file);
        execl(executable.c_str(), executable.c_str(), "--version", static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    success = waitChild(child, status, 15) && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    if (!success || !std::filesystem::is_regular_file(output) || std::filesystem::file_size(output) > 256) {
        throw std::runtime_error("Updated executable failed its version check.");
    }
    std::ifstream stream(output);
    std::string reported, extra;
    stream >> reported;
    if (reported != version || (stream >> extra)) { throw std::runtime_error("Updated binary version does not match the release."); }
}
} // namespace woby
