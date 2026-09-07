#include "console.h"

#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdint>

namespace {

bool validHandle(HANDLE handle)
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    return GetFileType(handle) != FILE_TYPE_UNKNOWN || GetLastError() == ERROR_SUCCESS;
}

bool validStream(FILE* stream)
{
    const int descriptor = _fileno(stream);
    return descriptor >= 0 && validHandle(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)));
}

void connectStream(FILE* stream, DWORD identifier, const char* mode, int flags)
{
    // CRT streams inherited from a shell can already refer to pipes or files.
    if (validStream(stream)) {
        return;
    }
    const HANDLE handle = GetStdHandle(identifier);
    if (!validHandle(handle)) {
        return;
    }
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate,
            0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return;
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), flags | _O_TEXT);
    if (descriptor < 0) {
        CloseHandle(duplicate);
        return;
    }
    // A GUI process without inherited streams starts with CRT descriptor -2.
    // Open a placeholder before replacing it with our owned handle duplicate.
    FILE* reopened = nullptr;
    if (freopen_s(&reopened, "NUL", mode, stream) == 0) {
        if (_dup2(descriptor, _fileno(stream)) == 0) {
            std::clearerr(stream);
            SetStdHandle(identifier, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream))));
        }
    }
    _close(descriptor);
}

} // namespace
#endif

namespace woby {

void initializeConsole()
{
#ifdef _WIN32
    const DWORD identifiers[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE inherited[3] = {};
    for (int index = 0; index < 3; ++index) {
        const HANDLE handle = GetStdHandle(identifiers[index]);
        if (validHandle(handle)) {
            inherited[index] = handle;
        }
    }
    // Explorer has no console to attach to. Failure is normal in that case.
    AttachConsole(ATTACH_PARENT_PROCESS);
    for (int index = 0; index < 3; ++index) {
        if (inherited[index] != nullptr) {
            SetStdHandle(identifiers[index], inherited[index]);
        }
    }
    connectStream(stdin, STD_INPUT_HANDLE, "r", _O_RDONLY);
    connectStream(stdout, STD_OUTPUT_HANDLE, "w", _O_WRONLY);
    connectStream(stderr, STD_ERROR_HANDLE, "w", _O_WRONLY);
#endif
}

bool hasStandardError()
{
#ifdef _WIN32
    return validStream(stderr);
#else
    return true;
#endif
}

} // namespace woby
