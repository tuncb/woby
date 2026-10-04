#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

struct ReadProbe {
    std::string text;
    void* buffer{};
    OVERLAPPED* overlapped{};
    HANDLE file{};
    size_t offset{};
    DWORD size{};
    int submissions{};
    int completions{};
    int frees{};
    int failSubmission{};
    int failCompletion{};
    bool failEvent{};
    int allocations{}, failAllocation{};
};

ReadProbe probe;

void* checkedAllocate(size_t size, size_t alignment)
{
    if (++probe.allocations == probe.failAllocation) { return nullptr; }
    return _aligned_malloc(size, alignment);
}

BOOL WINAPI delayedRead(HANDLE file, LPVOID buffer, DWORD size, LPDWORD, LPOVERLAPPED overlapped)
{
    CHECK(probe.buffer == nullptr);
    ++probe.submissions;
    if (probe.submissions == probe.failSubmission) {
        SetLastError(ERROR_READ_FAULT);
        return FALSE;
    }
    probe.file = file;
    probe.buffer = buffer;
    probe.overlapped = overlapped;
    probe.offset = (static_cast<size_t>(overlapped->OffsetHigh) << 32) | overlapped->Offset;
    probe.size = size;
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

BOOL WINAPI completeRead(HANDLE file, LPOVERLAPPED overlapped, LPDWORD bytes, BOOL wait)
{
    CHECK(file == probe.file);
    CHECK(overlapped == probe.overlapped);
    CHECK(wait == TRUE);
    REQUIRE(probe.buffer != nullptr);
    ++probe.completions;
    *bytes = static_cast<DWORD>(std::min<size_t>(probe.size,
        probe.text.size() - std::min(probe.offset, probe.text.size())));
    if (*bytes) { std::memcpy(probe.buffer, probe.text.data() + probe.offset, *bytes); }
    probe.buffer = nullptr;
    probe.overlapped = nullptr;
    if (probe.completions == probe.failCompletion) {
        SetLastError(ERROR_READ_FAULT);
        return FALSE;
    }
    return TRUE;
}

void checkedFree(void* buffer)
{
    // Read completion is deliberately delayed until completeRead. This detects
    // premature destruction deterministically, without performing a freed write.
    CHECK(probe.buffer == nullptr);
    ++probe.frees;
    _aligned_free(buffer);
}

HANDLE WINAPI createReadEvent(LPSECURITY_ATTRIBUTES attributes, BOOL manualReset, BOOL initialState, LPCSTR name)
{
    if (probe.failEvent) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return CreateEventA(attributes, manualReset, initialState, name);
}

struct Fixture {
    std::filesystem::path root;
    std::filesystem::path path;

    Fixture()
    {
        probe = {};
        const auto prefix = "woby_reader_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = 0;; ++i) {
            root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / (prefix + "_" + std::to_string(i));
            if (std::filesystem::create_directory(root)) { break; }
        }
        path = root / "input.obj";
        std::ofstream(path) << "# fixture\n";
    }

    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

} // namespace

// Only this standalone executable substitutes OS calls. The production header,
// parser control flow, buffer ownership, and Windows reader remain unchanged.
#define ReadFile delayedRead
#define GetOverlappedResult completeRead
#define CreateEventA createReadEvent
#define _aligned_free checkedFree
#define _aligned_malloc checkedAllocate
#include <rapidobj/rapidobj.hpp>
#undef _aligned_malloc
#undef _aligned_free
#undef CreateEventA
#undef GetOverlappedResult
#undef ReadFile

namespace detail = rapidobj::detail;

TEST_CASE("parser reports aligned allocation failure before starting IO")
{
    const Fixture fixture;
    detail::sys::File file(fixture.path);
    REQUIRE(static_cast<bool>(file));
    detail::sys::FileReader reader(file);
    detail::Chunk chunk{};
    detail::SharedContext context{};
    SUBCASE("first buffer") { probe.failAllocation = 1; }
    SUBCASE("second buffer") { probe.failAllocation = 2; }
    CHECK_THROWS_AS(detail::ProcessBlocksImpl(&reader, 0, 1, false, &chunk, &context), std::bad_alloc);
    CHECK(probe.submissions == 0);
    CHECK(probe.frees == 1);
}

TEST_CASE("asynchronous parser worker propagates allocation failure to its caller")
{
    const Fixture fixture;
    // ParseFileParallel normally has several workers; using a single task here
    // exercises the same async boundary without sharing the OS probe between threads.
    std::ofstream fileData(fixture.path, std::ios::binary | std::ios::trunc);
    fileData << std::string(detail::kBlockSize, '\n');
    fileData.close();
    detail::sys::File file(fixture.path);
    auto context = std::make_shared<detail::SharedContext>();
    std::vector<detail::Chunk> chunks;
    probe.failAllocation = 1;
    CHECK_THROWS_AS(detail::ParseFileParallel(&file, &chunks, context), std::bad_alloc);
    CHECK(probe.submissions == 0);
    CHECK(probe.frees == 1);
}

