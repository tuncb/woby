#include <doctest/doctest.h>
#include <future>
#include <stdexcept>

import woby.file_associations;

TEST_CASE("File association module ignores an empty request")
{
    woby::FileAssociationsRuntime runtime;
    woby::startFileAssociationAction(runtime, {});
    CHECK_FALSE(runtime.work.valid());
    CHECK_FALSE(runtime.state.busy);
}

TEST_CASE("File association module publishes a completed worker result")
{
    woby::FileAssociationsRuntime runtime;
    std::promise<woby::FileAssociationsState> work;
    runtime.work = work.get_future();
    runtime.state.busy = true;
    woby::pollFileAssociations(runtime);
    CHECK(runtime.state.busy);
    woby::FileAssociationsState result;
    result.checked = true;
    result.message = "Complete";
    work.set_value(std::move(result));
    woby::pollFileAssociations(runtime);
    CHECK(runtime.state.checked);
    CHECK(runtime.state.message == "Complete");
    CHECK_FALSE(runtime.state.busy);
    CHECK_FALSE(runtime.work.valid());
}

TEST_CASE("File association module publishes worker failures")
{
    woby::FileAssociationsRuntime runtime;
    std::promise<woby::FileAssociationsState> work;
    runtime.work = work.get_future();
    runtime.state.busy = true;
    work.set_exception(std::make_exception_ptr(std::runtime_error("Worker failed")));
    woby::pollFileAssociations(runtime);
    CHECK(runtime.state.failed);
    CHECK(runtime.state.message == "Worker failed");
    CHECK_FALSE(runtime.state.busy);
    CHECK_FALSE(runtime.work.valid());
}
