#pragma once
#include "freeform.h"
#include <cmath>

inline woby::FreeformTrimLoop trimPolygon(const std::vector<std::array<double,2>>& points)
{
    auto curve=std::make_shared<woby::FreeformPatch>();
    curve->degreeU=1; curve->countU=static_cast<uint32_t>(points.size()+1);
    for (const auto& p:points) { curve->controls.push_back({p[0],p[1],0,1}); }
    curve->controls.push_back(curve->controls.front());
    curve->domainU={0,double(points.size())}; curve->knotsU={0};
    for (size_t i=0;i<=points.size();++i) { curve->knotsU.push_back(double(i)); }
    curve->knotsU.push_back(double(points.size()));
    return {{{curve,curve->domainU}},12};
}

inline woby::FreeformPatch trimTestPatch(bool circular=false)
{
    woby::FreeformPatch p;
    p.name="trimmed"; p.surface=true; p.degreeU=1; p.degreeV=1; p.countU=2; p.countV=2;
    p.controls={{{-1,-1,.4,1}},{{1,-1,.4,1}},{{-1,1,.4,1}},{{1,1,.4,1}}};
    p.knotsU={0,0,1,1}; p.knotsV=p.knotsU; p.domainU={0,1}; p.domainV={0,1};
    p.texcoords={{{0,0}},{{1,0}},{{0,1}},{{1,1}}};
    auto trim=std::make_shared<woby::FreeformTrimming>(); trim->sourceFile="test.obj";
    trim->regions.emplace_back(); trim->regions[0].outer.sourceLine=12;
    auto hole=trimPolygon({{.25,.25},{.75,.25},{.75,.75},{.25,.75}});
    if (circular) {
        auto curve=std::make_shared<woby::FreeformPatch>();
        curve->degreeU=2; curve->countU=9; curve->domainU={0,4};
        curve->knotsU={0,0,0,1,1,2,2,3,3,4,4,4};
        const double w=std::sqrt(.5);
        curve->controls={{{.75,.5,0,1}},{{.75,.75,0,w}},{{.5,.75,0,1}},{{.25,.75,0,w}},{{.25,.5,0,1}},
            {{.25,.25,0,w}},{{.5,.25,0,1}},{{.75,.25,0,w}},{{.75,.5,0,1}}};
        hole={{{curve,{0,4}}},12};
    }
    trim->regions[0].holes.push_back(std::move(hole)); p.trimming=std::move(trim);
    return p;
}
