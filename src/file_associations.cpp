#include "file_associations.h"
#include "file_associations_windows.h"
#include "utf8_path.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <limits>
#include <vector>

namespace woby {
namespace {
constexpr const wchar_t* capabilities = L"Software\\Woby\\FileAssociations\\Capabilities";
constexpr const wchar_t* associationMappings = L"Software\\Woby\\FileAssociations\\Capabilities\\FileAssociations";

struct AssociationSpec {
    const wchar_t* extension;
    const wchar_t* progId;
    const wchar_t* description;
    const wchar_t* argument;
};

AssociationSpec associationSpec(FileAssociationType type)
{
    switch (type) {
    case FileAssociationType::scene: return {L".woby", L"Woby.Scene", L"Woby scene", L"--woby"};
    case FileAssociationType::obj: return {L".obj", L"Woby.WavefrontObj", L"Wavefront OBJ model", L"--file"};
    }
    throw std::invalid_argument("Unknown file association type.");
}

struct RegistryKey {
    HKEY handle = nullptr;
    ~RegistryKey() { if (handle) { RegCloseKey(handle); } }
};

void checkRegistry(LSTATUS status)
{
    if (status != ERROR_SUCCESS) {
        throw std::runtime_error("File association registry operation failed (Windows error "
            + std::to_string(status) + ").");
    }
}

bool openKey(HKEY root, const std::wstring& path, REGSAM access, RegistryKey& key)
{
    const auto status = RegOpenKeyExW(root, path.c_str(), 0, access, &key.handle);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) { return false; }
    checkRegistry(status);
    return true;
}

std::wstring readString(HKEY root, const std::wstring& path, const wchar_t* name)
{
    RegistryKey key;
    if (!openKey(root, path, KEY_QUERY_VALUE, key)) { return {}; }
    DWORD bytes = 0;
    auto status = RegGetValueW(key.handle, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_UNSUPPORTED_TYPE) { return {}; }
    checkRegistry(status);
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    checkRegistry(RegGetValueW(key.handle, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes));
    return buffer.data();
}

bool hasOpenWithEntry(HKEY root, const AssociationSpec& spec)
{
    RegistryKey key;
    if (!openKey(root, std::wstring(L"Software\\Classes\\") + spec.extension + L"\\OpenWithProgids",
            KEY_QUERY_VALUE, key)) { return false; }
    const auto status = RegQueryValueExW(key.handle, spec.progId, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_FILE_NOT_FOUND) { return false; }
    checkRegistry(status);
    return true;
}

void writeValue(HKEY root, const std::wstring& path, const wchar_t* name,
    DWORD type, const void* data, DWORD bytes)
{
    RegistryKey key;
    checkRegistry(RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.handle, nullptr));
    checkRegistry(RegSetValueExW(key.handle, name, 0, type, static_cast<const BYTE*>(data), bytes));
}

void writeString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& text)
{
    if (text.size() >= std::numeric_limits<DWORD>::max() / sizeof(wchar_t)) {
        throw std::invalid_argument("File association value is too long.");
    }
    writeValue(root, path, name, REG_SZ, text.c_str(), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
}

void deleteValue(HKEY root, const std::wstring& path, const wchar_t* name)
{
    RegistryKey key;
    if (!openKey(root, path, KEY_SET_VALUE, key)) { return; }
    const auto status = RegDeleteValueW(key.handle, name);
    if (status != ERROR_FILE_NOT_FOUND) { checkRegistry(status); }
}

void deleteTree(HKEY root, const std::wstring& path)
{
    const auto status = RegDeleteTreeW(root, path.c_str());
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) { checkRegistry(status); }
}

