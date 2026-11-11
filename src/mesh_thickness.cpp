#include "mesh_thickness.h"
#include "mesh_intersections.h"
#include "parallel_work.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <numbers>
#include <stdexcept>

namespace woby {
namespace {
using Point = std::array<double, 3>;
void canceled(std::stop_token stop) { if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); } }
Point sub(Point a, const Point& b) { for (size_t k = 0; k < 3; ++k) { a[k] -= b[k]; } return a; }
Point add(Point a, const Point& b) { for (size_t k = 0; k < 3; ++k) { a[k] += b[k]; } return a; }
Point mul(Point a, double s) { for (auto& v : a) { v *= s; } return a; }
double dot(const Point& a, const Point& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
Point cross(const Point& a, const Point& b) { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
double length(const Point& a) { return std::hypot(a[0], a[1], a[2]); }
Point unit(Point a) { return mul(a, 1/length(a)); }
struct Triangle { std::array<Point, 3> p; Point normal{}, center{}; size_t component = 0; };
struct Node { Point low{}, high{}; size_t begin = 0, count = 0, left = 0, right = 0; };
struct Tree { std::vector<Triangle> triangles; std::vector<size_t> order; std::vector<Node> nodes; std::stop_token stop; };
size_t buildNode(Tree& tree, size_t begin, size_t end)
{
    canceled(tree.stop);
    Node node; node.begin = begin; node.count = end-begin;
    node.low.fill(std::numeric_limits<double>::infinity()); node.high.fill(-std::numeric_limits<double>::infinity());
    for (size_t i = begin; i < end; ++i) {
        if (i%1024 == 0) { canceled(tree.stop); }
        for (const auto& p : tree.triangles[tree.order[i]].p) {
            for (size_t k = 0; k < 3; ++k) { node.low[k] = std::min(node.low[k],p[k]); node.high[k] = std::max(node.high[k],p[k]); }
        }
    }
    const size_t id = tree.nodes.size(); tree.nodes.push_back(node);
    if (end-begin > 8) {
        const auto extent = sub(node.high,node.low);
        const auto axis = static_cast<size_t>(std::max_element(extent.begin(),extent.end())-extent.begin());
        const size_t middle = begin+(end-begin)/2;
        std::nth_element(tree.order.begin()+static_cast<ptrdiff_t>(begin),tree.order.begin()+static_cast<ptrdiff_t>(middle),tree.order.begin()+static_cast<ptrdiff_t>(end),
            [&](size_t a, size_t b) { return tree.triangles[a].center[axis] != tree.triangles[b].center[axis]
                ? tree.triangles[a].center[axis] < tree.triangles[b].center[axis] : a < b; });
        tree.nodes[id].left = buildNode(tree,begin,middle); tree.nodes[id].right = buildNode(tree,middle,end); tree.nodes[id].count = 0;
    }
    return id;
}
bool boxHit(const Node& node, const Point& p, const Point& d, double maximum)
{
    double low = 0, high = maximum;
    for (size_t k = 0; k < 3; ++k) {
        if (d[k] == 0) { if (p[k] < node.low[k] || p[k] > node.high[k]) { return false; } continue; }
        double a = (node.low[k]-p[k])/d[k], b = (node.high[k]-p[k])/d[k];
        if (a > b) { std::swap(a,b); } low = std::max(low,a); high = std::min(high,b);
        if (low > high) { return false; }
    }
    return true;
}
double triangleHit(const Triangle& triangle, const Point& p, const Point& d)
{
    const auto a = sub(triangle.p[1],triangle.p[0]), b = sub(triangle.p[2],triangle.p[0]);
    const auto h = cross(d,b); const double det = dot(a,h);
    if (std::abs(det) <= 32*std::numeric_limits<double>::epsilon()*length(a)*length(b)) { return -1; }
    const auto s = sub(p,triangle.p[0]); const double u = dot(s,h)/det;
    const auto q = cross(s,a); const double v = dot(d,q)/det;
    if (u < -1e-12 || v < -1e-12 || u+v > 1+1e-12) { return -1; }
    const double distance = dot(b,q)/det;
    return distance > 64*std::numeric_limits<double>::epsilon() ? distance : -1;
}
struct Hit { double distance = std::numeric_limits<double>::infinity(); size_t face = noThicknessFace; };
void nearest(const Tree& tree, size_t nodeId, const Point& p, const Point& d, size_t excluded, Hit& hit)
{
    canceled(tree.stop);
    const auto& node = tree.nodes[nodeId]; if (!boxHit(node,p,d,hit.distance)) { return; }
    if (!node.count) { nearest(tree,node.left,p,d,excluded,hit); nearest(tree,node.right,p,d,excluded,hit); return; }
    for (size_t i = node.begin; i < node.begin+node.count; ++i) {
        const auto face = tree.order[i]; if (face == excluded) { continue; }
        const double distance = triangleHit(tree.triangles[face],p,d);
        if (distance > 0 && (distance < hit.distance || (distance == hit.distance && face < hit.face))) { hit = {distance,face}; }
    }
}
void crossings(const Tree& tree, size_t id, const Point& p, const Point& d, size_t component, std::vector<double>& hits)
{
    canceled(tree.stop);
    const auto& node = tree.nodes[id]; if (!boxHit(node,p,d,std::numeric_limits<double>::infinity())) { return; }
    if (!node.count) { crossings(tree,node.left,p,d,component,hits); crossings(tree,node.right,p,d,component,hits); return; }
    for (size_t i = node.begin; i < node.begin+node.count; ++i) {
        const auto& triangle = tree.triangles[tree.order[i]]; if (triangle.component == component) { continue; }
        const auto t = triangleHit(triangle,p,d); if (t > 0) { hits.push_back(t); }
    }
}
bool nested(const Tree& tree, size_t component, const Point& point)
{
    // Count distinct crossings of other shells. The source intersection check
    // excludes touching shells. Two fixed directions must agree on nesting.
    int parity = -1;
    for (const Point direction : {Point{.819,.371,.438},Point{-.217,.903,.371}}) {
        std::vector<double> hits; crossings(tree,0,point,unit(direction),component,hits);
        std::sort(hits.begin(),hits.end()); size_t count = 0; double previous = -1;
        for (const auto t : hits) { if (previous < 0 || std::abs(t-previous) > 1e-12*std::max(1.0,t)) { ++count; previous = t; } }
        const int current = static_cast<int>(count%2);
        if (parity >= 0 && parity != current) { throw std::runtime_error("Shell nesting is ambiguous."); } parity = current;
    }
    return parity == 1;
}
double percentile(std::vector<std::pair<double,double>> values, double fraction, std::stop_token stop)
{
    if (values.empty()) { return 0; }
    std::sort(values.begin(),values.end(),[&](const auto& a,const auto& b) { canceled(stop); return a.first < b.first; });
    double total = 0; for (const auto& value : values) { total += value.second; }
    double sum = 0; for (const auto& value : values) { sum += value.second; if (sum >= fraction*total) { return value.first; } }
    return values.back().first;
}
void measureSource(ThicknessSource& output, const SourceTopology& source, std::stop_token stop)
{
    if (!source.available || source.geometry->faces.empty()) { output.unavailableReason = "No retained surface triangles."; return; }
    if (source.excludedCollapsedFaces) { output.unavailableReason = "Collapsed triangles prevent a solid boundary check."; return; }
    for (const auto& edge : source.edges) {
        canceled(stop);
        if (edge.incidentFaces.size() != 2 || edge.windingConflict || edge.orientationContradiction) {
            output.unavailableReason = "Surface must be closed, manifold and consistently oriented."; return;
        }
    }
    AnalysisGeometry geometry; geometry.sources.push_back(source.geometry); geometry.availableSources = 1;
    const auto intersections = inspectIntersections(geometry,{false,false},stop,{1,0});
    if (!intersections.findings.empty() || intersections.truncated) {
        output.unavailableReason = intersections.findings.empty() ? "Self-intersection check reached its work limit." : "Surface has self-intersections or touching shells."; return;
    }
    Point low = source.geometry->vertices[source.geometry->faces.front().vertices[0]].position, high = low;
    for (const auto& face : source.geometry->faces) {
        canceled(stop);
        for (const auto v : face.vertices) {
            const auto& p = source.geometry->vertices[v].position;
            for (size_t k = 0; k < 3; ++k) { low[k] = std::min(low[k],p[k]); high[k] = std::max(high[k],p[k]); }
        }
    }
    const auto extent = sub(high,low); const double scale = *std::max_element(extent.begin(),extent.end());
    if (!(scale > 0) || !std::isfinite(scale*scale)) { output.unavailableReason = "Surface scale is outside the supported numeric range."; return; }
    Tree tree; tree.stop = stop; tree.triangles.resize(source.faces.size()); tree.order.resize(source.faces.size());
    std::iota(tree.order.begin(),tree.order.end(),size_t{0});
    std::vector<double> volumes(source.components.size(),0);
    for (size_t f = 0; f < tree.triangles.size(); ++f) {
        canceled(stop); auto& triangle = tree.triangles[f]; triangle.component = source.faces[f].component;
        for (size_t k = 0; k < 3; ++k) { triangle.p[k] = mul(sub(source.geometry->vertices[source.geometry->faces[f].vertices[k]].position,low),1/scale); }
        const auto normal = cross(sub(triangle.p[1],triangle.p[0]),sub(triangle.p[2],triangle.p[0]));
        if (!(length(normal) > 0)) { output.unavailableReason = "Surface contains a numerically collapsed triangle."; return; }
        triangle.normal = unit(normal); triangle.center = mul(add(add(triangle.p[0],triangle.p[1]),triangle.p[2]),1.0/3);
        // A component-local origin avoids cancellation for small disjoint solids.
        const auto anchorId = source.geometry->faces[source.components[triangle.component].front()].vertices[0];
        const auto anchor = mul(sub(source.geometry->vertices[anchorId].position,low),1/scale);
        volumes[triangle.component] += dot(sub(triangle.p[0],anchor),cross(sub(triangle.p[1],anchor),sub(triangle.p[2],anchor)));
    }
    (void)buildNode(tree,0,tree.triangles.size());
    std::vector<double> orientation(volumes.size());
    for (size_t c = 0; c < volumes.size(); ++c) {
        if (volumes[c] == 0 || !std::isfinite(volumes[c])) { output.unavailableReason = "Shell volume is unresolved."; return; }
        bool inside;
        try { inside = nested(tree,c,tree.triangles[source.components[c].front()].center); }
        catch (const std::runtime_error& error) { canceled(stop); output.unavailableReason = error.what(); return; }
        orientation[c] = (volumes[c] > 0 ? 1 : -1)*(inside ? -1 : 1);
    }
    for (auto& triangle : tree.triangles) { triangle.normal = mul(triangle.normal,orientation[triangle.component]); }
    output.faces.resize(tree.triangles.size());
    constexpr std::array<std::array<double,3>,4> samples{{{1.0/3,1.0/3,1.0/3},{.6,.2,.2},{.2,.6,.2},{.2,.2,.6}}};
    parallelAnalysisBatches(tree.triangles.size(),128,stop,[&](size_t begin,size_t end) {
        for (size_t f = begin; f < end; ++f) {
            canceled(stop); auto& result = output.faces[f]; const auto& triangle = tree.triangles[f];
            result.area = .5*length(cross(sub(triangle.p[1],triangle.p[0]),sub(triangle.p[2],triangle.p[0])))*scale*scale;
            const auto inward = mul(triangle.normal,-1);
            const auto tangent = unit(cross(inward,std::abs(inward[0]) < .8 ? Point{1,0,0} : Point{0,1,0}));
            const auto bitangent = cross(inward,tangent);
            std::vector<double> distances; size_t supportedRays = 0;
            for (const auto& weights : samples) {
                Point p{}; for (size_t k = 0; k < 3; ++k) { p = add(p,mul(triangle.p[k],weights[k])); }
                std::array<Hit,7> hits{}; std::array<Point,7> directions{};
                for (size_t ray = 0; ray < hits.size(); ++ray) {
                    const double angle = static_cast<double>(ray)*std::numbers::pi/3;
                    directions[ray] = ray == 0 ? inward : unit(add(inward,mul(add(mul(tangent,std::cos(angle)),mul(bitangent,std::sin(angle))),.4663076581549986)));
                    nearest(tree,0,p,directions[ray],f,hits[ray]);
                    if (hits[ray].face != noThicknessFace) {
                        const auto& target = tree.triangles[hits[ray].face];
                        if (dot(target.normal,triangle.normal) > -.5 || dot(target.normal,directions[ray]) <= .1) { hits[ray] = {}; }
                    }
                }
                const auto best = std::min_element(hits.begin(),hits.end(),[](const auto& a,const auto& b) { return a.distance < b.distance; });
                if (best->face == noThicknessFace) { continue; }
                const size_t support = static_cast<size_t>(std::count_if(hits.begin(),hits.end(),[&](const auto& hit) { return hit.face != noThicknessFace && hit.distance <= best->distance*1.5; }));
                if (support < 3) { continue; }
                supportedRays += support; const double distance = best->distance*scale; distances.push_back(distance);
                if (!std::isfinite(result.minimum) || distance < result.minimum) {
                    result.minimum = distance; result.opposite = best->face;
                    result.from = add(low,mul(p,scale));
                    result.to = add(result.from,mul(directions[static_cast<size_t>(best-hits.begin())],distance));
                }
            }
            if (distances.size() >= 2) {
                std::sort(distances.begin(),distances.end()); result.typical = distances[distances.size()/2];
                result.confidence = static_cast<double>(supportedRays)/28;
            } else { result.minimum = std::numeric_limits<double>::quiet_NaN(); result.opposite = noThicknessFace; }
            for (size_t k = 0; k < 3; ++k) {
                const auto& uses = source.edges[source.faces[f].edges[k]].incidentFaces;
                result.neighbors[k] = uses[0].face == f ? uses[1].face : uses[0].face;
            }
        }
    });
    std::vector<std::pair<double,double>> values;
    for (const auto& face : output.faces) { if (std::isfinite(face.typical)) { values.emplace_back(face.typical,face.area); } }
    output.reference = percentile(std::move(values),.95,stop);
}
}

ThicknessSettings normalizedThicknessSettings(ThicknessSettings s)
{
    s.maximumThickness = std::isfinite(s.maximumThickness) ? std::clamp(s.maximumThickness,0.0f,1e15f) : 1;
    s.maximumRelativeThickness = std::isfinite(s.maximumRelativeThickness) ? std::clamp(s.maximumRelativeThickness,0.0f,1.0f) : .2f;
    s.minimumRegionArea = std::isfinite(s.minimumRegionArea) ? std::clamp(s.minimumRegionArea,0.0f,1e30f) : 0;
    s.elementsThroughThickness = std::clamp(s.elementsThroughThickness,1,100);
    return s;
}
bool sameThicknessFilters(ThicknessSettings a, ThicknessSettings b)
{
    a = normalizedThicknessSettings(a); b = normalizedThicknessSettings(b);
    a.autoUpdate = b.autoUpdate; a.show = b.show; a.showMap = b.showMap; return a == b;
}
std::shared_ptr<const ThicknessMeasurements> measureMeshThickness(const std::vector<DuplicateSource>& sources,std::stop_token stop)
{
    canceled(stop); auto result = std::make_shared<ThicknessMeasurements>();
    auto topology = buildTopologyIncidence(buildAnalysisGeometry(sources,TopologyMode::exactPosition,stop),stop);
    inspectTopologyFindings(topology,topologyVertexLinks,stop);
    std::vector<bool> invalidVertices(topology.sources.size());
    for (const auto& finding : topology.nonManifoldVertices) { invalidVertices[finding.source] = true; }
    for (size_t i = 0; i < topology.sources.size(); ++i) {
        const auto& source = topology.sources[i];
        canceled(stop); ThicknessSource output; output.geometry = source.geometry;
        if (invalidVertices[i]) { output.unavailableReason = "Surface has non-manifold vertices."; }
        else { measureSource(output,source,stop); }
        if (output.unavailableReason.empty()) { ++result->availableSources; } else { ++result->unavailableSources; }
        size_t measured = 0; for (const auto& face : output.faces) { measured += std::isfinite(face.typical); }
        result->measuredFaces += measured; result->unknownFaces += source.geometry->faces.size()-measured+source.excludedCollapsedFaces;
        result->sources.push_back(std::move(output));
    }
    if (sources.empty()) { result->unavailableSources = 1; }
    canceled(stop); return result;
}
MeshThickness filterThinRegions(std::shared_ptr<const ThicknessMeasurements> measurements,ThicknessSettings settings,std::stop_token stop)
{
    canceled(stop); MeshThickness result; result.settings = normalizedThicknessSettings(settings); result.measurements = std::move(measurements);
    if (!result.measurements) { return result; } const auto& s = result.settings;
    for (size_t sourceId = 0; sourceId < result.measurements->sources.size(); ++sourceId) {
        const auto& source = result.measurements->sources[sourceId]; if (!source.unavailableReason.empty()) { continue; }
        std::vector<bool> selected(source.faces.size()); std::vector<size_t> parent(source.faces.size()); std::iota(parent.begin(),parent.end(),size_t{0});
        for (size_t f = 0; f < source.faces.size(); ++f) {
            canceled(stop); const auto& face = source.faces[f];
            selected[f] = std::isfinite(face.typical) && ((s.absoluteEnabled && face.typical <= s.maximumThickness)
                || (s.relativeEnabled && source.reference > 0 && face.typical/source.reference <= s.maximumRelativeThickness));
        }
        const auto root = [&](size_t f) { while (parent[f] != f) { parent[f] = parent[parent[f]]; f = parent[f]; } return f; };
        for (size_t f = 0; f < source.faces.size(); ++f) {
            canceled(stop); if (!selected[f]) { continue; }
            const auto join = [&](size_t other) { if (other < selected.size() && selected[other]) {
                const auto a = root(f), b = root(other); parent[std::max(a,b)] = std::min(a,b);
            } };
            for (const auto neighbor : source.faces[f].neighbors) { join(neighbor); } join(source.faces[f].opposite);
        }
        std::vector<size_t> regions(source.faces.size(),noThicknessFace); std::vector<ThinRegion> found;
        for (size_t f = 0; f < source.faces.size(); ++f) {
            canceled(stop); if (!selected[f]) { continue; } const size_t r = root(f);
            if (regions[r] == noThicknessFace) {
                regions[r] = found.size(); ThinRegion region; region.source = sourceId; region.minimum = std::numeric_limits<double>::infinity();
                region.minimumBound.fill(std::numeric_limits<double>::infinity()); region.maximumBound.fill(-std::numeric_limits<double>::infinity()); found.push_back(std::move(region));
            }
            auto& region = found[regions[r]]; const auto& face = source.faces[f]; region.faces.push_back(f);
            region.area += face.area; region.confidence += face.confidence*face.area;
            if (face.minimum < region.minimum) { region.minimum = face.minimum; region.from = face.from; region.to = face.to; }
            for (const auto v : source.geometry->faces[f].vertices) {
                const auto& p = source.geometry->vertices[v].position;
                for (size_t k = 0; k < 3; ++k) { region.minimumBound[k] = std::min(region.minimumBound[k],p[k]); region.maximumBound[k] = std::max(region.maximumBound[k],p[k]); }
            }
        }
        for (auto& region : found) {
            canceled(stop); if (region.area < s.minimumRegionArea || !(region.area > 0)) { continue; }
            std::vector<std::pair<double,double>> values; for (const auto f : region.faces) { values.emplace_back(source.faces[f].typical,source.faces[f].area); }
            region.typical = percentile(std::move(values),.5,stop); region.relativeThickness = region.typical/source.reference;
            region.confidence /= region.area; region.suggestedSize = region.minimum/static_cast<double>(s.elementsThroughThickness);
            result.regions.push_back(std::move(region));
        }
    }
    canceled(stop); return result;
}
const char* thicknessStatus(const MeshThickness& result)
{
    if (!result.measurements) { return "not_checked"; }
    const auto& m = *result.measurements;
    if (!m.measuredFaces) { return "unavailable"; }
    return m.unavailableSources || m.unknownFaces ? "partial" : "complete";
}
} // namespace woby
