#include "model_mesh.h"
#include "utf8_path.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace woby;
using Clock = std::chrono::steady_clock;
static_assert(sizeof(Vertex) == 32);
struct AllocationTrace { size_t live=0, peak=0, calls=0; };
template<class T> struct TraceAllocator {
    using value_type = T;
    AllocationTrace* trace = nullptr;
    TraceAllocator() = default;
    explicit TraceAllocator(AllocationTrace& value): trace(&value) {}
    template<class U> TraceAllocator(const TraceAllocator<U>& other): trace(other.trace) {}
    T* allocate(size_t count) {
        auto* result = std::allocator<T>{}.allocate(count);
        if(trace) { trace->live += count*sizeof(T); trace->peak = std::max(trace->peak,trace->live); ++trace->calls; }
        return result;
    }
    void deallocate(T* value,size_t count) {
        if(trace) { trace->live -= count*sizeof(T); }
        std::allocator<T>{}.deallocate(value,count);
    }
    template<class U> bool operator==(const TraceAllocator<U>& other) const { return trace==other.trace; }
};
using Buffer = std::vector<Vertex,TraceAllocator<Vertex>>;
void check(bool condition,const char* message) { if(!condition) { throw std::runtime_error(message); } }
Vertex expectedVertex(size_t index) {
    return {{float(index%1024),float((index*3)%1024),float((index*7)%1024)},
        {0,0,1},{float(index%997)/997.0f,float(index%991)/991.0f}};
}
uint64_t validate(const Buffer& values) {
    uint64_t checksum=14695981039346656037ull;
    for(size_t index=0;index<values.size();++index) {
        const auto expected=expectedVertex(index);
        check(std::memcmp(&values[index],&expected,sizeof(Vertex))==0,"Shrink changed vertex bytes");
        std::array<uint32_t,8> words;
        std::memcpy(words.data(),&values[index],sizeof(Vertex));
        for(const auto word:words) { checksum=(checksum^word)*1099511628211ull; }
    }
    return checksum;
}
nlohmann::json shrink(size_t count,size_t capacity,uint64_t& checksum) {
    check(capacity>=count,"Invalid input capacity");
    AllocationTrace trace;
    nlohmann::json result;
    {
        Buffer values{TraceAllocator<Vertex>(trace)};
        values.reserve(capacity); values.resize(count);
        for(size_t index=0;index<count;++index) { values[index]=expectedVertex(index); }
        const auto beforeChecksum=validate(values);
        const auto oldCapacity=values.capacity(), beforeLive=trace.live, beforeCalls=trace.calls;
        const auto begin=Clock::now();
        values.shrink_to_fit();
        const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        check(values.size()==count,"Shrink changed size");
        check(values.capacity()==count,"This library did not trim to exact size");
        const auto afterChecksum=validate(values);
        check(beforeChecksum==afterChecksum,"Shrink checksum mismatch");
        checksum^=afterChecksum;
        result={{"shrink_ms",elapsed},{"size",count},{"old_capacity",oldCapacity},{"new_capacity",values.capacity()},
            {"old_vertex_allocation_bytes",oldCapacity*sizeof(Vertex)},
            {"new_vertex_allocation_bytes",values.capacity()*sizeof(Vertex)},
            {"vertex_allocation_saved_bytes",(oldCapacity-values.capacity())*sizeof(Vertex)},
            {"requested_live_before_bytes",beforeLive},{"requested_live_after_bytes",trace.live},
            {"requested_peak_bytes",trace.peak},{"trim_allocation_calls",trace.calls-beforeCalls},
            {"checksum",afterChecksum}};
    }
    check(trace.live==0,"Allocation tracking did not return to zero");
    return result;
}
void selfTest() {
    uint64_t checksum=0;
    const auto trimmed=shrink(7,19,checksum);
    check(trimmed.at("vertex_allocation_saved_bytes")==12*sizeof(Vertex),"Trim allocation count mismatch");
    const auto unchanged=shrink(19,19,checksum);
    check(unchanged.at("vertex_allocation_saved_bytes")==0,"Exact capacity changed");
    check(unchanged.at("trim_allocation_calls")==0,"Exact capacity unexpectedly allocated");
    const auto empty=shrink(0,19,checksum);
    check(empty.at("new_capacity")==0,"Empty trim retained payload");
}
}
int main(int argc,char** argv) {
    try {
        selfTest();
        if(argc>1 && std::string_view(argv[1])=="--self-test") { std::cout<<"{\"ok\":true}\n"; return 0; }
        check(argc==2,"usage: woby_simplification_14_capacity ABSOLUTE_RAW_RESULTS_JSON");
        const auto path=pathFromUtf8(argv[1]); check(path.is_absolute(),"Absolute input path required");
        std::ifstream input(path); check(bool(input),"Could not open raw results");
        nlohmann::json raw; input>>raw; check(raw.at("ok").get<bool>(),"Input results failed");
        nlohmann::json rows=nlohmann::json::array(); uint64_t checksum=0;
        for(const auto& record:raw.at("records")) {
            const size_t size=record.at("render_vertices").get<size_t>();
            const size_t capacityBytes=record.at("current_vertex_capacity_bytes").get<size_t>();
            check(capacityBytes%sizeof(Vertex)==0,"Input capacity is not a vertex multiple");
            const size_t capacity=capacityBytes/sizeof(Vertex);
            nlohmann::json samples=nlohmann::json::array();
            for(size_t repeat=0;repeat<5;++repeat) { samples.push_back(shrink(size,capacity,checksum)); }
            rows.push_back({{"model",record.at("path")},{"size",size},{"capacity",capacity},{"samples",samples}});
        }
        std::cout<<nlohmann::json{{"ok",true},{"sizeof_vertex",sizeof(Vertex)},{"rows",rows},{"checksum",checksum},
            {"scope","Synthetic buffers at measured model size/capacity. Only shrink_to_fit allocation/copy/free is timed; filling and full byte/checksum validation are outside timers. Custom allocator tracks requested bytes including any allocator rebinding, excluding allocator overhead/process RSS. No complete loader timing."}}.dump()<<'\n';
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
