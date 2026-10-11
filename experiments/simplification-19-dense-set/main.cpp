#include "dense_set_index.h"
#include "tracked_custom_index.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <string_view>

namespace probe {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using BigBucket = ankerl::unordered_dense::bucket_type::big;
void require(bool value, const char* message) { if(!value) { throw std::runtime_error(message); } }
template<class T> std::vector<std::array<T,3>> inputKeys(size_t count) {
    std::vector<std::array<T,3>> result; result.reserve(count);
    for(size_t i=0;i<count;++i) { result.push_back({static_cast<T>(i),static_cast<T>(i%4093),static_cast<T>(i%131)}); }
    std::mt19937 generator(417); std::shuffle(result.begin(),result.end(),generator); return result;
}
template<class T, class Index> Json measure(const std::vector<std::array<T,3>>& input) {
    const auto startMemory = woby::dense_set_allocations::start();
    size_t retained=0,peak=0,total=0,calls=0,keyCapacity=0,buckets=0,bucketBytes=0,checksum=0;
    double milliseconds=0;
    {
        Index index;
        const auto begin = Clock::now();
        woby::reserveAnalysisIndex(index,input.size());
        for(unsigned pass=0;pass<4;++pass) {
            for(const auto& key:input) { checksum += woby::analysisIndex(index,key); }
        }
        milliseconds=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        require(index.keys.size()==input.size(),"Index cardinality changed");
        require(std::equal(index.keys.begin(),index.keys.end(),input.begin(),input.end()),"First-use key order changed");
        require(checksum==4*(input.size()*(input.size()-1)/2),"ID checksum changed");
        keyCapacity=index.keys.capacity();
        if constexpr(requires { index.buckets; }) {
            buckets=index.buckets.capacity(); bucketBytes=sizeof(size_t);
        } else {
            using Set = std::remove_reference_t<decltype(index.keys.lookup)>;
            buckets=index.keys.lookup.bucket_count(); bucketBytes=sizeof(typename Set::bucket_type);
        }
        retained=woby::dense_set_allocations::live.load()-startMemory.liveBytes;
        peak=woby::dense_set_allocations::peak.load()-startMemory.liveBytes;
        total=woby::dense_set_allocations::total.load()-startMemory.totalBytes;
        calls=woby::dense_set_allocations::calls.load()-startMemory.callCount;
    }
    require(woby::dense_set_allocations::live.load()==startMemory.liveBytes,"Measured index leaked requested allocations");
    return Json{{"build_plus_3_hit_passes_ms",milliseconds},{"checksum",checksum},
        {"retained_requested_bytes",retained},{"peak_requested_bytes",peak},
        {"total_requested_bytes",total},{"allocation_calls",calls},
        {"key_capacity",keyCapacity},{"bucket_capacity",buckets},{"bucket_record_bytes",bucketBytes}};
}
template<class T> Json run(size_t count, unsigned samples) {
    const auto input=inputKeys<T>(count);
    using Custom=woby::TrackedCustomAnalysisIndex<T,3>;
    using Dense=woby::DenseSetAnalysisIndex<T,3>;
    using Big=woby::DenseSetAnalysisIndex<T,3,BigBucket>;
    using Find=woby::DenseSetAnalysisIndex<T,3,ankerl::unordered_dense::bucket_type::standard,true>;
    (void)measure<T,Custom>(input); (void)measure<T,Dense>(input); (void)measure<T,Big>(input); (void)measure<T,Find>(input);
    Json rows=Json::array();
    for(unsigned sample=0;sample<samples;++sample) {
        Json custom,dense,big,find;
        if(sample%4==0) {custom=measure<T,Custom>(input);dense=measure<T,Dense>(input);big=measure<T,Big>(input);find=measure<T,Find>(input);}
        else if(sample%4==1) {dense=measure<T,Dense>(input);big=measure<T,Big>(input);find=measure<T,Find>(input);custom=measure<T,Custom>(input);}
        else if(sample%4==2) {big=measure<T,Big>(input);find=measure<T,Find>(input);custom=measure<T,Custom>(input);dense=measure<T,Dense>(input);}
        else {find=measure<T,Find>(input);custom=measure<T,Custom>(input);dense=measure<T,Dense>(input);big=measure<T,Big>(input);}
        rows.push_back({{"sample",sample},{"first_backend",sample%4},
            {"custom",custom},{"dense_set",dense},{"dense_set_big",big},{"dense_set_find_first",find}});
    }
    return Json{{"entries",count},{"samples",rows}};
}
template<class T,size_t N,class Bucket,bool FindFirst=false> size_t contract() {
    using Key=std::array<T,N>;
    using Index=woby::DenseSetAnalysisIndex<T,N,Bucket,FindFirst>;
    woby::AnalysisIndex<T,N> expected; Index actual;
    woby::reserveAnalysisIndex(expected,2); woby::reserveAnalysisIndex(actual,2);
    std::vector<Key> first;
    for(size_t i=0;i<1000;++i) {
        Key key{}; for(size_t j=0;j<N;++j) {key[j]=static_cast<T>(i*(j+1));}
        require(woby::analysisIndex(expected,key)==woby::analysisIndex(actual,key),"Growth ID changed");
        first.push_back(key);
        const auto* stable=actual.keys.data();
        const auto calls=woby::dense_set_allocations::calls.load();
        require(woby::analysisIndex(actual,key)==i,"Growth hit ID changed");
        require(actual.keys.data()==stable&&woby::dense_set_allocations::calls.load()==calls,"Growth hit allocates or invalidates keys");
    }
    require(actual.keys.size()==first.size(),"Growth cardinality changed");
    require(std::equal(actual.keys.begin(),actual.keys.end(),first.begin(),first.end()),"Growth key order changed");
    woby::reserveAnalysisIndex(actual,4000); woby::reserveAnalysisIndex(expected,4000);
    for(size_t i=0;i<first.size();++i) {require(woby::analysisIndex(actual,first[i])==i,"Reserve changed ID");}
    require(std::equal(actual.keys.begin(),actual.keys.end(),first.begin(),first.end()),"Reserve key order changed");
    Index copied=actual; require(copied.keys.data()!=actual.keys.data(),"Copy retains another owner's keys");
    Index assigned; assigned=actual;
    const auto* copyAddress=copied.keys.data(); const auto* assignedAddress=assigned.keys.data();
    const auto copyCalls=woby::dense_set_allocations::calls.load();
    require(woby::analysisIndex(copied,first[0])==0&&woby::analysisIndex(assigned,first[0])==0,"Copied hit ID changed");
    require(copied.keys.data()==copyAddress&&assigned.keys.data()==assignedAddress
        &&woby::dense_set_allocations::calls.load()==copyCalls,"Copied hit allocates or invalidates keys");
    Key added{}; added.fill(static_cast<T>(123456)); (void)woby::analysisIndex(actual,added);
    require(copied.keys.size()==first.size()&&assigned.keys.size()==first.size(),"Copy mutation changed another owner");
    Index moved=std::move(copied); Index moveAssigned; moveAssigned=std::move(assigned);
    require(std::equal(moved.keys.begin(),moved.keys.end(),first.begin(),first.end()),"Move changed keys");
    require(std::equal(moveAssigned.keys.begin(),moveAssigned.keys.end(),first.begin(),first.end()),"Move assignment changed keys");
    const auto* movedAddress=moved.keys.data(); const auto* moveAssignedAddress=moveAssigned.keys.data();
    const auto movedCalls=woby::dense_set_allocations::calls.load();
    for(size_t i=0;i<first.size();++i) {
        require(woby::analysisIndex(moved,first[i])==i&&woby::analysisIndex(moveAssigned,first[i])==i,"Copied/moved lookup changed");
    }
    require(moved.keys.data()==movedAddress&&moveAssigned.keys.data()==moveAssignedAddress
        &&woby::dense_set_allocations::calls.load()==movedCalls,"Moved hit allocates or invalidates keys");
    require(woby::analysisIndex(copied,added)==0&&woby::analysisIndex(assigned,added)==0,"Moved-from index is not reusable");
    bool rejected=false;
    try {woby::reserveAnalysisIndex(actual,woby::denseSetCapacityLimit<T,N,Bucket>()+1);}
    catch(const std::length_error&) {rejected=true;}
    require(rejected&&actual.keys.size()==1001,"Capacity preflight failed or changed keys");
    return first.size();
}
template<class Bucket> void floatingContract() {
    woby::AnalysisIndex<double,3> expected; woby::DenseSetAnalysisIndex<double,3,Bucket> actual;
    const double tiny=std::numeric_limits<double>::denorm_min(),large=std::numeric_limits<double>::max();
    const std::array<std::array<double,3>,6> keys{{{-0.,0.,-0.},{0.,-0.,0.},{tiny,-tiny,0.},
        {large,-large,1.},{std::nextafter(1.,2.),1.,0.},{1.,std::nextafter(1.,2.),0.}}};
    for(const auto& key:keys) { require(woby::analysisIndex(expected,key)==woby::analysisIndex(actual,key),"Finite/signed-zero ID changed"); }
    require(actual.keys.size()==5,"Signed-zero cardinality changed");
    for(size_t i=0;i<expected.keys.size();++i) {
        for(size_t j=0;j<3;++j) {require(std::bit_cast<uint64_t>(expected.keys[i][j])==std::bit_cast<uint64_t>(actual.keys[i][j]),"First key bits changed");}
    }
}
Json selfTest() {
    size_t records=0;
    records+=contract<double,3,ankerl::unordered_dense::bucket_type::standard>();
    records+=contract<double,4,ankerl::unordered_dense::bucket_type::standard>();
    records+=contract<double,16,ankerl::unordered_dense::bucket_type::standard>();
    records+=contract<size_t,2,ankerl::unordered_dense::bucket_type::standard>();
    records+=contract<uint64_t,3,ankerl::unordered_dense::bucket_type::standard>();
    records+=contract<double,3,BigBucket>(); records+=contract<size_t,3,BigBucket>();
    records+=contract<double,3,ankerl::unordered_dense::bucket_type::standard,true>();
    records+=contract<size_t,3,ankerl::unordered_dense::bucket_type::standard,true>();
    floatingContract<ankerl::unordered_dense::bucket_type::standard>(); floatingContract<BigBucket>();
    (void)run<double>(256,1); (void)run<size_t>(256,1);
    return Json{{"ok",true},{"growth_records_compared",records},{"signed_zero_and_finite_contracts",2},
        {"default_capacity_limit",woby::denseSetCapacityLimit<double,3,ankerl::unordered_dense::bucket_type::standard>()},
        {"big_capacity_limit",woby::denseSetCapacityLimit<double,3,BigBucket>()}};
}
}
int main(int argc,char** argv) {
    try {
        using namespace probe; const auto contracts=selfTest();
        if(argc==2&&std::string_view(argv[1])=="--self-test") {std::cout<<contracts.dump()<<'\n';return 0;}
        require(argc==1,"Usage: woby_analysis_index_dense_set_probe [--self-test]");
        Json result{{"contracts",contracts},{"coordinate",{run<double>(4096,9),run<double>(524288,9)}},
            {"integer_tuple",{run<size_t>(4096,9),run<size_t>(524288,9)}},
            {"notes","One warmup per backend; nine rotated four-backend samples. Same production hasher with default library mix, fixed shuffle seed417, one insertion and three hit passes. Index construction/destruction and full ordered-key validation excluded from timing. Matched tracking allocator measures only index allocation requests, including construction and dense bucket rebinds; input keys, allocator overhead and process memory excluded. Allocation-event counters affect both micro and pipeline timings. Custom hash/probing source is extracted unchanged with allocator/type-name substitutions. Sets retain each key once. Append strategy reserves one spare and maintains it with geometric growth; default find-first alternative avoids temporary hit insertion but repeats lookup on misses. Default 32-bit value-index guard rejects unsupported sizes; a production port must retain the custom wide fallback."}};
        std::cout<<result.dump(2)<<'\n';return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
