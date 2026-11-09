#include "tetrahedral_quality.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace woby {
namespace {
using Point = Coordinate;
constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double tolerance = 1e-12;
void checkStop(std::stop_token stop) { if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); } }
Point subtract(Point a, const Point& b) { for (size_t k = 0; k < 3; ++k) { a[k] -= b[k]; } return a; }
double dot(const Point& a, const Point& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Point cross(const Point& a, const Point& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
double length(const Point& p) { return std::hypot(p[0],p[1],p[2]); }
double determinant(const Point& a, const Point& b, const Point& c) { return dot(cross(a,b),c); }

// Every elementary operation encloses its exact real result. Compile this
// translation unit with strict floating-point semantics (no contraction).
struct Interval { double low = 0, high = 0; };
Interval exact(double x) { return {x,x}; }
Interval operator+(Interval a, Interval b) { return {std::nextafter(a.low+b.low,-infinity),std::nextafter(a.high+b.high,infinity)}; }
Interval operator-(Interval a, Interval b) { return {std::nextafter(a.low-b.high,-infinity),std::nextafter(a.high-b.low,infinity)}; }
Interval operator*(Interval a, Interval b) {
    const std::array<double,4> v{a.low*b.low,a.low*b.high,a.high*b.low,a.high*b.high};
    return {std::nextafter(*std::min_element(v.begin(),v.end()),-infinity),std::nextafter(*std::max_element(v.begin(),v.end()),infinity)};
}
Interval divide(Interval a, double b) { return {std::nextafter(a.low/b,-infinity),std::nextafter(a.high/b,infinity)}; }
using Vector = std::array<Interval,3>;
Interval determinant(const Vector& a, const Vector& b, const Vector& c) {
    return (a[1]*b[2]-a[2]*b[1])*c[0]+(a[2]*b[0]-a[0]*b[2])*c[1]+(a[0]*b[1]-a[1]*b[0])*c[2];
}
using Powers = std::array<unsigned,4>;
constexpr auto powers = [] {
    std::array<Powers,20> values{}; size_t n = 0;
    for (unsigned a = 0; a <= 3; ++a) { for (unsigned b = 0; b <= 3-a; ++b) {
        for (unsigned c = 0; c <= 3-a-b; ++c) { values[n++] = {a,b,c,3-a-b-c}; }
    }}
    return values;
}();
size_t coefficientIndex(const Powers& p) { return static_cast<size_t>(std::find(powers.begin(),powers.end(),p)-powers.begin()); }
using Polynomial = std::array<Interval,20>;
constexpr std::array<std::array<size_t,2>,6> edges{{{0,1},{1,2},{2,0},{0,3},{1,3},{2,3}}};
Polynomial jacobianPolynomial(const std::array<Vector,10>& p, bool quadratic) {
    if (!quadratic) {
        Vector a{}, b{}, c{};
        for (size_t k = 0; k < 3; ++k) { a[k] = p[1][k]-p[0][k]; b[k] = p[2][k]-p[0][k]; c[k] = p[3][k]-p[0][k]; }
        Polynomial result; result.fill(determinant(a,b,c)); return result;
    }
    constexpr std::array<Point,4> derivatives{{{-1,-1,-1},{1,0,0},{0,1,0},{0,0,1}}};
    std::array<std::array<Vector,3>,4> jacobians{};
    for (size_t v = 0; v < 4; ++v) {
        for (size_t n = 0; n < 10; ++n) {
            for (size_t column = 0; column < 3; ++column) {
                const double derivative = n < 4
                    ? (n == v ? 3.0 : -1.0)*derivatives[n][column]
                    : 4.0*((edges[n-4][0] == v ? derivatives[edges[n-4][1]][column] : 0.0)
                        +(edges[n-4][1] == v ? derivatives[edges[n-4][0]][column] : 0.0));
                if (derivative == 0) { continue; }
                for (size_t axis = 0; axis < 3; ++axis) {
                    jacobians[v][column][axis] = jacobians[v][column][axis]+p[n][axis]*exact(derivative);
                }
            }
        }
    }
    Polynomial coefficients{}; std::array<unsigned,20> counts{};
    for (size_t a = 0; a < 4; ++a) { for (size_t b = 0; b < 4; ++b) { for (size_t c = 0; c < 4; ++c) {
        Powers power{}; ++power[a]; ++power[b]; ++power[c];
        const auto i = coefficientIndex(power);
        coefficients[i] = coefficients[i]+determinant(jacobians[a][0],jacobians[b][1],jacobians[c][2]); ++counts[i];
    }}}
    for (size_t i = 0; i < coefficients.size(); ++i) { coefficients[i] = divide(coefficients[i],counts[i]); }
    return coefficients;
}
Polynomial bisect(const Polynomial& p, size_t replaced, size_t other) {
    constexpr unsigned choose[4][4]{{1,0,0,0},{1,1,0,0},{1,2,1,0},{1,3,3,1}};
    Polynomial result{};
    for (size_t i = 0; i < powers.size(); ++i) {
        const auto count = powers[i][replaced];
        if (!count) { result[i] = p[i]; continue; }
        for (unsigned k = 0; k <= count; ++k) {
            auto index = powers[i]; index[replaced] -= k; index[other] += k;
            result[i] = result[i]+p[coefficientIndex(index)]*exact(static_cast<double>(choose[count][k]));
        }
        result[i] = divide(result[i],static_cast<double>(1u << count));
    }
    return result;
}
struct ValidityBounds { TetrahedralValidity validity; double low, high; };
ValidityBounds inspectPolynomial(const Polynomial& root, std::stop_token stop) {
    struct Region { Polynomial polynomial; std::array<Point,4> vertices; unsigned depth = 0; };
    std::vector<Region> stack{{root,{{{0,0,0},{1,0,0},{0,1,0},{0,0,1}}},0}};
    double low = infinity, high = -infinity;
    bool positive = false, negative = false, unresolved = false;
    size_t visited = 0;
    while (!stack.empty()) {
        checkStop(stop);
        auto r = std::move(stack.back()); stack.pop_back(); ++visited;
        double minimum = infinity, maximum = -infinity;
        for (size_t i = 0; i < powers.size(); ++i) {
            const auto v = r.polynomial[i];
            minimum = std::min(minimum,v.low); maximum = std::max(maximum,v.high);
            if (*std::max_element(powers[i].begin(),powers[i].end()) == 3) {
                positive |= v.low > tolerance; negative |= v.high < -tolerance;
            }
        }
        if (minimum > tolerance || maximum < -tolerance || (minimum >= -tolerance && maximum <= tolerance)
            || r.depth >= 18 || visited >= 4096 || (positive && negative)) {
            low = std::min(low,minimum); high = std::max(high,maximum);
            unresolved |= minimum <= tolerance && maximum >= -tolerance;
            continue;
        }
        size_t a = 0, b = 1; double longest = 0;
        for (const auto& e : edges) {
            const auto delta = subtract(r.vertices[e[0]],r.vertices[e[1]]); const auto distance = dot(delta,delta);
            if (distance > longest) { longest = distance; a = e[0]; b = e[1]; }
        }
        Point midpoint{}; for (size_t k = 0; k < 3; ++k) { midpoint[k] = (r.vertices[a][k]+r.vertices[b][k])*.5; }
        Region left{bisect(r.polynomial,b,a),r.vertices,r.depth+1}, right{bisect(r.polynomial,a,b),r.vertices,r.depth+1};
        left.vertices[b] = right.vertices[a] = midpoint;
        stack.push_back(std::move(right)); stack.push_back(std::move(left));
    }
    const auto validity = positive && negative ? TetrahedralValidity::folded
        : low >= -tolerance && high <= tolerance ? TetrahedralValidity::collapsed
        : unresolved ? TetrahedralValidity::unresolved
        : positive ? TetrahedralValidity::positive : TetrahedralValidity::reversed;
    return {validity,low,high};
}
TetrahedralQualityCell inspectCell(const Mesh& mesh, const TetrahedralCell& cell, std::stop_token stop) {
    TetrahedralQualityCell result; result.values.fill(std::numeric_limits<double>::quiet_NaN()); result.nodeCount = cell.nodeCount;
    if (cell.nodeCount != 4 && cell.nodeCount != 10) { throw std::invalid_argument("Quality requires 4-node or 10-node tetrahedra."); }
    std::array<Point,10> p{}; bool repeated = false;
    double scale = 0;
    for (size_t i = 0; i < cell.nodeCount; ++i) {
        if (cell.nodes[i] >= meshGeometry(mesh).vertices.size()) { throw std::invalid_argument("Tetrahedral node is outside the vertex buffer."); }
        p[i] = meshPosition(mesh,cell.nodes[i]);
        if (!finiteCoordinate(p[i])) { throw std::invalid_argument("Tetrahedral positions must be finite."); }
        for (size_t j = 0; j < i; ++j) { repeated |= cell.nodes[i] == cell.nodes[j]; scale = std::max(scale,length(subtract(p[i],p[j]))); }
    }
    if (scale == 0) {
        for (const size_t i : {0u,1u,2u,5u,6u}) { result.values[i] = 0; }
        result.validity = repeated ? TetrahedralValidity::repeatedNodes : TetrahedralValidity::collapsed; return result;
    }
    if (!std::isfinite(scale)) { return result; }
    std::array<Vector,10> intervalPositions{};
    const auto origin = p[0];
    for (size_t i = 0; i < cell.nodeCount; ++i) { for (size_t k = 0; k < 3; ++k) {
        intervalPositions[i][k] = divide(exact(p[i][k])-exact(origin[k]),scale);
        p[i][k] = (p[i][k]-origin[k])/scale;
    }}
    const auto polynomial = jacobianPolynomial(intervalPositions,cell.nodeCount == 10);
    const auto bounds = inspectPolynomial(polynomial,stop);
    result.validity = repeated ? TetrahedralValidity::repeatedNodes : bounds.validity;
    // Integral of each cubic Bernstein basis function over the reference tet
    // is 1/(6*20), so this is the signed volume of the full quadratic mapping.
    double volume = 0; for (const auto& coefficient : polynomial) { volume += std::midpoint(coefficient.low,coefficient.high)/120.0; }
    result.values[0] = ((volume*scale)*scale)*scale;
    result.values[5] = bounds.low; result.values[6] = bounds.high;
    const bool reversed = bounds.validity == TetrahedralValidity::reversed;
    const double lower = reversed ? -bounds.high : bounds.low, upper = reversed ? -bounds.low : bounds.high;
    if (upper > tolerance) { result.values[7] = std::clamp(lower/upper,-1.0,1.0); }
    // Corner measures must not depend on the displacement of mid-edge nodes.
    double cornerScale = 0;
    for (const auto& edge : edges) { cornerScale = std::max(cornerScale,length(subtract(p[edge[0]],p[edge[1]]))); }
    if (cornerScale > 0) { for (size_t i = 0; i < 4; ++i) { for (auto& x : p[i]) { x /= cornerScale; } } }
    const double det = determinant(subtract(p[1],p[0]),subtract(p[2],p[0]),subtract(p[3],p[0]));
    double sum = 0, product = 0;
    for (const auto& edge : edges) { const auto v = subtract(p[edge[0]],p[edge[1]]); sum += dot(v,v); }
    for (size_t a = 0; a < 4; ++a) {
        double corner = 1; for (size_t b = 0; b < 4; ++b) { if (a != b) { corner *= length(subtract(p[a],p[b])); } }
        product = std::max(product,corner);
    }
    result.values[1] = product > 0 ? std::clamp(std::sqrt(2.0)*det/product,-1.0,1.0) : 0;
    result.values[2] = sum > 0 ? std::clamp(12.0*std::pow(std::abs(det)*.5,2.0/3.0)/sum,0.0,1.0) : 0;
    std::array<Point,4> normals{}; bool validNormals = std::abs(det) > tolerance;
    for (size_t opposite = 0; opposite < 4; ++opposite) {
        std::array<size_t,3> face{}; size_t n = 0;
        for (size_t i = 0; i < 4; ++i) { if (i != opposite) { face[n++] = i; } }
        auto normal = cross(subtract(p[face[1]],p[face[0]]),subtract(p[face[2]],p[face[0]]));
        const auto norm = length(normal); validNormals &= norm > 0;
        if (dot(normal,subtract(p[opposite],p[face[0]])) > 0) { for (auto& x : normal) { x = -x; } }
        if (norm > 0) { for (auto& x : normal) { x /= norm; } } normals[opposite] = normal;
    }
    if (validNormals) {
        double minimum = 180, maximum = 0;
        for (const auto& e : edges) {
            const auto angle = std::acos(std::clamp(-dot(normals[e[0]],normals[e[1]]),-1.0,1.0))*180.0/std::acos(-1.0);
            minimum = std::min(minimum,angle); maximum = std::max(maximum,angle);
        }
        result.values[3] = minimum; result.values[4] = maximum;
    }
    return result;
}
constexpr std::array<const char*,8> keys{"volume","scaled_jacobian","mean_ratio","minimum_dihedral","maximum_dihedral","jacobian_lower","jacobian_upper","jacobian_ratio"};
constexpr std::array<const char*,8> names{"Signed volume","Corner scaled Jacobian","Corner mean ratio","Minimum corner dihedral (deg)","Maximum corner dihedral (deg)","Normalized Jacobian lower bound","Normalized Jacobian upper bound","Jacobian ratio lower bound"};
}

TetrahedralQualitySettings normalizedTetrahedralQualitySettings(TetrahedralQualitySettings s) {
    if (static_cast<size_t>(s.metric) >= tetrahedralQualityMetricCount) { s.metric = TetrahedralQualityMetric::meanRatio; }
    if (!std::isfinite(s.minimum)) { s.minimum = .2f; } if (!std::isfinite(s.maximum)) { s.maximum = 1; }
    if (s.minimumEnabled && s.maximumEnabled) { s.maximum = std::max(s.maximum,s.minimum); }
    return s;
}
const char* tetrahedralQualityMetricKey(TetrahedralQualityMetric m) { return keys.at(static_cast<size_t>(m)); }
const char* tetrahedralQualityMetricName(TetrahedralQualityMetric m) { return names.at(static_cast<size_t>(m)); }
TetrahedralQualityMetric parseTetrahedralQualityMetric(const std::string& key) {
    const auto it = std::find(keys.begin(),keys.end(),key); if (it == keys.end()) { throw std::invalid_argument("Unknown tetrahedral quality metric."); }
    return static_cast<TetrahedralQualityMetric>(it-keys.begin());
}
const char* tetrahedralValidityName(TetrahedralValidity v) {
    constexpr std::array<const char*,6> namesByState{"positive","reversed","folded","collapsed","unresolved","repeated_nodes"};
    return namesByState.at(static_cast<size_t>(v));
}
bool tetrahedralMetricHigherIsBetter(TetrahedralQualityMetric m) {
    return m != TetrahedralQualityMetric::volume && m != TetrahedralQualityMetric::maximumAngle && m != TetrahedralQualityMetric::jacobianUpper;
}
TetrahedralMeshQuality inspectTetrahedralQuality(const Mesh& mesh, std::stop_token stop) {
    TetrahedralMeshQuality result; const auto& geometry = meshGeometry(mesh);
    if (geometry.tetrahedra.size() > UINT32_MAX) { throw std::length_error("Too many tetrahedral cells."); }
    result.cells.reserve(geometry.tetrahedra.size()); uint64_t faces = 0;
    for (size_t i = 0; i < geometry.tetrahedra.size(); ++i) {
        checkStop(stop); const auto& source = geometry.tetrahedra[i]; auto cell = inspectCell(mesh,source,stop);
        cell.sourceCell = source.sourceCell; cell.partId = geometry.nodes.at(source.groupIndex).sourceObjectId;
        cell.firstFace = static_cast<uint32_t>(faces); cell.faceCount = source.nodeCount == 10 ? 16u : 4u; faces += cell.faceCount;
        if (faces > UINT32_MAX/3) { throw std::length_error("Tetrahedral display exceeds supported size."); }
        result.quadraticCells += source.nodeCount == 10; ++result.validityCounts[static_cast<size_t>(cell.validity)]; result.cells.push_back(cell);
        if (cell.validity != TetrahedralValidity::positive && cell.validity != TetrahedralValidity::reversed) { result.validityFindings.push_back(static_cast<uint32_t>(i)); }
    }
    for (size_t metric = 0; metric < tetrahedralQualityMetricCount; ++metric) {
        auto& sorted = result.sorted[metric];
        for (size_t i = 0; i < result.cells.size(); ++i) { checkStop(stop); if (std::isfinite(result.cells[i].values[metric])) { sorted.push_back(static_cast<uint32_t>(i)); } }
        const auto value = [&](size_t i) { return result.cells[sorted[i]].values[metric]; };
        std::sort(sorted.begin(),sorted.end(),[&](uint32_t a,uint32_t b) { checkStop(stop); const auto x = result.cells[a].values[metric], y = result.cells[b].values[metric]; return x != y ? x < y : a < b; });
        if (sorted.empty()) { continue; }
        const auto quantile = [&](double q) { const double at = q*static_cast<double>(sorted.size()-1); const auto a = static_cast<size_t>(at); return std::lerp(value(a),value(std::min(a+1,sorted.size()-1)),at-static_cast<double>(a)); };
        result.statistics[metric] = {sorted.size(),value(0),quantile(.05),quantile(.5),quantile(.95),value(sorted.size()-1)};
        auto& d = result.distributions[metric]; d.minimum = value(0); d.maximum = value(sorted.size()-1);
        if (metric == 1 || metric == 7) { d.minimum = -1; d.maximum = 1; }
        if (metric == 2) { d.minimum = 0; d.maximum = 1; }
        if (d.minimum == d.maximum) { d.maximum = d.minimum+std::max(1.0,std::abs(d.minimum)*.01); }
        d.counts[0] = sorted.size();
        for (const auto index : sorted) { checkStop(stop); const auto position = std::clamp((result.cells[index].values[metric]-d.minimum)/(d.maximum-d.minimum),0.0,1.0); ++d.bins[0][std::min(static_cast<size_t>(position*surfaceQualityBinCount),surfaceQualityBinCount-1)]; }
    }
    return result;
}
TetrahedralQualityDisplay tetrahedralQualityDisplay(const Mesh& mesh, const TetrahedralMeshQuality& quality, std::stop_token stop) {
    TetrahedralQualityDisplay result;
    constexpr std::array<std::array<size_t,6>,4> faces{{{0,1,3,4,8,7},{1,2,3,5,9,8},{2,0,3,6,7,9},{0,2,1,6,5,4}}};
    constexpr std::array<std::array<size_t,3>,4> triangles{{{0,3,5},{3,1,4},{5,4,2},{3,4,5}}};
    const auto& g = meshGeometry(mesh);
    if (g.tetrahedra.size() != quality.cells.size()) { throw std::invalid_argument("Tetrahedral display does not match its cells."); }
    const uint64_t count = quality.cells.empty() ? 0 : uint64_t(quality.cells.back().firstFace)+quality.cells.back().faceCount;
    if (count*sizeof(SurfaceQualityFace) > UINT32_MAX || count*6*sizeof(uint32_t) > UINT32_MAX) { throw std::length_error("Tetrahedral GPU buffer exceeds supported size."); }
    result.indices.reserve(static_cast<size_t>(count)*3); for (auto& bank : result.faces) { bank.reserve(static_cast<size_t>(count)); }
    result.edges.reserve(static_cast<size_t>(count)*6);
    for (size_t i = 0; i < g.tetrahedra.size(); ++i) {
        checkStop(stop); const auto& cell = g.tetrahedra[i]; const auto& q = quality.cells[i];
        std::array<float,8> values{};
        const bool invalid = q.validity == TetrahedralValidity::folded || q.validity == TetrahedralValidity::collapsed || q.validity == TetrahedralValidity::repeatedNodes;
        for (size_t metric = 0; metric < values.size(); ++metric) {
            const auto& d = quality.distributions[metric];
            values[metric] = invalid ? -1.0f : !std::isfinite(q.values[metric]) || q.validity == TetrahedralValidity::unresolved ? -2.0f
                : static_cast<float>(std::clamp((q.values[metric]-d.minimum)/(d.maximum-d.minimum),0.0,1.0));
        }
        for (const auto& face : faces) {
            const auto emit = [&](size_t a,size_t b,size_t c) {
                const std::array<uint32_t,3> ids{cell.nodes[face[a]],cell.nodes[face[b]],cell.nodes[face[c]]};
                result.indices.insert(result.indices.end(),ids.begin(),ids.end());
                for (size_t k = 0; k < 3; ++k) { result.edges.push_back(ids[k]); result.edges.push_back(ids[(k+1)%3]); }
                const auto normal = calculateFaceNormal(g.vertices[ids[0]].position,g.vertices[ids[1]].position,g.vertices[ids[2]].position);
                for (size_t bank = 0; bank < 2; ++bank) { SurfaceQualityFace record; std::copy_n(values.begin()+bank*4,4,record.values.begin()); record.normal = normal; result.faces[bank].push_back(record); }
            };
            if (cell.nodeCount == 4) { emit(0,1,2); } else { for (const auto& t : triangles) { emit(t[0],t[1],t[2]); } }
        }
    }
    return result;
}
std::array<size_t,2> tetrahedralQualityOutsideLimits(const TetrahedralMeshQuality& q, TetrahedralQualitySettings settings) {
    settings = normalizedTetrahedralQualitySettings(settings); const auto metric = static_cast<size_t>(settings.metric); const auto& sorted = q.sorted[metric];
    const auto low = std::lower_bound(sorted.begin(),sorted.end(),settings.minimum,[&](uint32_t i,double v) { return q.cells[i].values[metric] < v; });
    const auto high = std::upper_bound(sorted.begin(),sorted.end(),settings.maximum,[&](double v,uint32_t i) { return v < q.cells[i].values[metric]; });
    return {settings.minimumEnabled ? static_cast<size_t>(low-sorted.begin()) : 0,settings.maximumEnabled ? static_cast<size_t>(sorted.end()-high) : 0};
}
}
