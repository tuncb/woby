#include "update_internal.h"
#include "utf8_path.h"
#include <cstdio>
#include <exception>

// A headless viewer stand-in: exercises the real startup guard after restart.
int runFixture(int argc, char** argv)
{
    if (argc == 5 && std::string(argv[1]) == "--handoff") {
        const auto root = woby::pathFromUtf8(argv[2]);
        const auto job = woby::pathFromUtf8(argv[3]);
        const auto guard = woby::lockDeployment(root, true);
        woby::launchUpdateHelper(root, job, std::string(argv[4]) == "True");
        return std::filesystem::exists(job / "journal.json") ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::printf("%s\n", WOBY_VERSION);
        return 0;
    }
    const auto root = woby::updateExecutablePath().parent_path();
    nlohmann::json result{{"argc", argc}, {"cwd", woby::pathToUtf8(std::filesystem::current_path())}};
    try {
        const auto guard = woby::guardViewerDeployment();
        result["guarded"] = bool(guard);
        result["state"] = woby::readUpdateJson(root / ".woby-update/status.json").at("state");
    } catch (const std::exception& error) { result["error"] = error.what(); }
    woby::writeUpdateJson(root / "restarted.json", result);
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    std::vector<char*> pointers;
    for (int index = 0; index < argc; ++index) { arguments.push_back(woby::pathToUtf8(std::filesystem::path(argv[index]))); }
    for (auto& argument : arguments) { pointers.push_back(argument.data()); }
    return runFixture(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return runFixture(argc, argv); }
#endif