bool equalWindowsText(const std::wstring& a, const std::wstring& b)
{
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring progIdPath(const AssociationSpec& spec)
{
    return std::wstring(L"Software\\Classes\\") + spec.progId;
}

std::wstring queryAssociation(ASSOCSTR field, const wchar_t* extension)
{
    DWORD length = 0;
    if (AssocQueryStringW(ASSOCF_NONE, field, extension, nullptr, nullptr, &length) != S_FALSE || length == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(length);
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, field, extension, nullptr, buffer.data(), &length))) { return {}; }
    return buffer.data();
}

std::filesystem::path executablePath()
{
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) { throw std::runtime_error("Could not find the Woby application path."); }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) { CoUninitialize(); } }
};

FileAssociationsState executeAction(FileAssociationRequest request)
{
    FileAssociationsState state;
    state.supported = true;
    ComScope com;
    try {
        const auto executable = executablePath();
        try {
            switch (request.action) {
            case FileAssociationAction::registerCopy:
                registerFileAssociation(HKEY_CURRENT_USER, request.type, executable);
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
                state.message = "Registered this copy. Use Windows defaults to select Woby.";
                break;
            case FileAssociationAction::removeRegistration:
                removeFileAssociation(HKEY_CURRENT_USER, request.type, executable);
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
                state.message = "Removed registration for this copy.";
                break;
            case FileAssociationAction::openDefaults: {
                auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
                    L"ms-settings:defaultapps?registeredAppUser=Woby", nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32) {
                    result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
                        L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL));
                }
                if (result <= 32) { throw std::runtime_error("Could not open Windows Default apps."); }
                state.message = "Select the file type and choose Woby in Windows. Then return here to check the result.";
                break;
            }
            case FileAssociationAction::none:
            case FileAssociationAction::refresh: break;
            }
        } catch (const std::exception& error) {
            state.failed = true;
            state.message = error.what();
        }
        for (size_t index = 0; index < state.types.size(); ++index) {
            const auto type = index == 0 ? FileAssociationType::scene : FileAssociationType::obj;
            auto& status = state.types[index];
            status = readFileAssociationRegistration(HKEY_CURRENT_USER, type, executable);
            const auto spec = associationSpec(type);
            const auto progId = queryAssociation(ASSOCSTR_PROGID, spec.extension);
            if (equalWindowsText(progId, spec.progId)) {
                status.defaultCurrentCopy = status.registeredCurrentCopy;
                status.defaultApp = status.registeredCurrentCopy ? "Woby (this copy)" : "Woby (another copy)";
            } else {
                const auto name = queryAssociation(ASSOCSTR_FRIENDLYAPPNAME, spec.extension);
                status.defaultApp = !name.empty() ? pathToUtf8(std::filesystem::path(name))
                    : (!progId.empty() ? pathToUtf8(std::filesystem::path(progId)) : "No default app");
            }
        }
        state.checked = true;
    } catch (const std::exception& error) {
        state.failed = true;
        state.message = error.what();
    }
    return state;
}
} // namespace

std::wstring fileAssociationOpenCommand(FileAssociationType type, const std::filesystem::path& executable)
{
    const auto text = executable.native();
    if (!executable.is_absolute() || text.find_first_of(L"\"\r\n") != std::wstring::npos) {
        throw std::invalid_argument("File associations require an absolute application path without quotes or line breaks.");
    }
    return L"\"" + text + L"\" " + associationSpec(type).argument + L" \"%1\"";
}

FileAssociationStatus readFileAssociationRegistration(HKEY userRoot, FileAssociationType type,
    const std::filesystem::path& executable)
{
    FileAssociationStatus result;
    const auto spec = associationSpec(type);
    const auto path = progIdPath(spec);
    const auto registered = readString(userRoot, path, L"WobyExecutable");
    const auto command = readString(userRoot, path + L"\\shell\\open\\command", nullptr);
    if (!registered.empty()) { result.registeredExecutable = registered; }
    result.registeredCurrentCopy = equalWindowsText(command, fileAssociationOpenCommand(type, executable))
        && equalWindowsText(registered, executable.native())
        && hasOpenWithEntry(userRoot, spec)
        && readString(userRoot, associationMappings, spec.extension) == spec.progId
        && readString(userRoot, L"Software\\RegisteredApplications", L"Woby") == capabilities;
    return result;
}

