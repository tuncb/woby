#include "point_cloud.h"
#include "scene_mesh_preparation.h"
#include "spatial_sort.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include "math_utils.h"

namespace woby::points {
namespace {
void require(bool condition,const char* message) { if (!condition) throw std::invalid_argument(message); }
uint32_t spread(uint32_t x) {
    x=(x|(x<<16))&0x030000ffu; x=(x|(x<<8))&0x0300f00fu;
    x=(x|(x<<4))&0x030c30c3u; return (x|(x<<2))&0x09249249u;
}
uint32_t hash(uint32_t x) { x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; return x^(x>>16); }
struct Key { uint32_t morton,id; };
struct Canceled {};
void checkCancel(const std::function<bool()>& cancel) { if (cancel && cancel()) throw Canceled{}; }
void radix(std::vector<Key>& keys, const std::function<bool()>& cancel, unsigned helpers) {
    stableSpatialRadixSort(keys, [](const Key& key) { return key.morton; }, [&] { checkCancel(cancel); }, helpers);
}
uint32_t buildNode(Cloud& cloud,uint32_t begin,uint32_t count,uint32_t group,uint32_t leafSize,uint32_t proxySize,const std::function<bool()>& cancel) {
    checkCancel(cancel);
    const auto index=static_cast<uint32_t>(cloud.nodes.size()); cloud.nodes.emplace_back();
    Node node; node.begin=begin; node.count=count; node.group=group;
    if (count>leafSize) {
        node.left=buildNode(cloud,begin,count/2,group,leafSize,proxySize,cancel);
        node.right=buildNode(cloud,begin+count/2,count-count/2,group,leafSize,proxySize,cancel);
        const auto& a=cloud.nodes[node.left]; const auto& b=cloud.nodes[node.right];
        for (size_t axis=0;axis<3;++axis) {
            node.low[axis]=std::min(a.low[axis],b.low[axis]); node.high[axis]=std::max(a.high[axis],b.high[axis]);
            node.extrema[axis*2]=a.low[axis]<=b.low[axis]?a.extrema[axis*2]:b.extrema[axis*2];
            node.extrema[axis*2+1]=a.high[axis]>=b.high[axis]?a.extrema[axis*2+1]:b.extrema[axis*2+1];
        }
    } else {
        node.low=node.high=cloud.points[begin].position; node.extrema.fill(begin);
        for (uint32_t i=begin+1;i<begin+count;++i) for (size_t axis=0;axis<3;++axis) {
            const float value=cloud.points[i].position[axis];
            if (value<node.low[axis]) { node.low[axis]=value; node.extrema[axis*2]=i; }
            if (value>node.high[axis]) { node.high[axis]=value; node.extrema[axis*2+1]=i; }
        }
    }
    // Stratify in spatial order, plus actual extrema to retain isolated extremities.
    // IDs, rather than averaged positions, remain authoritative at every level.
    std::vector<uint32_t> samples;
    const uint32_t n=std::min(count,proxySize);
    samples.reserve(n+6);
    for (uint32_t i=0;i<n;++i) {
        const auto lo=static_cast<uint32_t>(uint64_t(i)*count/n);
        const auto hi=static_cast<uint32_t>(uint64_t(i+1)*count/n);
        samples.push_back(begin+lo+hash(begin+i)%(hi-lo));
    }
    samples.insert(samples.end(),node.extrema.begin(),node.extrema.end());
    std::sort(samples.begin(),samples.end()); samples.erase(std::unique(samples.begin(),samples.end()),samples.end());
    node.proxyBegin=static_cast<uint32_t>(cloud.proxies.size()); node.proxyCount=static_cast<uint32_t>(samples.size());
    for (const auto sample:samples) cloud.proxies.push_back(cloud.points[sample]);
    cloud.nodes[index]=node; return index;
}
Matrix multiply(const Matrix& model,const Matrix& projection) { return woby::multiplyMatrices(model,projection); }
std::array<float,4> transform(const Matrix& m,const std::array<float,3>& p) {
    std::array<float,4> r{};
    for (size_t i=0;i<4;++i) r[i]=m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2]+m[12+i];
    return r;
}
struct Rectangle { float x0=0,y0=0,x1=0,y1=0,depth=1; bool visible=false; };
Rectangle project(const Node& node,const Matrix& m,uint32_t width,uint32_t height,float pointSize) {
    std::array<std::array<float,4>,8> corners{};
    const float marginX=pointSize/float(width),marginY=pointSize/float(height);
    bool crossing=false;
    for (uint32_t i=0;i<8;++i) {
        corners[i]=transform(m,{(i&1)?node.high[0]:node.low[0],(i&2)?node.high[1]:node.low[1],(i&4)?node.high[2]:node.low[2]});
        for (const auto c:corners[i]) if (!std::isfinite(c)) return {0,0,float(width),float(height),1,true};
        if (corners[i][3]<=0) crossing=true;
    }
    for (uint32_t plane=0;plane<6;++plane) {
        bool outside=true;
        for (const auto& c:corners) {
            const float value=plane==0?c[0]+c[3]*(1+marginX):plane==1?c[3]*(1+marginX)-c[0]
                :plane==2?c[1]+c[3]*(1+marginY):plane==3?c[3]*(1+marginY)-c[1]:plane==4?c[2]:c[3]-c[2];
            if (value>=0) outside=false;
        }
        if (outside) return {};
    }
    if (crossing) return {0,0,float(width),float(height),1,true};
    Rectangle rect{float(width),float(height),0,0,0,true};
    for (const auto& c:corners) {
        const float x=(c[0]/c[3]*.5f+.5f)*float(width),y=(.5f-c[1]/c[3]*.5f)*float(height);
        rect.x0=std::min(rect.x0,x); rect.y0=std::min(rect.y0,y);
        rect.x1=std::max(rect.x1,x); rect.y1=std::max(rect.y1,y); rect.depth=std::max(rect.depth,c[2]/c[3]);
    }
    const float radius=pointSize*.5f;
    rect.x0=std::clamp(rect.x0-radius,0.0f,float(width)); rect.x1=std::clamp(rect.x1+radius,0.0f,float(width));
    rect.y0=std::clamp(rect.y0-radius,0.0f,float(height)); rect.y1=std::clamp(rect.y1+radius,0.0f,float(height));
    return rect;
}
void validateView(const Matrix& projection,uint32_t width,uint32_t height,float pointSize) {
    require(width && height && width<=16384 && height<=16384,"Invalid point viewport");
    require(std::isfinite(pointSize) && pointSize>=1 && pointSize<=16384,"Invalid point footprint");
    for (const auto value:projection) require(std::isfinite(value),"Nonfinite point projection");
}
} // namespace

