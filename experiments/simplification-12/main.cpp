#include "graphics.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
namespace g = woby::graphics;
using Clock = std::chrono::steady_clock;
using Vec4 = std::array<float,4>;
struct TypedPacket {
    Vec4 color{}, point{}, uv{};
    uint32_t pointOffset=0, markerFirst=0, markerSourceFirst=0, triangleOffset=0;
    float edgeHalfWidth=0;
    bool fill=false, compact=false;
};
struct TypedRoot { TypedPacket packet; };
#if defined(_MSC_VER)
#define WOBY_PROBE_NOINLINE __declspec(noinline)
#else
#define WOBY_PROBE_NOINLINE __attribute__((noinline))
#endif
// An out-of-line setter stops the timed loop from replacing repeated writes
// with the final write. This candidate has no handle or name dispatch.
WOBY_PROBE_NOINLINE void setTyped(TypedRoot& root, const TypedPacket& packet) { root.packet=packet; }
struct LegacyPacket { Vec4 color{}, points[2]{}, uv{}, edges{}, marker{}; };
LegacyPacket encode(const TypedPacket& p) {
    return {p.color, {Vec4{p.point[0],p.point[1],p.point[2],p.compact ? 1.0f : 0.0f},
        Vec4{float(p.pointOffset&65535u),float(p.pointOffset>>16u),float(p.markerSourceFirst&65535u),float(p.markerSourceFirst>>16u)}},
        p.uv, {p.edgeHalfWidth,p.fill ? 1.0f : 0.0f,float(p.triangleOffset&65535u),float(p.triangleOffset>>16u)},
        {float(p.markerFirst&65535u),float(p.markerFirst>>16u),0,0}};
}
uint32_t decode(float low,float high) { return uint32_t(low)|(uint32_t(high)<<16u); }
void check(bool ok,const char* error) { if(!ok) throw std::runtime_error(error); }
template<class F> double measure(F&& f,size_t rounds) {
    const auto begin=Clock::now();
    for(size_t i=0;i<rounds;++i) f(i);
    return std::chrono::duration<double,std::milli>(Clock::now()-begin).count()/double(rounds);
}
void selfTest() {
    for(uint32_t value : {0u,1u,65535u,65536u,16777217u,2147483648u,4294967295u}) {
        TypedPacket p; p.pointOffset=p.markerFirst=p.markerSourceFirst=p.triangleOffset=value;
        p.color={.1f,.2f,.3f,.4f}; p.point={3,1920,1080,0}; p.uv={10,20,1,0};
        p.edgeHalfWidth=2.5f; p.fill=true; p.compact=true;
        const auto e=encode(p);
        check(decode(e.points[1][0],e.points[1][1])==value,"Point-offset encoding mismatch");
        check(decode(e.points[1][2],e.points[1][3])==value,"Source-ID encoding mismatch");
        check(decode(e.marker[0],e.marker[1])==value,"Marker-ID encoding mismatch");
        check(decode(e.edges[2],e.edges[3])==value,"Triangle-offset encoding mismatch");
        TypedRoot root; setTyped(root,p);
        check(root.packet.markerFirst==value && root.packet.pointOffset==value && root.packet.markerSourceFirst==value
            && root.packet.triangleOffset==value,"Typed ID mismatch");
        check(root.packet.color==p.color && root.packet.point==p.point && root.packet.uv==p.uv
            && root.packet.edgeHalfWidth==p.edgeHalfWidth && root.packet.fill==p.fill && root.packet.compact==p.compact,
            "Typed field mismatch");
        check(e.color==p.color && e.uv==p.uv && e.points[0][0]==p.point[0] && e.points[0][1]==p.point[1]
            && e.points[0][2]==p.point[2] && e.points[0][3]==1 && e.edges[0]==p.edgeHalfWidth && e.edges[1]==1,
            "Legacy/typed parameter mismatch");
    }
    g::Init init; init.type=g::RendererType::Noop; check(g::init(init),"Noop init failed");
    const auto color=g::createUniform("u_color",g::UniformType::Vec4);
    const Vec4 v{}; g::setUniform(color,v.data()); g::destroy(color); g::shutdown();
}
}
int main(int argc,char** argv) {
    try {
        selfTest();
        if(argc>1 && std::string_view(argv[1])=="--self-test") { std::cout<<"{\"ok\":true}\n"; return 0; }
        g::Init init; init.type=g::RendererType::Noop; check(g::init(init),"Noop init failed");
        const auto color=g::createUniform("u_color",g::UniformType::Vec4);
        const auto points=g::createUniform("u_pointParams",g::UniformType::Vec4,2);
        const auto uv=g::createUniform("u_uvGrid",g::UniformType::Vec4);
        const auto edges=g::createUniform("u_triangleEdges",g::UniformType::Vec4);
        const auto marker=g::createUniform("u_markerBase",g::UniformType::Vec4);
        std::vector<TypedPacket> packets(20000);
        std::vector<LegacyPacket> legacy; legacy.reserve(packets.size());
        for(size_t i=0;i<packets.size();++i) {
            auto& p=packets[i]; p.color={float(i%100)/100,.6f,.8f,1}; p.point={4,1920,1080,0};
            p.pointOffset=uint32_t(i*123457); p.markerFirst=uint32_t(i*123459+1); p.markerSourceFirst=uint32_t(i*123461);
            p.triangleOffset=uint32_t(i*123463); p.edgeHalfWidth=1.5f; p.fill=true; p.compact=true;
            legacy.push_back(encode(p));
        }
        TypedRoot typed;
        nlohmann::json rows=nlohmann::json::array();
        for(size_t draws : {size_t{10},size_t{1000},size_t{20000}}) {
            const size_t rounds=std::max(size_t{30},size_t{4000000}/draws);
            const auto production=[&](size_t rotation) {
                for(size_t i=0;i<draws;++i) {
                    const auto& p=legacy[(i+rotation)%packets.size()];
                    g::setUniform(color,p.color.data()); g::setUniform(points,p.points,2);
                    g::setUniform(uv,p.uv.data()); g::setUniform(edges,p.edges.data()); g::setUniform(marker,p.marker.data());
                }
            };
            const auto candidate=[&](size_t rotation) {
                for(size_t i=0;i<draws;++i) setTyped(typed,packets[(i+rotation)%packets.size()]);
            };
            production(0); candidate(0);
            nlohmann::json samples=nlohmann::json::array();
            for(size_t repeat=0;repeat<5;++repeat) {
                double baseline=0,proposal=0;
                if(repeat%2==0) { baseline=measure(production,rounds); proposal=measure(candidate,rounds); }
                else { proposal=measure(candidate,rounds); baseline=measure(production,rounds); }
                samples.push_back({{"production_five_writes_ms",baseline},{"typed_packet_ms",proposal}});
            }
            rows.push_back({{"draw_packets",draws},{"writes_per_packet",5},{"rounds",rounds},{"samples",samples}});
        }
        std::cout<<nlohmann::json{{"ok",true},{"candidate_packet_size",sizeof(TypedPacket)},
            {"legacy_payload_bytes_per_packet",sizeof(LegacyPacket)},{"checksum",typed.packet.markerFirst},{"rows",rows},
            {"scope","Actual production Noop setUniform versus an isolated typed packet setter; no draws, GPU work, root staging or complete frame."}}.dump()<<'\n';
        g::shutdown(); return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
