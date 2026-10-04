#include "performance_capture.h"
#include <doctest/doctest.h>

TEST_CASE("frame capture excludes setup, bounds memory, and reports every dropped event")
{
    woby::FrameCapture capture;
    woby::FrameEvent event;
    woby::captureFrame(capture, event);
    CHECK(capture.events.empty());
    woby::beginFrameCapture(capture, 10, {});
    CHECK_THROWS(woby::beginFrameCapture(capture, 11, {}));
    event.timing.frameIndex = 10;
    woby::captureFrame(capture, event);
    CHECK(capture.events.empty());
    for (size_t i = 0; i < woby::maxCapturedFrames + 3; ++i) {
        event.timing.frameIndex = 11 + i;
        event.completedMilliseconds = static_cast<double>(i);
        woby::captureFrame(capture, event);
    }
    REQUIRE(capture.events.size() == woby::maxCapturedFrames);
    CHECK(capture.droppedFrames == 3);
    CHECK(capture.events.front().timing.frameIndex == 11);
    CHECK(capture.events.back().timing.frameIndex == 10 + woby::maxCapturedFrames);
    woby::endFrameCapture(capture);
    CHECK_THROWS(woby::endFrameCapture(capture));
    woby::captureFrame(capture, event);
    CHECK(capture.droppedFrames == 3);
    woby::beginFrameCapture(capture, 100000, {});
    CHECK(capture.events.empty());
    CHECK(capture.droppedFrames == 0);
}

TEST_CASE("frame events preserve transient size, presentation, scene and camera changes")
{
    woby::FrameCapture capture;
    woby::beginFrameCapture(capture, 0, {});
    woby::FrameEvent event;
    event.timing.frameIndex = 1;
    event.environment.drawableWidth = 1280;
    woby::captureFrame(capture, event);
    event.timing.frameIndex = 2;
    event.environment.drawableWidth = 640;
    event.environment.sceneEditRevision = 1;
    event.environment.presentationSubmitted = true;
    event.camera[0] = 42;
    woby::captureFrame(capture, event);
    REQUIRE(capture.events.size() == 2);
    CHECK(capture.events[0].environment != capture.events[1].environment);
    CHECK(capture.events[0].camera[0] == 0);
    CHECK(capture.events[1].camera[0] == 42);
}