TEST_CASE("parser drains prefetched reads before freeing buffers on error")
{
    const Fixture fixture;
    std::string firstLine = "vp 0 0\n";
    auto expected = rapidobj::rapidobj_errc::ParseError;
    SUBCASE("freeform parameter vertex") {}
    SUBCASE("weighted NURBS control point") { firstLine = "v 1 2 3 1\n"; }
    SUBCASE("malformed polygon vertex") { firstLine = "v invalid 0 0\n"; }
    SUBCASE("line too long") {
        firstLine = "#" + std::string(detail::kMaxLineLength, 'x') + "\n";
        expected = rapidobj::rapidobj_errc::LineTooLongError;
    }
    SUBCASE("cleanup read failure preserves parser error") { probe.failCompletion = 2; }
    probe.text = firstLine + std::string(2 * detail::kBlockSize, '\n');
    detail::sys::File file(fixture.path);
    REQUIRE(static_cast<bool>(file));
    detail::sys::FileReader reader(file);
    detail::Chunk chunk{};
    detail::SharedContext context{};
    detail::ProcessBlocksImpl(&reader, 0, 3, false, &chunk, &context);
    CHECK(chunk.error.code == rapidobj::make_error_code(expected));
    CHECK(chunk.error.line_num == 1);
    CHECK(probe.submissions == 2);
    CHECK(probe.completions == 2);
    CHECK(probe.frees == 2);
    CHECK(probe.buffer == nullptr);
}

TEST_CASE("parser read cleanup handles EOF and IO failures without extra waits")
{
    const Fixture fixture;
    probe.text = std::string(2 * detail::kBlockSize, '\n');
    SUBCASE("full blocks followed by EOF") {}
    SUBCASE("partial final block") { probe.text.resize(detail::kBlockSize + 17); }
    SUBCASE("first submission fails") { probe.failSubmission = 1; }
    SUBCASE("prefetch submission fails") { probe.failSubmission = 2; }
    SUBCASE("first completion fails") { probe.failCompletion = 1; }
    SUBCASE("prefetch completion fails") { probe.failCompletion = 2; }
    detail::sys::File file(fixture.path);
    REQUIRE(static_cast<bool>(file));
    detail::sys::FileReader reader(file);
    detail::Chunk chunk{};
    detail::SharedContext context{};
    detail::ProcessBlocksImpl(&reader, 0, 3, false, &chunk, &context);
    CHECK(static_cast<bool>(chunk.error) == (probe.failSubmission != 0 || probe.failCompletion != 0));
    CHECK(probe.completions == probe.submissions - (probe.failSubmission != 0 ? 1 : 0));
    CHECK(probe.frees == 2);
    CHECK(probe.buffer == nullptr);
}

TEST_CASE("pending read drains during exception unwinding")
{
    const Fixture fixture;
    probe.text = "# completed during unwinding\n";
    detail::sys::File file(fixture.path);
    REQUIRE(static_cast<bool>(file));
    detail::sys::FileReader reader(file);
    const auto unwind = [&] {
        auto buffer = std::unique_ptr<char, detail::sys::AlignedDeleter>(
            detail::sys::AlignedAllocate(detail::kBlockSize, 4096));
        auto read = detail::PendingRead{&reader};
        REQUIRE_FALSE(reader.ReadBlock(0, detail::kBlockSize, buffer.get()));
        throw std::runtime_error("original failure");
    };
    CHECK_THROWS_WITH(unwind(), "original failure");
    CHECK(probe.completions == 1);
    CHECK(probe.frees == 1);
}

TEST_CASE("Windows reader reports event creation failure")
{
    const Fixture fixture;
    detail::sys::File file(fixture.path);
    REQUIRE(static_cast<bool>(file));
    probe.failEvent = true;
    detail::sys::FileReader reader(file);
    CHECK(reader.Error() == std::error_code(ERROR_NOT_ENOUGH_MEMORY, std::system_category()));
    CHECK(probe.submissions == 0);
}

TEST_CASE("parser error does not consume a synchronous stream prefetch")
{
    probe = {};
    std::istringstream stream("vp 0 0\n" + std::string(2 * detail::kBlockSize, '\n'));
    detail::StreamReader reader(&stream);
    detail::Chunk chunk{};
    detail::SharedContext context{};
    detail::ProcessBlocksImpl(&reader, 0, 3, false, &chunk, &context);
    CHECK(chunk.error.code == rapidobj::make_error_code(rapidobj::rapidobj_errc::ParseError));
    CHECK(stream.tellg() == static_cast<std::streamoff>(detail::kBlockSize));
    CHECK(probe.completions == 0);
    CHECK(probe.frees == 2);
}