Cloud buildCloud(const Mesh& mesh,uint32_t leafSize,uint32_t proxySize) {
    if (meshGeometry(mesh).vertices.empty()) return {};
    auto prepared=prepareSceneMesh(mesh,gpuMeshPoints,{},false);
    return std::move(*buildCloud(mesh,std::move(prepared->pointVertexIndices),prepared->nodeRanges,{},leafSize,proxySize));
}
std::optional<Cloud> buildCloud(const Mesh& mesh,std::vector<uint32_t> sourceVertices,
    std::span<const GpuNodeRange> ranges,const std::function<bool()>& shouldCancel,uint32_t leafSize,uint32_t proxySize) {
    // Keep caller callbacks serialized, including callbacks with mutable state.
    std::mutex cancelMutex;
    bool canceled=false;
    const std::function<bool()> cancel = [&] {
        std::lock_guard lock(cancelMutex);
        if (!canceled && shouldCancel) { canceled=shouldCancel(); }
        return canceled;
    };
    require(leafSize>=2 && proxySize>0 && proxySize<=leafSize,"Invalid hierarchy sizes");
    // Match the annotation branch's share of the helper budget. Faster membership
    // can start our key/sort passes before its next pass acquires workers. Without
    // a local cap, that pass can be left on one thread for its whole duration.
    const auto limit = analysisWorkerLimit();
    const auto helpers = limit > 3 && analysisWorkerCount.load(std::memory_order_relaxed) != 0
        ? (limit - 2) / 2 : limit;
    Cloud cloud; cloud.sourceVertices=std::move(sourceVertices);
    require(cloud.sourceVertices.size()<std::numeric_limits<uint32_t>::max(),"Point source IDs exceed 32 bits");
    try {
    checkCancel(cancel);
    const auto& sourceColors = meshGeometry(mesh).vertexColors;
    if (!sourceColors.empty()) {
        require(sourceColors.size() == meshGeometry(mesh).vertices.size(), "Invalid vertex color count");
        cloud.colors.resize(cloud.sourceVertices.size());
        parallelWorkBatches(cloud.sourceVertices.size(), spatialKeyBatchSize, [&] { checkCancel(cancel); },
            [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    require(cloud.sourceVertices[i] < sourceColors.size(), "Invalid colored point vertex");
                    cloud.colors[i] = sourceColors[cloud.sourceVertices[i]];
                }
            }, spatialKeyBatchSize * 2, std::min(helpers, 3u));
    }
    cloud.points.reserve(cloud.sourceVertices.size());
    for (size_t groupIndex=0;groupIndex<ranges.size();++groupIndex) {
        const auto& range=ranges[groupIndex];
        require(range.pointIndexOffset<=cloud.sourceVertices.size()
            && range.pointIndexCount<=cloud.sourceVertices.size()-range.pointIndexOffset,"Invalid point source range");
        const auto first=range.pointIndexOffset+1, end=first+range.pointIndexCount;
        Group group; group.firstId=first; group.endId=end;
        group.color={std::min(1.0f,(.22f+float((groupIndex*37)%53)/100)*1.5f),
            std::min(1.0f,(.28f+float((groupIndex*19)%47)/100)*1.5f),std::min(1.0f,(.3f+float((groupIndex*31)%43)/100)*1.5f),1};
        cloud.groups.push_back(group);
        if (first==end) continue;
        const auto& geometry=meshGeometry(mesh);
        const auto position=[&](uint32_t id)->const std::array<float,3>& {
            const auto vertex=cloud.sourceVertices[id-1];
            require(vertex<geometry.vertices.size(),"Invalid source point vertex");
            return geometry.vertices[vertex].position;
        };
        auto low=position(first), high=low;
        const auto* bounds=range.completeVertexSet && range.pointIndexCount==geometry.vertices.size()
            ? validatedMeshBounds(mesh) : nullptr;
        if (bounds) { low=bounds->min; high=bounds->max; }
        else {
            for (uint32_t id=first;id<end;++id) {
                if ((id&65535)==0) checkCancel(cancel);
                const auto& point=position(id);
                require(finitePosition(point),"Nonfinite source point");
                for (size_t axis=0;axis<3;++axis) {
                    low[axis]=std::min(low[axis],point[axis]); high[axis]=std::max(high[axis],point[axis]);
                }
            }
        }
        std::vector<Key> keys(end-first);
        parallelWorkBatches(keys.size(),spatialKeyBatchSize,[&] { checkCancel(cancel); },[&](size_t begin,size_t finish) {
            for (size_t i=begin;i<finish;++i) {
                const auto id=first+static_cast<uint32_t>(i);
                const auto& point=position(id);
                uint32_t key=0;
                for (uint32_t axis=0;axis<3;++axis) {
                    const double extent=double(high[axis])-low[axis];
                    const double value=extent>0?(double(point[axis])-low[axis])/extent:0;
                    key|=spread(static_cast<uint32_t>(std::clamp(value*1023,0.0,1023.0)))<<axis;
                }
                keys[i]={key,id};
            }
        },spatialKeyBatchSize*2,helpers);
        radix(keys,cancel,helpers);
        const auto begin=static_cast<uint32_t>(cloud.points.size());
        cloud.points.resize(cloud.points.size()+keys.size());
        // Spatial order and original IDs are fixed before workers fill disjoint
        // ranges. Limit helpers so the annotation branch can run at the same time.
        parallelWorkBatches(keys.size(),spatialKeyBatchSize,[&] { checkCancel(cancel); },[&](size_t firstKey,size_t lastKey) {
            for (size_t i=firstKey;i<lastKey;++i) {
                const auto key=keys[i]; cloud.points[begin+i]={position(key.id),key.id};
            }
        },spatialKeyBatchSize*2,std::min(helpers,3u));
        keys.clear(); keys.shrink_to_fit();
        cloud.roots.push_back(buildNode(cloud,begin,end-first,static_cast<uint32_t>(groupIndex),leafSize,proxySize,cancel));
    }
    // Equal tree depth samples by point density, which starves sparse regions.
    // Prepare spatial-error cuts instead, including full sparse leaves. Each
    // group's budgets are independent of the other groups in the source file.
    std::vector<uint32_t> budgets{1,128,256,512,1024,2048};
    for (uint32_t budget=4096;;budget=std::min(2000000u,budget+budget/4)) {
        budgets.push_back(budget); if (budget==2000000) break;
    }
    cloud.navigationCuts.resize(budgets.size());
    struct Candidate { double error; uint32_t node; bool operator<(const Candidate& other) const {
        return error==other.error?node>other.node:error<other.error;
    }};
    const auto candidate=[&](uint32_t index) {
        const auto& n=cloud.nodes[index];
        const double x=double(n.high[0])-n.low[0],y=double(n.high[1])-n.low[1],z=double(n.high[2])-n.low[2];
        return Candidate{(x*y+x*z+y*z)/n.proxyCount,index};
    };
    std::vector<uint8_t> active(cloud.nodes.size());
    for (size_t rootIndex=0;rootIndex<cloud.roots.size();++rootIndex) {
        const auto root=cloud.roots[rootIndex];
        const size_t end=rootIndex+1<cloud.roots.size()?cloud.roots[rootIndex+1]:cloud.nodes.size();
        std::priority_queue<Candidate> queue; queue.push(candidate(root)); active[root]=1;
        uint64_t count=cloud.nodes[root].proxyCount;
        for (size_t level=0;level<budgets.size();++level) {
            checkCancel(cancel);
            if (cloud.nodes[root].count<=budgets[level]) {
                cloud.navigationCuts[level].push_back({root,true}); continue;
            }
            std::vector<Candidate> deferred;
            while (!queue.empty()) {
                const auto next=queue.top(); queue.pop(); const auto& n=cloud.nodes[next.node];
                const auto replacement=n.left?cloud.nodes[n.left].proxyCount+cloud.nodes[n.right].proxyCount:n.count;
                if (count-n.proxyCount+replacement>budgets[level]) { deferred.push_back(next); continue; }
                count=count-n.proxyCount+replacement;
                if (n.left) {
                    active[next.node]=0;
                    for (const auto child:{n.left,n.right}) { active[child]=1; queue.push(candidate(child)); }
                } else active[next.node]=2;
            }
            for (const auto& next:deferred) queue.push(next);
            for (size_t index=root;index<end;++index) if (active[index])
                cloud.navigationCuts[level].push_back({static_cast<uint32_t>(index),active[index]==2});
        }
    }
    } catch (const Canceled&) { return {}; }
    return cloud;
}
Selection selectDetail(const Cloud& cloud,const Matrix& projection,uint32_t width,uint32_t height,float pointSize,uint32_t budget,float spacing) {
    return selectDetail(cloud,cloud.groups,projection,width,height,pointSize,budget,spacing);
}
Selection selectDetail(const Cloud& cloud,std::span<const Group> groups,const Matrix& projection,uint32_t width,uint32_t height,float pointSize,uint32_t budget,float spacing) {
    require(groups.size()==cloud.groups.size(),"Point group count mismatch");
    validateView(projection,width,height,pointSize);
    require(std::isfinite(spacing) && spacing>0,"Invalid point spacing");
    Selection result; if (!budget) return result;
    struct Candidate { float error; uint32_t node; bool operator<(const Candidate& b) const { return error==b.error?node>b.node:error<b.error; } };
    std::priority_queue<Candidate> queue;
    std::vector<uint8_t> active(cloud.nodes.size());
    std::vector<Matrix> matrices; matrices.reserve(groups.size());
    for (const auto& group:groups) matrices.push_back(multiply(group.model,projection));
    const auto candidate=[&](uint32_t index) {
        const auto& n=cloud.nodes[index]; ++result.visited;
        const auto rect=project(n,matrices[n.group],width,height,pointSize);
        return Candidate{rect.visible?(rect.x1-rect.x0)*(rect.y1-rect.y0)/float(n.proxyCount):-1,index};
    };
    for (const auto root:cloud.roots) if (groups[cloud.nodes[root].group].enabled) {
        const auto c=candidate(root); if (c.error<0) continue;
        const auto count=cloud.nodes[root].proxyCount;
        if (result.points+count>budget) {
            const auto remaining=budget-static_cast<uint32_t>(result.points);
            for (uint32_t i=0;i<remaining;++i) {
                const auto sample=static_cast<uint32_t>((uint64_t(i)*2+1)*count/(uint64_t(remaining)*2));
                result.ranges.push_back({cloud.nodes[root].proxyBegin+sample,1,cloud.nodes[root].group,true});
            }
            result.points+=remaining; continue;
        }
        active[root]=1; result.points+=count; queue.push(c);
    }
    while (!queue.empty()) {
        const auto c=queue.top(); queue.pop();
        if (c.error<=spacing*spacing) break;
        const auto& n=cloud.nodes[c.node];
        if (!n.left) {
            if (result.points-n.proxyCount+n.count<=budget) { result.points+=n.count-n.proxyCount; active[c.node]=2; }
            continue;
        }
        const auto a=candidate(n.left),b=candidate(n.right);
        const uint32_t count=(a.error>=0?cloud.nodes[n.left].proxyCount:0)+(b.error>=0?cloud.nodes[n.right].proxyCount:0);
        if (result.points-n.proxyCount+count>budget) continue;
        result.points=result.points-n.proxyCount+count; active[c.node]=0;
        for (const auto child:{a,b}) if (child.error>=0) { active[child.node]=1; queue.push(child); }
    }
    for (size_t i=0;i<active.size();++i) if (active[i]) {
        const auto& n=cloud.nodes[i]; const bool proxy=active[i]==1;
        result.ranges.push_back({proxy?n.proxyBegin:n.begin,proxy?n.proxyCount:n.count,n.group,proxy});
    }
    return result;
}
Selection navigationDetail(const Cloud& cloud,uint32_t budget,uint32_t group) {
    Selection result;
    if (!budget || cloud.navigationCuts.empty()) return result;
    uint64_t sourceCount=0;
    for (const auto root:cloud.roots) {
        const auto& n=cloud.nodes[root];
        if (group==UINT32_MAX || group==n.group) sourceCount+=n.count;
    }
    // Smaller clouds need no reduction when all originals fit the budget.
    if (sourceCount<=budget) {
        for (const auto root:cloud.roots) {
            const auto& n=cloud.nodes[root];
            if (group==UINT32_MAX || group==n.group) result.ranges.push_back({n.begin,n.count,n.group,false});
        }
        result.points=sourceCount; return result;
    }
    const auto groupNodes=[&](const std::vector<NavigationNode>& cut) -> std::span<const NavigationNode> {
        if (group==UINT32_MAX) return cut;
        // Prepared cuts stay ordered by group.
        // Search that span instead of visiting every group for every draw.
        const auto first=std::lower_bound(cut.begin(),cut.end(),group,[&](const auto& item,uint32_t value) { return cloud.nodes[item.node].group<value; });
        const auto last=std::upper_bound(first,cut.end(),group,[&](uint32_t value,const auto& item) { return value<cloud.nodes[item.node].group; });
        return {first,last};
    };
    auto cut=groupNodes(cloud.navigationCuts.front());
    for (const auto& candidate:cloud.navigationCuts) {
        const auto selected=groupNodes(candidate);
        uint64_t count=0; for (const auto& item:selected) count+=item.full?cloud.nodes[item.node].count:cloud.nodes[item.node].proxyCount;
        if (count>budget) break;
        cut=selected;
    }
    for (const auto& item:cut) {
        const auto& n=cloud.nodes[item.node]; const auto count=item.full?n.count:n.proxyCount;
        result.ranges.push_back({item.full?n.begin:n.proxyBegin,count,n.group,!item.full}); result.points+=count;
    }
    return result;
}
Selection queryFootprints(const Cloud& cloud,uint32_t group,const Matrix& matrix,uint32_t width,uint32_t height,
    float pointSize,std::array<float,4> rectangle) {
    validateView(matrix,width,height,pointSize);
    Selection result;
    std::vector<uint32_t> pending;
    for (const auto root:cloud.roots) if (cloud.nodes[root].group==group) pending.push_back(root);
    while (!pending.empty()) {
        const auto index=pending.back(); pending.pop_back(); ++result.visited;
        const auto& n=cloud.nodes[index];
        const auto r=project(n,matrix,width,height,pointSize);
        if (!r.visible || r.x1<rectangle[0] || r.y1<rectangle[1] || r.x0>rectangle[2] || r.y0>rectangle[3]) continue;
        if (n.left) { pending.push_back(n.left); pending.push_back(n.right); }
        else { result.ranges.push_back({n.begin,n.count,n.group,false}); result.points+=n.count; }
    }
    return result;
}
Selection allPoints(const Cloud& cloud) {
    Selection result;
    for (const auto root:cloud.roots) {
        const auto& n=cloud.nodes[root]; if (!cloud.groups[n.group].enabled) continue;
        result.ranges.push_back({n.begin,n.count,n.group,false}); result.points+=n.count;
    }
    return result;
}
Selection nextRefinement(const Selection& full,Refinement& cursor,uint32_t budget) {
    Selection result;
    while (cursor.range<full.ranges.size() && result.points<budget) {
        const auto& range=full.ranges[cursor.range];
        require(cursor.offset<=range.count,"Invalid refinement cursor");
        const auto count=std::min(range.count-cursor.offset,budget-static_cast<uint32_t>(result.points));
        if (count) result.ranges.push_back({range.begin+cursor.offset,count,range.group,range.proxy});
        result.points+=count; cursor.processed+=count; cursor.offset+=count;
        if (cursor.offset==range.count) { ++cursor.range; cursor.offset=0; }
    }
    return result;
}
uint32_t adjustedBudget(uint32_t previous,double gpuMs,double targetMs,uint32_t maximum) {
    require(previous && maximum && std::isfinite(gpuMs) && gpuMs>=0 && std::isfinite(targetMs) && targetMs>0,"Invalid point timing budget");
    const double ratio=std::clamp(targetMs/std::max(gpuMs,.01),.5,1.15);
    return static_cast<uint32_t>(std::clamp(double(previous)*ratio,double(std::min(4096u,maximum)),double(maximum)));
}
Pick pick(const Cloud& cloud,const Matrix& projection,uint32_t width,uint32_t height,float pointSize,float x,float y,float surfaceDepth) {
    validateView(projection,width,height,pointSize);
    require(std::isfinite(x) && std::isfinite(y) && std::isfinite(surfaceDepth) && surfaceDepth>=0 && surfaceDepth<=1,"Invalid point query");
    Pick result; result.depth=surfaceDepth;
    for (const auto root:cloud.roots) {
        const auto& group=cloud.groups[cloud.nodes[root].group]; if (!group.enabled) continue;
        const auto m=multiply(group.model,projection); std::vector<uint32_t> stack{root};
        while (!stack.empty()) {
            const auto index=stack.back(); stack.pop_back(); const auto& n=cloud.nodes[index];
            const auto rect=project(n,m,width,height,pointSize);
            if (!rect.visible || x<rect.x0 || x>rect.x1 || y<rect.y0 || y>rect.y1 || rect.depth+1e-6f<result.depth) continue;
            if (n.left) { stack.push_back(n.left); stack.push_back(n.right); continue; }
            for (uint32_t i=n.begin;i<n.begin+n.count;++i) {
                const auto& p=cloud.points[i]; ++result.tested; const auto c=transform(m,p.position);
                if (!(c[3]>0) || c[2]<0 || c[2]>c[3]) continue;
                const float dx=(c[0]/c[3]*.5f+.5f)*float(width)-x,dy=(.5f-c[1]/c[3]*.5f)*float(height)-y;
                const float z=c[2]/c[3];
                if (dx*dx+dy*dy<=pointSize*pointSize*.25f && (z>result.depth || (z==result.depth && p.id>result.id))) { result.depth=z; result.id=p.id; }
            }
        }
    }
    return result;
}
} // namespace woby::points
