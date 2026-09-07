#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "console.h"
#include <cstdio>
#include <string>
#include <string_view>

namespace {

int runChild(const wchar_t* executable, const wchar_t* argument)
{
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + argument;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process)) {
        return 10;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD result = 11;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &result);
    } else {
        TerminateProcess(process.hProcess, 11);
        WaitForSingleObject(process.hProcess, 10000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(result);
}

} // namespace

#ifdef WOBY_CONSOLE_PARENT
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2 || GetConsoleCP() == 0) {
        return 12;
    }
    const int result = runChild(argv[1], L"--attached");
    if (result != 0) {
        return result;
    }
    const HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    wchar_t text[4096]{};
    DWORD count = 0;
    const BOOL read = ReadConsoleOutputCharacterW(output, text, 4095, COORD{0, 0}, &count);
    CloseHandle(output);
    const std::wstring_view captured(text, count);
    return read && captured.find(L"stdout marker") != std::wstring_view::npos
        && captured.find(L"stderr marker") != std::wstring_view::npos ? 0 : 13;
}
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    const std::wstring_view mode(commandLine);
    if (mode == L"--detached-parent") {
        wchar_t executable[32768]{};
        if (GetModuleFileNameW(nullptr, executable, 32768) == 0) {
            return 14;
        }
        // This GUI parent never attaches to a console. The child gets ordinary
        // launch flags and no inherited streams, just like an Explorer launch.
        return runChild(executable, L"--detached");
    }
    if (GetConsoleCP() != 0) {
        return 15;
    }
    woby::initializeConsole();
    if (mode == L"--detached") {
        return GetConsoleCP() == 0 && !woby::hasStandardError() ? 0 : 16;
    }
    if (mode == L"--attached" && GetConsoleCP() == 0) {
        return 17;
    }
    if (!woby::hasStandardError()) {
        return 18;
    }
    if (mode == L"--streams") {
        char input[64]{};
        if (!std::fgets(input, sizeof(input), stdin)) {
            return 19;
        }
        std::printf("%s", input);
    }
    std::printf("stdout marker\n");
    std::fprintf(stderr, "stderr marker\n");
    return 0;
}
#endif
