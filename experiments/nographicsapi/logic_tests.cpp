#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "logic.h"
#include <limits>
using namespace woby::ng;
TEST_CASE("viewport picking handles offsets and independent DPI scales") {
    const woby::SceneViewport viewport{18,200,120,21};
    CHECK_FALSE(cursorPixel(8,20,2,1.5f,viewport));
    const auto p = cursorPixel(10,20,2,1.5f,viewport);
    REQUIRE(p);
    CHECK(p->x == 2); CHECK(p->y == 9);
    CHECK_FALSE(cursorPixel(109,20,2,1.5f,viewport));
    CHECK_FALSE(cursorPixel(std::numeric_limits<float>::infinity(),20,2,1.5f,viewport));
}
TEST_CASE("ImGui scissor floors origins, ceils ends, clamps and rejects empty rectangles") {
    const auto clip = clipRect(8,17,80.2f,100,10,20,2,1.5f,100,80);
    CHECK(clip.x == 0); CHECK(clip.y == 0); CHECK(clip.width == 100); CHECK(clip.height == 80);
    CHECK(clipRect(200,0,300,10,0,0,1,1,100,100).width == 0);
    CHECK(clipRect(20,0,10,10,0,0,1,1,100,100).width == 0);
}
TEST_CASE("pending selections cannot cross scene replacement or arrive out of order") {
    Selection selection;
    CHECK(completePick(selection,1,4,frontId));
    CHECK(selection.id == frontId);
    CHECK_FALSE(completePick(selection,1,3,rearId));
    invalidateSelection(selection);
    CHECK(selection.id == 0);
    CHECK_FALSE(completePick(selection,1,5,frontId));
    CHECK(completePick(selection,2,6,rearId));
    CHECK(selection.accepted == 2); CHECK(selection.rejected == 2);
}
TEST_CASE("fixtures use woby meshes and exact large ID ranges") {
    auto fixture = makeFixture(Scenario::translucent);
    REQUIRE(fixture.state.files.size() == 3);
    REQUIRE(fixture.markers.draws.size() == 2);
    CHECK(fixture.state.files[1].groupSettings[0].opacity == doctest::Approx(.35f));
    auto* draw = woby::findMarkerDraw(fixture.markers.draws, frontId + 3);
    REQUIRE(draw);
    CHECK(draw->fileIndex == 1);
    CHECK(expectedPick(Scenario::hidden) == rearId);
    CHECK(expectedPick(Scenario::occluded) == 0);
    CHECK_THROWS_AS(expectedPick(Scenario::count), std::invalid_argument);
}