void registerFileAssociation(HKEY userRoot, FileAssociationType type, const std::filesystem::path& executable)
{
    const auto command = fileAssociationOpenCommand(type, executable);
    const auto spec = associationSpec(type);
    const auto path = progIdPath(spec);
    writeString(userRoot, path, nullptr, spec.description);
    writeString(userRoot, path, L"WobyExecutable", executable.native());
    writeString(userRoot, path + L"\\DefaultIcon", nullptr, L"\"" + executable.native() + L"\",0");
    writeString(userRoot, path + L"\\shell\\open\\command", nullptr, command);
    writeString(userRoot, capabilities, L"ApplicationName", L"Woby");
    writeString(userRoot, capabilities, L"ApplicationDescription", L"View 3D models and Woby scenes.");
    writeString(userRoot, associationMappings, spec.extension, spec.progId);
    writeString(userRoot, L"Software\\RegisteredApplications", L"Woby", capabilities);
    writeValue(userRoot, std::wstring(L"Software\\Classes\\") + spec.extension + L"\\OpenWithProgids",
        spec.progId, REG_NONE, nullptr, 0);
}

void removeFileAssociation(HKEY userRoot, FileAssociationType type, const std::filesystem::path& executable)
{
    const auto spec = associationSpec(type);
    const auto path = progIdPath(spec);
    const auto registered = readString(userRoot, path, L"WobyExecutable");
    const auto command = readString(userRoot, path + L"\\shell\\open\\command", nullptr);
    if (registered.empty() && command.empty()) { return; }
    // A different portable copy, or a modified command, must retain its registration.
    if (!equalWindowsText(registered, executable.native())
        || !equalWindowsText(command, fileAssociationOpenCommand(type, executable))) {
        throw std::runtime_error("This registration belongs to another copy. Register this copy before removing it.");
    }
    deleteValue(userRoot, std::wstring(L"Software\\Classes\\") + spec.extension + L"\\OpenWithProgids", spec.progId);
    if (readString(userRoot, associationMappings, spec.extension) == spec.progId) {
        deleteValue(userRoot, associationMappings, spec.extension);
    }
    deleteTree(userRoot, path);
    RegistryKey mappings;
    DWORD count = 0;
    if (openKey(userRoot, associationMappings, KEY_QUERY_VALUE, mappings)) {
        checkRegistry(RegQueryInfoKeyW(mappings.handle, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, &count, nullptr, nullptr, nullptr, nullptr));
    }
    if (count == 0) {
        if (readString(userRoot, L"Software\\RegisteredApplications", L"Woby") == capabilities) {
            deleteValue(userRoot, L"Software\\RegisteredApplications", L"Woby");
        }
        deleteTree(userRoot, L"Software\\Woby\\FileAssociations");
    }
}
} // namespace woby
#endif

namespace woby {
void startFileAssociationAction(FileAssociationsRuntime& runtime, FileAssociationRequest request)
{
    if (runtime.work.valid() || request.action == FileAssociationAction::none) { return; }
#ifdef _WIN32
    runtime.state.supported = true;
    try {
        runtime.work = std::async(std::launch::async, [request] { return executeAction(request); });
        runtime.state.busy = true;
        runtime.state.failed = false;
        runtime.state.message.clear();
    } catch (const std::exception& error) {
        runtime.state.failed = true;
        runtime.state.message = error.what();
    }
#endif
}

void pollFileAssociations(FileAssociationsRuntime& runtime)
{
    if (!runtime.work.valid()
        || runtime.work.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return; }
    try { runtime.state = runtime.work.get(); }
    catch (const std::exception& error) {
        runtime.state.busy = false;
        runtime.state.failed = true;
        runtime.state.message = error.what();
    }
}
} // namespace woby
