import json, math
from fractions import Fraction as F
from pathlib import Path

root = Path(__file__).resolve().parents[1]
events = [json.loads(line) for line in (root/'measurements/rowan-roundoff-witness/stderr.jsonl').read_text().splitlines()]
witness = next(event for event in events if event.get('event') == 'intersection')
output = next(event for event in events if event.get('event') == 'manifold_output')
def sub(a, b): return [x-y for x,y in zip(a,b)]
def dot(a, b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
a, b = [[list(map(F,point)) for point in face['points']] for face in witness['faces']]
normal = cross(sub(a[1],a[0]),sub(a[2],a[0]))
signed = [dot(normal,sub(point,a[0])) for point in b]
normal_length = math.sqrt(float(dot(normal,normal)))
record = {'manifold_output':output,'witness':witness,
          'exact_plane_side_signs':[0 if x==0 else (1 if x>0 else -1) for x in signed],
          'signed_plane_distances':[float(x)/normal_length for x in signed],
          'small_triangle_edge_lengths':[math.dist(b[i],b[(i+1)%3]) for i in range(3)],
          'small_triangle_area':math.sqrt(float(dot(cross(sub(b[1],b[0]),sub(b[2],b[0])),cross(sub(b[1],b[0]),sub(b[2],b[0])))))/2}
if signed[0]*signed[1] < 0:
    t = signed[0]/(signed[0]-signed[1])
    point = [x+t*(y-x) for x,y in zip(b[0],b[1])]
    v0,v1,v2 = sub(a[1],a[0]),sub(a[2],a[0]),sub(point,a[0])
    d00,d01,d11,d20,d21 = dot(v0,v0),dot(v0,v1),dot(v1,v1),dot(v2,v0),dot(v2,v1)
    den = d00*d11-d01*d01
    v,w = (d11*d20-d01*d21)/den,(d00*d21-d01*d20)/den
    weights = [1-v-w,v,w]
    record['edge_plane_intersection_barycentric'] = [float(x) for x in weights]
    record['edge_plane_intersection_strictly_inside_large_triangle'] = all(x>0 for x in weights)
    record['intersection_distance_from_shared_vertex'] = math.dist(point,b[2])
(root/'witness-analysis.json').write_text(json.dumps(record,indent=2))
print(json.dumps(record,indent=2))
