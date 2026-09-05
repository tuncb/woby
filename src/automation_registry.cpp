#include "automation_registry.h"
#include "command_line.h"
#include "utf8_path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace woby {
namespace {

void secureRegistryDirectory(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        throw std::runtime_error("Cannot determine the current user for automation discovery.");
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    const BOOL read = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!read) {
        throw std::runtime_error("Cannot read the current user for automation discovery.");
    }
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
        throw std::runtime_error("Cannot create automation directory permissions.");
    }
    const std::wstring sddl = std::wstring(L"D:P(A;OICI;FA;;;") + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        throw std::runtime_error("Cannot create automation directory permissions.");
    }
    const BOOL secured = SetFileSecurityW(directory.c_str(),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    if (!secured) {
        throw std::runtime_error("Cannot secure the automation discovery directory.");
    }
#else
    if (::chmod(directory.c_str(), S_IRWXU) != 0) {
        throw std::runtime_error("Cannot secure the automation discovery directory.");
    }
#endif
}

std::filesystem::path recordPath(const std::filesystem::path& directory, const std::string& id)
{
    if (!validInstanceId(id)) {
        throw std::runtime_error("Invalid instance ID.");
    }
    return directory / ("instance-" + id + ".json");
}

AutomationInstance readRecord(const std::filesystem::path& path)
{
    if (std::filesystem::file_size(path) > 65536u) {
        throw std::runtime_error("Invalid automation instance record.");
    }
    std::ifstream input(path);
    const auto data = nlohmann::json::parse(input);
    AutomationInstance instance;
    instance.id = data.at("id").get<std::string>();
    instance.token = data.at("token").get<std::string>();
    instance.port = data.at("port").get<int>();
    instance.pid = data.at("pid").get<uint64_t>();
    if (data.at("version") != 1 || !validInstanceId(instance.id) || instance.port < 1
        || instance.port > 65535 || instance.token.size() != 64u
        || instance.token.find_first_not_of("0123456789abcdef") != std::string::npos
        || path.filename() != recordPath(path.parent_path(), instance.id).filename()) {
        throw std::runtime_error("Invalid automation instance record.");
    }
    return instance;
}

bool instanceLockHeld(std::filesystem::path path)
{
    path.replace_extension(".lock");
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_SHARING_VIOLATION;
    }
    CloseHandle(handle);
    return false;
#else
    const int handle = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (handle < 0) {
        return false;
    }
    const bool held = ::flock(handle, LOCK_EX | LOCK_NB) != 0;
    ::close(handle);
    return held;
#endif
}

} // namespace

std::filesystem::path automationRegistryDirectory()
{
#ifdef _WIN32
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        throw std::runtime_error("Cannot locate LocalAppData for automation discovery.");
    }
    const std::filesystem::path directory(localAppData);
    CoTaskMemFree(localAppData);
    return directory / "woby" / "instances";
#else
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
        return pathFromUtf8(runtime) / "woby" / "instances";
    }
    const char* userHome = std::getenv("HOME");
    if (!userHome || !*userHome) {
        throw std::runtime_error("Cannot locate the user directory for automation discovery.");
    }
#ifdef __APPLE__
    return pathFromUtf8(userHome) / "Library" / "Application Support" / "woby" / "instances";
#else
    return pathFromUtf8(userHome) / ".local" / "state" / "woby" / "instances";
#endif
#endif
}

std::string automationRandomHex(size_t byteCount)
{
    std::vector<unsigned char> bytes(byteCount);
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("Cannot generate an automation identity.");
    }
#else
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("Cannot generate an automation identity.");
    }
#endif
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(byteCount * 2u);
    for (const auto byte : bytes) {
        result += digits[byte >> 4u];
        result += digits[byte & 15u];
    }
    return result;
}

void reserveAutomationInstance(
    AutomationRegistration& registration, const std::filesystem::path& directory, const std::string& id)
{
    registration.recordPath = recordPath(directory, id);
    secureRegistryDirectory(directory);
    auto lockPath = registration.recordPath;
    lockPath.replace_extension(".lock");
#ifdef _WIN32
    const HANDLE handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Instance ID '" + id + "' is already in use or cannot be reserved.");
    }
    registration.lockHandle = reinterpret_cast<intptr_t>(handle);
    registration.instance.pid = GetCurrentProcessId();
#else
    const int handle = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (handle < 0) {
        throw std::runtime_error("Cannot reserve instance ID '" + id + "'.");
    }
    if (::flock(handle, LOCK_EX | LOCK_NB) != 0) {
        ::close(handle);
        throw std::runtime_error("Instance ID '" + id + "' is already in use.");
    }
    registration.lockHandle = handle;
    registration.instance.pid = static_cast<uint64_t>(::getpid());
#endif
    registration.instance.id = id;
    registration.instance.token = automationRandomHex(32u);
}

void publishAutomationInstance(AutomationRegistration& registration)
{
    const auto& instance = registration.instance;
    const nlohmann::json data = {{"version", 1}, {"id", instance.id}, {"pid", instance.pid},
        {"port", instance.port}, {"token", instance.token}};
    auto temporaryPath = registration.recordPath;
    temporaryPath += ".tmp-" + automationRandomHex(8u);
    try {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        output << data.dump();
        output.close();
        if (!output) {
            throw std::runtime_error("Cannot write automation discovery information.");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporaryPath.c_str(), registration.recordPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Cannot publish automation discovery information.");
        }
#else
        std::filesystem::rename(temporaryPath, registration.recordPath);
#endif
        registration.published = true;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw;
    }
}

void releaseAutomationInstance(AutomationRegistration& registration)
{
    if (registration.published) {
        std::error_code ignored;
        std::filesystem::remove(registration.recordPath, ignored);
        registration.published = false;
    }
    if (registration.lockHandle != -1) {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(registration.lockHandle));
#else
        ::close(static_cast<int>(registration.lockHandle));
#endif
        registration.lockHandle = -1;
    }
    // Keep lock files: unlinking a POSIX lock file can create two independent locks.
}

AutomationInstance readAutomationInstance(const std::filesystem::path& directory, const std::string& id)
{
    try {
        return readRecord(recordPath(directory, id));
    } catch (const std::exception&) {
        throw std::runtime_error("Cannot find instance '" + id + "'. Run 'woby ctl instances'.");
    }
}

std::vector<AutomationInstance> readAutomationInstances(const std::filesystem::path& directory)
{
    std::vector<AutomationInstance> instances;
    if (!std::filesystem::exists(directory)) {
        return instances;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        try {
            // Avoid a network timeout for every record left behind by a crashed app.
            // Live candidates are still authenticated over HTTP by the caller.
            if (instanceLockHeld(entry.path())) {
                instances.push_back(readRecord(entry.path()));
            }
        } catch (const std::exception&) {
            // A stale, incomplete or unrelated record is not a live instance.
        }
    }
    std::sort(instances.begin(), instances.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return instances;
}

} // namespace woby
