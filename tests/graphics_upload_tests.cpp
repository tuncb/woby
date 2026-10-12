#include "graphics.h"
#include <doctest/doctest.h>
#include <array>
#include <future>
#include <thread>

namespace {
struct UploadFixture {
    UploadFixture() {
        woby::graphics::Init options; options.type = woby::graphics::RendererType::Noop;
        REQUIRE(woby::graphics::init(options));
    }
    ~UploadFixture() { woby::graphics::shutdown(); }
};
auto source() {
    auto owner = std::make_shared<const std::array<uint32_t, 4>>(std::array<uint32_t, 4>{3, 7, 11, 19});
    return woby::graphics::BufferUploadSource{owner, owner->data(), 16, 4, true, true};
}
}

TEST_CASE("Owned GPU uploads complete and register buffers without rendered frames") {
    namespace g = woby::graphics;
    UploadFixture fixture;
    auto input = source();
    std::weak_ptr<const void> owner = input.owner;
    auto ticket = g::beginBufferUpload(std::vector{input});
    input = {};
    g::waitBufferUpload(ticket);
    CHECK(owner.expired());
    CHECK(g::submittedFrameToken() == 0);
    REQUIRE(g::bufferUploadComplete(ticket));
    CHECK(g::bufferUploadStats(ticket).uploadedBytes == 16);
    const auto handles = g::adoptBufferUpload(ticket);
    REQUIRE(handles.indices.size() == 1);
    CHECK(g::isValid(handles.indices[0]));
    CHECK_THROWS_WITH(g::adoptBufferUpload(ticket), "GPU upload was already registered.");
    g::destroy(handles.indices[0]);
}

TEST_CASE("Queued GPU uploads can cancel while the worker owns an earlier request") {
    namespace g = woby::graphics;
    UploadFixture fixture;
    std::promise<void> entered, release;
    auto ready = release.get_future().share();
    auto first = g::beginBufferUpload([&entered, ready](const std::atomic<bool>&) {
        entered.set_value(); ready.wait(); return std::vector<g::BufferUploadSource>{};
    });
    entered.get_future().wait();
    auto input = source();
    std::weak_ptr<const void> owner = input.owner;
    auto second = g::beginBufferUpload(std::vector{input});
    input = {};
    g::cancelBufferUpload(second);
    CHECK_FALSE(g::bufferUploadComplete(second));
    release.set_value();
    g::waitBufferUpload(first); g::waitBufferUpload(second);
    CHECK(owner.expired());
    CHECK(g::bufferUploadStats(second).uploadedBytes == 0);
    CHECK_THROWS_WITH(g::adoptBufferUpload(second), "GPU upload was canceled.");
    CHECK(g::submittedFrameToken() == 0);
}

TEST_CASE("GPU upload preparation runs on the worker and isolates request failures") {
    namespace g = woby::graphics;
    UploadFixture fixture;
    const auto caller = std::this_thread::get_id();
    std::thread::id preparing;
    auto first = g::beginBufferUpload([&preparing](const std::atomic<bool>&) -> std::vector<g::BufferUploadSource> {
        preparing = std::this_thread::get_id(); throw std::runtime_error("fixture preparation failed");
    });
    auto second = g::beginBufferUpload(std::vector{source()});
    g::waitBufferUpload(first); g::waitBufferUpload(second);
    CHECK(preparing != caller);
    CHECK_THROWS_WITH(g::adoptBufferUpload(first), "fixture preparation failed");
    const auto handles = g::adoptBufferUpload(second);
    REQUIRE(handles.indices.size() == 1);
    g::destroy(handles.indices[0]);
}

TEST_CASE("GPU uploader shutdown cancels active preparation and rejects previous device results") {
    namespace g = woby::graphics;
    UploadFixture fixture;
    std::promise<void> entered;
    auto ticket = g::beginBufferUpload([&entered](const std::atomic<bool>& canceled) {
        entered.set_value(); canceled.wait(false); return std::vector<g::BufferUploadSource>{};
    });
    entered.get_future().wait();
    g::shutdown();
    CHECK(g::bufferUploadComplete(ticket));
    g::Init options; options.type = g::RendererType::Noop;
    REQUIRE(g::init(options));
    CHECK_THROWS_WITH(g::adoptBufferUpload(ticket), "GPU upload belongs to another renderer.");
}

TEST_CASE("GPU upload validation rejects unowned bytes and invalid staging capacities") {
    namespace g = woby::graphics;
    UploadFixture fixture;
    auto input = source(); input.owner.reset();
    CHECK_THROWS_AS(g::beginBufferUpload(std::vector{input}), std::invalid_argument);
    input = source(); input.bytes = 15;
    CHECK_THROWS_AS(g::beginBufferUpload(std::vector{input}), std::invalid_argument);
    g::shutdown();
    for (const uint64_t bytes : {0ull, 3ull * 1024 * 1024, 257ull * 1024 * 1024, 4ull * 1024 * 1024 + 1}) {
        g::Init options; options.type = g::RendererType::Noop; options.modelUploadStagingBytes = bytes;
        CHECK_FALSE(g::init(options));
    }
}
