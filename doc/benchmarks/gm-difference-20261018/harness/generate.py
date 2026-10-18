import hashlib,json,re,sys
from pathlib import Path
root=Path(sys.argv[1]); out=Path(__file__).parent
source=(root/'src/difference_mesh.cpp').read_text()
def once(old,new):
    global source
    assert source.count(old)==1,(old,source.count(old))
    source=source.replace(old,new)
header=r'''
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
namespace gm_profile {
using Clock=std::chrono::steady_clock;
using Json=nlohmann::json;
struct Counters { uint64_t queries=0, samples=0, nodes=0, angles=0, farNodes=0, fallback=0, inside=0; bool sample=false; };
inline Counters counts;
inline Json report() {return {{"queries",counts.queries},{"sampled_queries",counts.samples},{"sampled_nodes",counts.nodes},{"sampled_angles",counts.angles},{"sampled_far",counts.farNodes},{"exact_fallbacks",counts.fallback},{"inside_queries",counts.inside}};}
struct Scope {
    std::string name; Clock::time_point start=Clock::now();
    explicit Scope(std::string s):name(std::move(s)){std::cerr<<Json({{"event","begin"},{"stage",name}}).dump()<<std::endl;}
    ~Scope(){std::cerr<<Json({{"event","end"},{"stage",name},{"seconds",std::chrono::duration<double>(Clock::now()-start).count()},{"counts",report()}}).dump()<<std::endl;}
};
}
'''
for name in ['read','validate','solid','publish','output','approximate']:
    pattern=rf'(?m)^[\w:<>, &*]+\b{name}\([^;]*?\)\s*\n\{{'
    matches=list(re.finditer(pattern,source));assert len(matches)==1,(name,len(matches))
    m=matches[0];source=source[:m.end()]+f'\n    gm_profile::Scope measured("{name}");'+source[m.end():]
once('    std::array<Point, 3> directions;', '    if (gm_profile::counts.sample) ++gm_profile::counts.angles;\n    std::array<Point, 3> directions;')
once('    const auto &node = tree.nodes[index];','    if (gm_profile::counts.sample) ++gm_profile::counts.nodes;\n    const auto &node = tree.nodes[index];')
once('        const auto separation = d - node.radius;','        if (gm_profile::counts.sample) ++gm_profile::counts.farNodes;\n        const auto separation = d - node.radius;')
once('    auto estimate = winding(tree, 0, point, false, stop);','    ++gm_profile::counts.queries;\n    gm_profile::counts.sample = (gm_profile::counts.queries % 1024 == 1);\n    gm_profile::counts.samples += gm_profile::counts.sample;\n    if (gm_profile::counts.queries % 1024 == 0) std::cerr << gm_profile::report().dump() << std::endl;\n    auto estimate = winding(tree, 0, point, false, stop);')
once('    estimate = winding(tree, 0, point, true, stop);','    ++gm_profile::counts.fallback;\n    estimate = winding(tree, 0, point, true, stop);')
once('        (void)buildWindingNode(tree, 0, tree.soup.faces.size(), stop);','        gm_profile::counts = {};\n        gm_profile::Scope reconstructed(std::string("reconstruct_") + name);\n        { gm_profile::Scope building("build_winding_tree");\n        (void)buildWindingNode(tree, 0, tree.soup.faces.size(), stop); }')
once('        auto result = manifold::Manifold::LevelSet([&](manifold::vec3 p) {','        gm_profile::Scope lattice("level_set");\n        auto result = manifold::Manifold::LevelSet([&](manifold::vec3 p) {')
once('            return inside(tree, {p.x, p.y, p.z}, stop) ? 1.0 : -1.0;','            const bool isInside = inside(tree, {p.x, p.y, p.z}, stop);\n            gm_profile::counts.inside += isInside;\n            return isInside ? 1.0 : -1.0;')
once('    for (auto *s : {&a, &b}) {','    std::cerr << gm_profile::Json({{"event","grid"},{"padded_samples",gridPoints},{"size",size},{"cell_size",cellSize}}).dump() << std::endl;\n    for (auto *s : {&a, &b}) {')
once('    const auto mesh = solidResult.GetMeshGL64();','    const auto mesh = solidResult.GetMeshGL64();\n    std::cerr << gm_profile::Json({{"event","manifold_output"},{"status",static_cast<int>(solidResult.Status())},{"vertices",mesh.vertProperties.size()/mesh.numProp},{"triangles",mesh.triVerts.size()/3}}).dump() << std::endl;')
once('    s.points = std::move(points);','    std::cerr << gm_profile::Json({{"event","output_welding"},{"before",s.points.size()},{"after",points.size()}}).dump() << std::endl;\n    s.points = std::move(points);')
generated=out/'difference_mesh_profile.cpp';generated.write_text(header+source)
(out/'manifest.json').write_text(json.dumps({'source':str(root/'src/difference_mesh.cpp'),'source_sha256':hashlib.sha256((root/'src/difference_mesh.cpp').read_bytes()).hexdigest(),'generated_sha256':hashlib.sha256(generated.read_bytes()).hexdigest()},indent=2))
