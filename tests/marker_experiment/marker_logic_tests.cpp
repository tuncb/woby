#include "marker_logic.h"
#include <doctest/doctest.h>
#include <limits>
using namespace marker_experiment;
TEST_CASE("marker ID limbs preserve IDs beyond float integer precision") {
    for (uint32_t n : {1u,65535u,65536u,16777217u,40000001u,0xffffffffu}) {
        Pixel p{static_cast<float>(n&65535u),static_cast<float>(n>>16u),.5f,1};
        CHECK(validPixel(p)); CHECK(identity(p)==n);
    }
    CHECK_FALSE(validPixel({1.5f,0,.5f,0}));
    CHECK_FALSE(validPixel({std::numeric_limits<float>::quiet_NaN(),0,.5f,0}));
}
TEST_CASE("marker lookup selects covered MSAA samples by pixel distance then depth and ID") {
    std::array<Pixel,196> p{};
    p[24*4+3]={5,1,.8f,0}; // Only the fourth sample is covered.
    CHECK(identity(selectPixel(p,10.5f,10.5f,32,32,4))==65541);
    CHECK(identity(selectPixel(p,10.5f,10.5f,32,32,1))==0);
    p[23*4]={1,0,.1f,0};
    CHECK(identity(selectPixel(p,10.5f,10.5f,32,32,4))==65541);
    p[24*4+1]={7,0,.2f,0}; p[24*4+2]={6,0,.2f,0};
    CHECK(identity(selectPixel(p,10.5f,10.5f,32,32,4))==6);
}
TEST_CASE("marker lookup clips viewport and circular hover tolerance") {
    std::array<Pixel,196> p{};
    p[0]={1,0,.1f,0};
    CHECK(identity(selectPixel(p,10.5f,10.5f,32,32,4))==0);
    p[23*4]={2,0,.1f,0};
    CHECK(identity(selectPixel(p,.5f,.5f,32,32,4))==0);
    p[24*4]={3,0,.1f,0};
    CHECK(identity(selectPixel(p,.5f,.5f,32,32,4))==3);
}
TEST_CASE("asynchronous marker results reject stale scenarios and out of order completion") {
    CHECK(acceptResult({42,0,.5f,0},12,11,2,2,1)==42);
    CHECK_FALSE(acceptResult({42,0,.5f,0},10,11,2,2,1));
    CHECK_FALSE(acceptResult({42,0,.5f,0},12,11,1,2,1));
    CHECK_FALSE(acceptResult({42,0,.5f,2},12,11,2,2,1));
    CHECK(acceptResult({0,0,0,0},12,11,2,2,1)==0);
    const std::array<Draw,3> draws={Draw{{},20,3},Draw{{},0,2},Draw{{},7,0}};
    CHECK(drawForIdentity(draws,21)==0);
    CHECK(drawForIdentity(draws,23)==0);
    CHECK(drawForIdentity(draws,1)==1);
    CHECK_FALSE(drawForIdentity(draws,0));
    CHECK_FALSE(drawForIdentity(draws,24));
    CHECK_FALSE(drawForIdentity(draws,8));
}
