#include "analysis_index.h"
#include <boost/unordered/unordered_flat_map.hpp>
#if defined(WOBY_PROBE_ANKERL)
#include <ankerl/unordered_dense.h>
#endif
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>

namespace probe {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
struct Allocation { size_t live=0,peak=0,total=0,calls=0; };
template<class T> struct TrackingAllocator {
    using value_type=T;
    Allocation* record=nullptr;
    TrackingAllocator()=default;
    explicit TrackingAllocator(Allocation& r):record(&r){}
    template<class U> TrackingAllocator(const TrackingAllocator<U>& other):record(other.record){}
    T* allocate(size_t n) {
        auto* p=std::allocator<T>{}.allocate(n);
        if(record) {record->live+=n*sizeof(T);record->peak=std::max(record->peak,record->live);record->total+=n*sizeof(T);++record->calls;}
        return p;
    }
    void deallocate(T* p,size_t n) {if(record) {record->live-=n*sizeof(T);}std::allocator<T>{}.deallocate(p,n);}
    template<class U> bool operator==(const TrackingAllocator<U>& other) const {return record==other.record;}
};
template<class T> struct Hasher {size_t operator()(const std::array<T,3>& key) const {return woby::analysisKeyHash(key);}};
void require(bool b,const char* m) {if(!b) {throw std::runtime_error(m);}}
template<class T> std::vector<std::array<T,3>> keys(size_t n) {
    std::vector<std::array<T,3>> values;values.reserve(n);
    for(size_t i=0;i<n;++i) {values.push_back({T(i),T(i%4093),T(i%131)});}
    std::mt19937 rng(417);std::shuffle(values.begin(),values.end(),rng);return values;
}
template<class T> Json custom(const std::vector<std::array<T,3>>& input,unsigned passes) {
    woby::AnalysisIndex<T,3> index;const auto start=Clock::now();woby::reserveAnalysisIndex(index,input.size());
    size_t checksum=0;for(unsigned pass=0;pass<passes;++pass) {for(const auto& k:input) {checksum+=woby::analysisIndex(index,k);}}
    const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    require(index.keys.size()==input.size(),"Custom cardinality changed");
    return Json{{"build_plus_3_hit_passes_ms",ms},{"checksum",checksum},{"retained_requested_bytes",index.keys.capacity()*sizeof(std::array<T,3>)+index.buckets.capacity()*sizeof(size_t)},
        {"key_capacity",index.keys.capacity()},{"bucket_capacity",index.buckets.capacity()}};
}
template<class T,bool Dense=false> Json flat(const std::vector<std::array<T,3>>& input,unsigned passes) {
    using Key=std::array<T,3>;
#if defined(WOBY_PROBE_ANKERL)
    using Pair=std::conditional_t<Dense,std::pair<Key,size_t>,std::pair<const Key,size_t>>;
    using Alloc=TrackingAllocator<Pair>;
    using Map=std::conditional_t<Dense,ankerl::unordered_dense::map<Key,size_t,Hasher<T>,std::equal_to<Key>,Alloc>,
        boost::unordered_flat_map<Key,size_t,Hasher<T>,std::equal_to<Key>,Alloc>>;
#else
    using Pair=std::pair<const Key,size_t>;using Alloc=TrackingAllocator<Pair>;
    using Map=boost::unordered_flat_map<Key,size_t,Hasher<T>,std::equal_to<Key>,Alloc>;
#endif
    Allocation alloc; Json result;
    {
        Map map(0,Hasher<T>{},std::equal_to<Key>{},Alloc(alloc));
        std::vector<Key,TrackingAllocator<Key>> ordered{TrackingAllocator<Key>(alloc)};
        const auto start=Clock::now();map.reserve(input.size());ordered.reserve(input.size());
        size_t checksum=0;for(unsigned pass=0;pass<passes;++pass) {for(const auto& k:input) {
            const auto [it,inserted]=map.try_emplace(k,ordered.size());if(inserted) {ordered.push_back(k);}checksum+=it->second;
        }}
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        require(map.size()==input.size()&&std::equal(ordered.begin(),ordered.end(),input.begin(),input.end()),"Flat map lost first-insertion identity");
        result={{"build_plus_3_hit_passes_ms",ms},{"checksum",checksum},
            {"retained_requested_bytes",alloc.live},
            {"peak_requested_bytes",alloc.peak},
            {"tracked_allocations",alloc.calls},{"total_requested_bytes",alloc.total},{"map_bucket_count",map.bucket_count()},
            {"map_max_load_factor",map.max_load_factor()},
            {"ordered_key_capacity",ordered.capacity()}};
    }
    require(alloc.live==0,"Flat map leaked tracked allocations");return result;
}
template<class T> Json run(size_t n,unsigned samples) {
    const auto input=keys<T>(n);(void)custom(input,4);(void)flat(input,4);Json rows=Json::array();
#if defined(WOBY_PROBE_ANKERL)
    (void)flat<T,true>(input,4);
#endif
    for(unsigned i=0;i<samples;++i) {Json a,b;
#if defined(WOBY_PROBE_ANKERL)
        Json c;
        if(i%3==0) {a=custom(input,4);b=flat(input,4);c=flat<T,true>(input,4);}
        else if(i%3==1) {b=flat(input,4);c=flat<T,true>(input,4);a=custom(input,4);}
        else {c=flat<T,true>(input,4);a=custom(input,4);b=flat(input,4);}
        require(a["checksum"]==c["checksum"],"Dense insertion order checksum changed");
#else
        if(i%2) {b=flat(input,4);a=custom(input,4);}else {a=custom(input,4);b=flat(input,4);}
#endif
        require(a["checksum"]==b["checksum"],"Insertion order checksum changed");Json row{{"custom",a},{"boost_adapter",b}};
#if defined(WOBY_PROBE_ANKERL)
        row["ankerl_adapter"]=c;
#endif
        rows.push_back(std::move(row));
    }
    return Json{{"entries",n},{"samples",rows}};
}
void selfTest() {
    (void)run<double>(256,1);(void)run<size_t>(256,1);
    woby::AnalysisIndex<double,3> index;woby::reserveAnalysisIndex(index,2);
    require(woby::analysisIndex(index,{0.,0.,0.})==woby::analysisIndex(index,{-0.,0.,-0.}),"Signed-zero identity changed");
    require(Hasher<double>{}({0.,0.,0.})==Hasher<double>{}({-0.,0.,-0.}),"Adapter signed-zero hash changed");
    boost::unordered_flat_map<std::array<double,3>,size_t,Hasher<double>> map;
    map.try_emplace({0.,0.,0.},0);const auto [it,inserted]=map.try_emplace({-0.,0.,-0.},1);
    require(!inserted&&it->second==0&&map.size()==1,"Adapter signed-zero identity changed");
#if defined(WOBY_PROBE_ANKERL)
    ankerl::unordered_dense::map<std::array<double,3>,size_t,Hasher<double>> dense;
    dense.try_emplace({0.,0.,0.},0);const auto [entry,added]=dense.try_emplace({-0.,0.,-0.},1);
    require(!added&&entry->second==0&&dense.size()==1,"Dense signed-zero identity changed");
#endif
}
}
int main(int argc,char** argv) {
    try {using namespace probe;selfTest();if(argc==2&&std::string_view(argv[1])=="--self-test") {return 0;}
        require(argc==1,"Usage: woby_analysis_index_probe [--self-test]");
        Json output{{"coordinate",{run<double>(4096,9),run<double>(524288,9)}},{"integer_tuple",{run<size_t>(4096,9),run<size_t>(524288,9)}},
            {"notes","MSVC Release, one warmup, rotated order with three backends or alternating with two. Same production hasher, fixed shuffled input, one insertion and three hit passes. Destruction excluded from timings. Adapters retain insertion-order keys as required by AnalysisIndex consumers. Map plus ordered-vector allocation byte counts are allocator requests, not process RSS or allocator overhead. Input keys excluded. Custom memory is vector capacity; reserve prevents growth."}};
        std::cout<<output.dump(2)<<'\n';
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
