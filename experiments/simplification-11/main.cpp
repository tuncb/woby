#include "../simplification-16/allocation_probe.h"
#include "scene_history.h"
#include "ui_operations.h"
#include "obj_mesh.h"
#include "model_load.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <set>
#include <random>

namespace {
using namespace woby;
using Json=nlohmann::json;
void require(bool value,const char* message){if(!value){throw std::runtime_error(message);}}
struct FixtureRoot {
    std::filesystem::path path=std::filesystem::absolute(std::filesystem::temp_directory_path())
        /("woby-simplification11-"+std::to_string(std::random_device{}())+"-"+std::to_string(std::random_device{}()));
    FixtureRoot(){require(std::filesystem::create_directory(path),"Cannot create unique fixture root");}
    ~FixtureRoot(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};
// A migration probe, not a production UiState replacement. The metadata owner
// excludes live assets and session values. File-part descriptors validate reloads.
struct PartDescriptor {
    std::string name;
    bool line=false,point=false;
    friend bool operator==(const PartDescriptor&,const PartDescriptor&)=default;
};
struct Content {
    UiState metadata;
    std::vector<std::vector<PartDescriptor>> fileParts;
    std::vector<std::shared_ptr<const AnnotationGeometry>> annotationGeometry;
};
std::vector<std::shared_ptr<const AnnotationGeometry>> publishGeometry(const UiState& state) {
    std::vector<std::shared_ptr<const AnnotationGeometry>> result;
    result.reserve(state.annotations.size());
    for(const auto& item:state.annotations){result.push_back(std::make_shared<const AnnotationGeometry>(*item.geometry));}
    return result;
}
Content capture(const UiState& state,const std::vector<std::shared_ptr<const AnnotationGeometry>>& published,bool share) {
    Content result;auto& m=result.metadata;
    m.sceneGeneration=state.sceneGeneration;m.coordinateOrigin=state.coordinateOrigin;m.showOrigin=state.showOrigin;m.showGrid=state.showGrid;
    m.triangleEdgeXray=state.triangleEdgeXray;m.adaptivePoints=state.adaptivePoints;m.showDimensions=state.showDimensions;
    m.upAxis=state.upAxis;m.masterVertexPointSize=state.masterVertexPointSize;m.views=state.views;m.sceneNodes=state.sceneNodes;
    m.comparisons.reserve(state.comparisons.size());
    for(const auto& item:state.comparisons) {
        UiComparison copy;copy.objectId=item.objectId;copy.name=item.name;copy.settings=item.settings;copy.translation=item.translation;
        copy.a=item.a;copy.b=item.b;m.comparisons.push_back(std::move(copy));
    }
    m.annotations.reserve(state.annotations.size());result.annotationGeometry.reserve(state.annotations.size());
    for(size_t i=0;i<state.annotations.size();++i) {
        const auto& item=state.annotations[i];UiAnnotation copy;
        copy.objectId=item.objectId;copy.targetId=item.targetId;copy.targetIds=item.targetIds;copy.targetName=item.targetName;copy.settings=item.settings;
        m.annotations.push_back(std::move(copy));result.annotationGeometry.push_back(share?published.at(i):std::make_shared<const AnnotationGeometry>(*item.geometry));
    }
    m.files.reserve(state.files.size());result.fileParts.reserve(state.files.size());
    for(const auto& file:state.files) {
        UiFileState copy;copy.path=file.path;copy.importerId=file.importerId;copy.groupSettings=file.groupSettings;copy.fileSettings=file.fileSettings;
        copy.vertexSizeScale=file.vertexSizeScale;copy.objectId=file.objectId;copy.mesh.origin=file.mesh.origin;
        // The production SceneDocument supplies these descriptors. A single
        // content owner must keep them once for reload validation and persistence.
        std::vector<PartDescriptor> descriptors;descriptors.reserve(meshGeometry(file.mesh).nodes.size());
        for(const auto& node:meshGeometry(file.mesh).nodes){descriptors.push_back({node.name,node.lineIndexCount!=0,node.pointIndexCount!=0});}
        result.fileParts.push_back(std::move(descriptors));m.files.push_back(std::move(copy));
    }
    return result;
}
bool equalNodes(const std::vector<UiSceneNode>& a,const std::vector<UiSceneNode>& b) {
    if(a.size()!=b.size()){return false;}
    for(size_t i=0;i<a.size();++i) {
        const auto& x=a[i];const auto& y=b[i];
        if(x.kind!=y.kind||x.name!=y.name||x.objectId!=y.objectId||x.fileIndex!=y.fileIndex||x.groupIndex!=y.groupIndex
            ||sceneNodeSettings(x.settings)!=sceneNodeSettings(y.settings)||!equalNodes(x.children,y.children)){return false;}
    }
    return true;
}
bool equalContent(const Content& first,const Content& second) {
    const auto& a=first.metadata;const auto& b=second.metadata;
    if(a.sceneGeneration!=b.sceneGeneration||a.coordinateOrigin!=b.coordinateOrigin||a.showOrigin!=b.showOrigin||a.showGrid!=b.showGrid
        ||a.showDimensions!=b.showDimensions||a.triangleEdgeXray!=b.triangleEdgeXray||a.adaptivePoints!=b.adaptivePoints
        ||a.upAxis!=b.upAxis||a.masterVertexPointSize!=b.masterVertexPointSize||a.files.size()!=b.files.size()
        ||a.annotations.size()!=b.annotations.size()||a.comparisons.size()!=b.comparisons.size()||a.views.size()!=b.views.size()
        ||first.fileParts!=second.fileParts||!equalNodes(a.sceneNodes,b.sceneNodes)){return false;}
    for(size_t i=0;i<a.files.size();++i) {
        const auto& x=a.files[i];const auto& y=b.files[i];
        if(x.path!=y.path||x.importerId!=y.importerId||x.objectId!=y.objectId||x.mesh.origin!=y.mesh.origin
            ||x.vertexSizeScale!=y.vertexSizeScale||sceneFileSettings(x.fileSettings)!=sceneFileSettings(y.fileSettings)
            ||x.groupSettings.size()!=y.groupSettings.size()){return false;}
        for(size_t g=0;g<x.groupSettings.size();++g) {
            if(x.groupSettings[g].objectId!=y.groupSettings[g].objectId||sceneGroupSettings(x.groupSettings[g])!=sceneGroupSettings(y.groupSettings[g])){return false;}
        }
    }
    for(size_t i=0;i<a.comparisons.size();++i) {
        const auto& x=a.comparisons[i];const auto& y=b.comparisons[i];
        if(x.objectId!=y.objectId||x.name!=y.name||x.settings!=y.settings||x.translation!=y.translation||x.a!=y.a||x.b!=y.b){return false;}
    }
    for(size_t i=0;i<a.views.size();++i) {
        const auto& x=a.views[i];const auto& y=b.views[i];
        if(x.id!=y.id||x.name!=y.name||x.scene!=y.scene||x.objects!=y.objects||x.parts!=y.parts){return false;}
    }
    for(size_t i=0;i<a.annotations.size();++i) {
        const auto& x=a.annotations[i];const auto& y=b.annotations[i];
        if(x.objectId!=y.objectId||x.targetId!=y.targetId||x.targetIds!=y.targetIds||x.targetName!=y.targetName||x.settings!=y.settings){return false;}
        const auto& p=first.annotationGeometry[i];const auto& q=second.annotationGeometry[i];
        if(p!=q&&*p!=*q){return false;} // Equal values with different owners remain no-ops.
    }
    return true;
}
UiState materialize(const Content& content) {
    UiState result=content.metadata;
    for(size_t f=0;f<result.files.size();++f){auto& nodes=editMeshGeometry(result.files[f].mesh).nodes;
        for(const auto& descriptor:content.fileParts[f]){MeshNode node;node.name=descriptor.name;node.lineIndexCount=descriptor.line?1u:0u;node.pointIndexCount=descriptor.point?1u:0u;nodes.push_back(std::move(node));}
        publishMeshGeometry(result.files[f].mesh);}
    for(size_t i=0;i<result.annotations.size();++i){result.annotations[i].geometry={content.annotationGeometry[i]};}
    return result;
}
struct History {
    std::deque<Content> snapshots;
    size_t cursor=0;
    uint64_t recordedRevision=0,interaction=0;
    bool coalescing=false;
};
void reset(History& h,const UiState& state,const std::vector<std::shared_ptr<const AnnotationGeometry>>& published,bool share) {
    History fresh;fresh.snapshots.push_back(capture(state,published,share));fresh.recordedRevision=state.sceneEditRevision;h=std::move(fresh);
}
bool record(History& h,const UiState& state,const std::vector<std::shared_ptr<const AnnotationGeometry>>& published,bool share,uint64_t interaction=0) {
    if(h.snapshots.empty()||h.snapshots[h.cursor].metadata.sceneGeneration!=state.sceneGeneration){reset(h,state,published,share);return true;}
    // Apply View navigation is an explicit extra transaction in production;
    // this probe measures content edits only and refuses an unmodeled action.
    require(!state.viewApplication,"Apply View is outside this probe");
    const bool merge=h.coalescing&&h.interaction!=0&&(interaction==h.interaction||interaction==0);
    const bool notified=h.recordedRevision!=state.sceneEditRevision;bool changed=false;
    if(notified) {
        auto next=capture(state,published,share);changed=!equalContent(next,h.snapshots[h.cursor]);
        if(changed) {
            h.snapshots.erase(h.snapshots.begin()+static_cast<ptrdiff_t>(h.cursor+1),h.snapshots.end());
            if(merge){h.snapshots[h.cursor]=std::move(next);}else{h.snapshots.push_back(std::move(next));++h.cursor;}
            h.coalescing=interaction!=0;
        }
        h.recordedRevision=state.sceneEditRevision;
    }
    if(!changed&&!merge){h.coalescing=false;}h.interaction=interaction;
    if(interaction==0) {
        h.interaction=0;h.coalescing=false;
        if(merge&&h.cursor>0&&equalContent(h.snapshots[h.cursor],h.snapshots[h.cursor-1])){h.snapshots.pop_back();--h.cursor;}
    }
    return notified;
}
UiState fixture(size_t segments,size_t groups,const std::filesystem::path& fixtureRoot) {
    UiState state;Mesh mesh;
    auto& g=editMeshGeometry(mesh);g.vertices={{{0,0,0}},{{1,0,0}},{{0,1,0}}};g.indices={0,1,2};
    for(size_t i=0;i<groups;++i){MeshNode n;n.name="part "+std::to_string(i);n.indexCount=3;g.nodes.push_back(std::move(n));}
    mesh.bounds={{0,0,0},{1,1,0},{.5f,.5f,0},1};publishMeshGeometry(mesh);
    state.files.push_back(createUiFileState(fixtureRoot/"history-source.obj",std::move(mesh),0));
    appendDefaultSceneNodesForFiles(state,0);assignSceneObjectIds(state);
    if(segments) {
        UiAnnotation annotation;annotation.objectId=state.nextObjectId++;annotation.targetId=state.files[0].groupSettings[0].objectId;
        annotation.targetName="history-source.obj / part 0";AnnotationGeometry geometry;geometry.fingerprint="synthetic-history-fixture";annotation.geometryRevision=1;
        geometry.segments.resize(segments);
        for(size_t i=0;i<segments;++i){auto& segment=geometry.segments[i];segment.a={1,0,0};segment.b={0,1,0};if(i%17==0){segment.endTriangle=0;}}
        annotation.geometry=shareAnnotationGeometry(std::move(geometry));
        state.annotations.push_back(std::move(annotation));
    }
    return state;
}
void selfTest() {
    const FixtureRoot root;auto state=fixture(53,3,root.path);auto published=publishGeometry(state);SceneHistory baseline;History shared;
    resetSceneHistory(baseline,state);reset(shared,state,published,true);
    const auto compare=[&]{require(shared.cursor==baseline.cursor&&shared.snapshots.size()==baseline.snapshots.size(),"History transaction changed");
        for(size_t i=0;i<shared.snapshots.size();++i){require(sceneContentEqual(createSceneDocument(materialize(shared.snapshots[i])),baseline.snapshots[i].document),"Persistent snapshot changed");}};
    const auto edit=[&](float x,uint64_t token){setMasterVertexPointSize(state,x);require(recordSceneHistory(baseline,state,token)==record(shared,state,published,true,token),"Notification changed");compare();};
    edit(8,11);markSceneDirty(state);recordSceneHistory(baseline,state,11);record(shared,state,published,true,11);compare();
    edit(12,11);edit(4,0);require(shared.cursor==0,"Drag returning to original made an action");
    edit(9,0);edit(11,0);baseline.cursor=1;shared.cursor=1;setMasterVertexPointSize(state,15);recordSceneHistory(baseline,state);record(shared,state,published,true);compare();
    const auto first=capture(state,published,true);auto equivalent=publishGeometry(state);const auto separate=capture(state,equivalent,true);
    require(equalContent(first,separate),"Equal geometry with another owner changed content");
    auto changedGeometry=*state.annotations[0].geometry;changedGeometry.segments.back().b={0,0,1};state.annotations[0].geometry=shareAnnotationGeometry(std::move(changedGeometry));++state.annotations[0].geometryRevision;published=publishGeometry(state);
    require(!equalContent(first,capture(state,published,true)),"Geometry replacement was lost");
    markSceneDirty(state);recordSceneHistory(baseline,state);record(shared,state,published,true);compare();
    const auto persistent=capture(state,published,true);state.camera.yawRadians+=1;state.uiScale=1.5f;state.selectedSceneObjects={state.files[0].objectId};
    state.annotations[0].targetValid=!state.annotations[0].targetValid;state.comparisons.push_back({});state.comparisons.clear();
    require(equalContent(persistent,capture(state,published,true)),"Session values dirtied content");
    const auto originalOwner=published[0];auto deleted=state;deleted.annotations.clear();auto deletedPublished=publishGeometry(deleted);const auto removed=capture(deleted,deletedPublished,true);
    require(!equalContent(persistent,removed)&&originalOwner->segments.size()==53,"Removal damaged a historical owner");
}
Json stats(allocation_probe::Start before,double ms) {
    const auto live=allocation_probe::live.load()-before.liveBytes,peak=allocation_probe::peak.load()-before.liveBytes;
    const auto total=allocation_probe::total.load()-before.totalBytes,calls=allocation_probe::calls.load()-before.callCount;
    return Json{{"ms",ms},{"live_requested_bytes",live},{"peak_requested_bytes",peak},{"total_requested_bytes",total},{"allocation_calls",calls}};
}
template<class Function> auto measured(Json& record,const char* name,Function work) {
    const auto before=allocation_probe::start();const auto start=std::chrono::steady_clock::now();auto value=work();
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    auto result=stats(before,ms);record[name]=std::move(result);return value;
}
Json sample(UiState& state,int actions,int run) {
    Json result;auto published=publishGeometry(state);
    std::array<double,12> productionFrames{},singleFrames{},sharedFrames{};
    const auto production=[&]{SceneHistory h;resetSceneHistory(h,state);for(int i=0;i<actions;++i){setMasterVertexPointSize(state,5+float(i%20));const auto start=std::chrono::steady_clock::now();recordSceneHistory(h,state);
        productionFrames[static_cast<size_t>(i)]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}return h;};
    const auto candidate=[&](bool share){History h;reset(h,state,published,share);for(int i=0;i<actions;++i){setMasterVertexPointSize(state,5+float(i%20));const auto start=std::chrono::steady_clock::now();record(h,state,published,share);
        (share?sharedFrames:singleFrames)[static_cast<size_t>(i)]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}return h;};
    state.masterVertexPointSize=4;SceneHistory wide;History one,shared;
    if(run%2==0){wide=measured(result,"production_history",production);state.masterVertexPointSize=4;one=measured(result,"single_content_history",[&]{return candidate(false);});state.masterVertexPointSize=4;shared=measured(result,"shared_content_history",[&]{return candidate(true);});}
    else{shared=measured(result,"shared_content_history",[&]{return candidate(true);});state.masterVertexPointSize=4;one=measured(result,"single_content_history",[&]{return candidate(false);});state.masterVertexPointSize=4;wide=measured(result,"production_history",production);}
    require(wide.snapshots.size()==one.snapshots.size()&&one.snapshots.size()==shared.snapshots.size(),"Benchmark snapshot counts differ");
    result["production_history"]["worst_record_frame_ms"]=*std::max_element(productionFrames.begin(),productionFrames.begin()+actions);
    result["single_content_history"]["worst_record_frame_ms"]=*std::max_element(singleFrames.begin(),singleFrames.begin()+actions);
    result["shared_content_history"]["worst_record_frame_ms"]=*std::max_element(sharedFrames.begin(),sharedFrames.begin()+actions);
    for(size_t i=0;i<shared.snapshots.size();++i){require(sceneContentEqual(createSceneDocument(materialize(shared.snapshots[i])),wide.snapshots[i].document),"Benchmark content changed");}
    result["snapshots"]=wide.snapshots.size();
    size_t productionSegmentBytes=0,singleSegmentBytes=0,sharedUniqueSegmentBytes=0;
    std::set<const AnnotationGeometry*> productionOwners;
    for(const auto& snapshot:wide.snapshots){for(const auto& item:snapshot.content.annotations){if(productionOwners.insert(item.geometry.storage.get()).second){productionSegmentBytes+=item.geometry->segments.capacity()*sizeof(AnnotationSegment);}}
        for(const auto& item:snapshot.document.annotations){if(productionOwners.insert(item.geometry.storage.get()).second){productionSegmentBytes+=item.geometry->segments.capacity()*sizeof(AnnotationSegment);}}}
    for(const auto& snapshot:one.snapshots){for(const auto& geometry:snapshot.annotationGeometry){singleSegmentBytes+=geometry->segments.capacity()*sizeof(AnnotationSegment);}}
    std::set<const AnnotationGeometry*> seenGeometry;
    for(const auto& snapshot:shared.snapshots){for(const auto& geometry:snapshot.annotationGeometry){if(seenGeometry.insert(geometry.get()).second){sharedUniqueSegmentBytes+=geometry->segments.capacity()*sizeof(AnnotationSegment);}}}
    result["annotation_segment_array_capacity_bytes"]={{"production_history",productionSegmentBytes},{"single_content_history",singleSegmentBytes},
        {"shared_unique_geometry",sharedUniqueSegmentBytes},{"shared_extra_snapshot_geometry",0}};
    const auto noOp=[&]{for(int i=0;i<10;++i){markSceneDirty(state);recordSceneHistory(wide,state);}return wide.snapshots.size();};
    (void)measured(result,"production_notified_noop_10",noOp);
    (void)measured(result,"single_content_notified_noop_10",[&]{for(int i=0;i<10;++i){markSceneDirty(state);record(one,state,published,false);}return one.snapshots.size();});
    (void)measured(result,"shared_content_notified_noop_10",[&]{for(int i=0;i<10;++i){markSceneDirty(state);record(shared,state,published,true);}return shared.snapshots.size();});
    const auto clean=createSceneDocument(state);const auto cleanContent=capture(state,published,true);
    (void)measured(result,"production_clean_dirty_query_10",[&]{for(int i=0;i<10;++i){updateSceneDirty(state,clean);require(!state.isDirty,"Clean scene became dirty");}return true;});
    (void)measured(result,"shared_clean_dirty_query_10",[&]{for(int i=0;i<10;++i){require(equalContent(capture(state,published,true),cleanContent),"Clean content changed");}return true;});
    recordSceneHistory(wide,state);record(shared,state,published,true);
    (void)measured(result,"production_idle_history_10000",[&]{size_t notifications=0;for(int i=0;i<10000;++i){notifications+=recordSceneHistory(wide,state);}return notifications;});
    (void)measured(result,"shared_idle_history_10000",[&]{size_t notifications=0;for(int i=0;i<10000;++i){notifications+=record(shared,state,published,true);}return notifications;});
    state.masterVertexPointSize=4;
    const auto dragWide=measured(result,"production_drag",[&]{SceneHistory h;resetSceneHistory(h,state);for(int i=0;i<actions;++i){setMasterVertexPointSize(state,5+float(i%20));recordSceneHistory(h,state,i==actions-1?0u:91u);}return h;});
    state.masterVertexPointSize=4;
    const auto dragSingle=measured(result,"single_content_drag",[&]{History h;reset(h,state,published,false);for(int i=0;i<actions;++i){setMasterVertexPointSize(state,5+float(i%20));record(h,state,published,false,i==actions-1?0u:91u);}return h;});
    state.masterVertexPointSize=4;
    const auto dragShared=measured(result,"shared_content_drag",[&]{History h;reset(h,state,published,true);for(int i=0;i<actions;++i){setMasterVertexPointSize(state,5+float(i%20));record(h,state,published,true,i==actions-1?0u:91u);}return h;});
    require(dragWide.snapshots.size()==2&&dragSingle.snapshots.size()==2&&dragShared.snapshots.size()==2,"Drag stopped coalescing");
    require(sceneContentEqual(createSceneDocument(materialize(dragShared.snapshots.back())),dragWide.snapshots.back().document),"Drag final content changed");
    result["verified"]=true;return result;
}
}
int main(int argc,char** argv) {
    try {
        selfTest();if(argc==2&&std::string_view(argv[1])=="--self-test"){return 0;}
        require(argc==5,"Usage: woby_simplification_11_probe segments groups actions samples");
        const int actions=std::stoi(argv[3]),repeats=std::stoi(argv[4]);
        require(actions>0&&actions<=12&&repeats>0&&repeats<=9,"Invalid dimensions");
        const FixtureRoot root;UiState state;std::string workload="synthetic";
        if(std::string_view(argv[1])=="--model") {
            workload=argv[2];state.files.push_back(createUiFileState(std::filesystem::absolute(argv[2]),loadObjMesh(argv[2]),0));
            appendDefaultSceneNodesForFiles(state,0);assignSceneObjectIds(state);
        } else if(std::string_view(argv[1])=="--scene") {
            workload=argv[2];const auto scenePath=std::filesystem::absolute(argv[2]);const auto document=readSceneDocument(scenePath);
            for(const auto& file:document.files) {const auto modelPath=sceneAbsolutePath(scenePath,file.path);auto live=createUiFileState(modelPath,loadModelMesh(modelPath),0,file.importerId);applySceneFileRecord(live,file);state.files.push_back(std::move(live));}
            appendDefaultSceneNodesForFiles(state,0);applySceneNodeRecords(state,document.nodes);assignSceneObjectIds(state);loadSceneAnnotations(state,document);
        } else {
            const size_t segments=std::stoull(argv[1]),groups=std::stoull(argv[2]);
            require(segments<=1000000&&groups>0&&groups<=100000,"Invalid dimensions");state=fixture(segments,groups,root.path);
        }
        size_t segments=0,groups=0;for(const auto& a:state.annotations){segments+=a.geometry->segments.size();}for(const auto& file:state.files){groups+=file.groupSettings.size();}
        Json output{{"workload",workload},{"segments",segments},{"groups",groups},{"actions",actions},{"sizeof_segment",sizeof(AnnotationSegment)},
            {"notes","Release requested C++ allocation sizes, global new header and atomic counters. Synthetic valid barycentric segments; annotation attachment is not exercised. Live source and published geometry owners are outside measured snapshot storage. Single-content candidate uses one geometry value per snapshot. Shared candidate uses one immutable published owner; migration must also replace the live value. Oracle persistence checks run outside timing. Apply View/reload/stale worker tests remain production implementation gates."}};
        Json runs=Json::array();for(int i=0;i<repeats;++i){std::cerr<<"sample "<<i<<'\n';runs.push_back(sample(state,actions,i));}output["samples"]=std::move(runs);std::cout<<output.dump(2)<<'\n';
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
