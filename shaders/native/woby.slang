#include <NoGraphicsAPI/shader.slang>
#include "root.h"
GPU_ROOT(WobyRoot, root);

float4 clipPosition(float4 value) {
#ifdef WOBY_VULKAN
    value.y = -value.y;
#endif
    return value;
}
struct VertexOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    // Match the original renderer: shade circular coverage once per pixel.
    // MSAA still handles geometry/depth coverage and individual picking samples.
    float2 uv : TEXCOORD0;
    nointerpolation uint id : TEXCOORD1;
    float4 color : COLOR0;
};
float3 positionAt(uint index) {
    uint p = index * root.stride;
    return float3(root.vertices[p],root.vertices[p+1],root.vertices[p+2]);
}
VertexOutput vs_mesh(uint index : SV_VertexID) {
    VertexOutput v = {};
    uint p = index * root.stride;
    v.position = clipPosition(mul(root.modelViewProj,float4(positionAt(index),1)));
    v.normal = mul(root.model,float4(root.vertices[p+3],root.vertices[p+4],root.vertices[p+5],0)).xyz;
    v.uv = float2(root.vertices[p+6],root.vertices[p+7]);
    return v;
}
VertexOutput vs_color(uint index : SV_VertexID) {
    VertexOutput v = {};
    v.position = clipPosition(mul(root.modelViewProj,float4(positionAt(index),1)));
    return v;
}
VertexOutput vs_annotation(uint index : SV_VertexID) {
    VertexOutput v = {};
    v.position = clipPosition(float4(positionAt(index),1));
    return v;
}
VertexOutput vs_comparison(uint index : SV_VertexID) {
    VertexOutput v = vs_mesh(index);
    uint p = index * root.stride;
    v.normal = float3(root.vertices[p+3],root.vertices[p+4],root.vertices[p+5]);
    v.uv = float2(root.vertices[p+6],root.vertices[p+7]);
    if (root.comparison.z > 1.5 && root.comparison.z < 2.5) {
        // Four per-face metrics occupy the UV slots of the first two corners.
        // Every corner fetches the same selected value; buffer size and vertex
        // layout stay identical to an ordinary expanded mesh.
        uint metric = uint(root.comparison.x);
        uint corner = (index/3)*3 + metric/2;
        v.uv = float2(root.vertices[corner*root.stride+6+metric%2],0);
    }
    return v;
}
VertexOutput pointVertex(uint vertex, uint instance) {
    VertexOutput v = {};
    uint base = uint(root.pointParams[1].x) + uint(root.pointParams[1].y)*65536u;
    uint index = root.pointIds[base+instance];
    float2 corner = float2(1.0-float((vertex>>1)&1),float(vertex&1))-0.5;
    float4 center = mul(root.modelViewProj,float4(positionAt(index),1));
    center.xy += corner * root.pointParams[0].x * float2(2.0/root.pointParams[0].y,2.0/root.pointParams[0].z) * center.w;
    v.position = clipPosition(center);
    v.uv = corner;
    uint first = uint(root.markerBase.x)|(uint(root.markerBase.y)<<16);
    v.id = first == 0 ? 0 : first+instance;
    return v;
}
VertexOutput vs_point_sprite(uint vertex : SV_VertexID,uint instance : SV_InstanceID) { return pointVertex(vertex,instance); }
VertexOutput vs_line_sprite(uint vertex : SV_VertexID,uint instance : SV_InstanceID) {
    VertexOutput v = {};
    v.position = float4(2,2,2,1); // Rejected/zero-length segments emit no fragments.
    uint base = uint(root.pointParams[1].x) + uint(root.pointParams[1].y)*65536u + instance*2;
    float4 a = mul(root.modelViewProj,float4(positionAt(root.pointIds[base]),1));
    float4 b = mul(root.modelViewProj,float4(positionAt(root.pointIds[base+1]),1));
    // Clip in homogeneous space before dividing by w. Native depth is [0,1],
    // including reversed-Z; both near and far planes are covered.
    float4 planes[6] = {float4(1,0,0,1),float4(-1,0,0,1),float4(0,1,0,1),
        float4(0,-1,0,1),float4(0,0,1,0),float4(0,0,-1,1)};
    for (uint i=0;i<6;++i) {
        float da=dot(a,planes[i]), db=dot(b,planes[i]);
        if (da<0 && db<0) return v;
        if ((da<0)!=(db<0)) {
            float4 cut=lerp(a,b,da/(da-db));
            if (da<0) a=cut; else b=cut;
        }
    }
    if (a.w<=0 || b.w<=0) return v;
    float2 viewport=root.pointParams[0].yz;
    float2 delta=(b.xy/b.w-a.xy/a.w)*viewport;
    float pixelLength=length(delta);
    if (pixelLength<.000001) return v;
    float2 normal=float2(-delta.y,delta.x)/pixelLength;
    float side=(vertex&1)!=0 ? 1.0 : -1.0;
    float4 endpoint=(vertex&2)!=0 ? b : a;
    endpoint.xy+=normal*side*root.pointParams[0].x/viewport*endpoint.w;
    v.position=clipPosition(endpoint);
    return v;
}
VertexOutput vs_marker_point(uint vertex : SV_VertexID,uint instance : SV_InstanceID) { return pointVertex(vertex,instance); }
float3 uvGridColor(float3 color, float2 uv, float2 density) {
        float2 cell = uv * density;
        float2 footprint = max(fwidth(cell),float2(.00001));
        float2 distance = abs(frac(cell+.5)-.5);
        float2 lines = 1-smoothstep(footprint*.6,footprint*1.4,distance);
        // Fade subpixel cells instead of letting distant/dense grids shimmer.
        lines *= 1-smoothstep(float2(.25),float2(.75),footprint);
        color = lerp(color,float3(.12,.78,.92),lines.x);
        color = lerp(color,float3(1,.55,.16),lines.y);
    return color;
}
float3 uvGradient(float value, float2 range) {
    float t = saturate((value-range.x)/(range.y-range.x));
    return lerp(float3(.12,.25,.85),float3(1,.78,.12),t);
}
float4 fs_mesh(VertexOutput v) : SV_Target0 {
    float diffuse = max(dot(normalize(v.normal),normalize(float3(.35,.6,.7))),0);
    float3 color = root.color.rgb*(.32+diffuse*.68);
    if (root.uvGrid.z > 3.5) { color = uvGradient(root.uvGrid.z > 4.5 ? v.uv.y : v.uv.x, root.uvGrid.xy); }
    else if (root.uvGrid.z > .5) { color = uvGridColor(color, v.uv, root.uvGrid.xy); }
    return float4(color,root.color.a);
}
float4 fs_color(VertexOutput v) : SV_Target0 { return root.color; }
uint triangleIndexOffset() {
    return uint(root.triangleEdges.z) | (uint(root.triangleEdges.w) << 16);
}
struct EdgeVertexOutput {
    VertexOutput mesh;
    noperspective float3 bary : TEXCOORD2;
};
EdgeVertexOutput vs_mesh_edges(uint corner : SV_VertexID) {
    EdgeVertexOutput v;
    v.mesh = vs_mesh(root.pointIds[triangleIndexOffset()+corner]);
    uint k = corner % 3;
    v.bary = float3(k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0);
    return v;
}
// X-ray and transparent overlays keep hardware-line coverage without a second
// CPU/GPU edge-index buffer. Each triangle emits (0,1), (1,2), (2,0).
VertexOutput vs_triangle_lines(uint vertex : SV_VertexID) {
    uint corner = ((vertex % 6 + 1) / 2) % 3;
    return vs_color(root.pointIds[triangleIndexOffset()+(vertex/6)*3+corner]);
}
float4 triangleEdgeColor(VertexOutput v, float3 bary) {
    float3 dx = ddx(bary), dy = ddy(bary);
    float3 distances = bary / max(sqrt(dx*dx+dy*dy),float3(1e-8));
    float distance = min(distances.x,min(distances.y,distances.z));
    float coverage = 1-smoothstep(max(0.0,root.triangleEdges.x-.5),root.triangleEdges.x+.5,distance);
    float3 edge = saturate(root.color.rgb*1.25);
    if (root.triangleEdges.y < .5) {
        if (coverage <= 0) discard;
        return float4(edge,root.color.a*coverage);
    }
    return float4(lerp(fs_mesh(v).rgb,edge,coverage),root.color.a);
}
float4 fs_mesh_edges(VertexOutput v, noperspective float3 bary : SV_Barycentrics) : SV_Target0 {
    return triangleEdgeColor(v,bary);
}
float4 fs_mesh_edges_pulled(EdgeVertexOutput v) : SV_Target0 { return triangleEdgeColor(v.mesh,v.bary); }
float4 fs_point_sprite(VertexOutput v) : SV_Target0 {
    if (length(v.uv) > .5 || root.color.a <= .000001) discard;
    return root.color;
}
float4 fs_comparison(VertexOutput v) : SV_Target0 {
    float3 color = root.color.rgb;
    if (root.comparison.z > 5.5) {
        float amount = saturate(v.uv.x);
        float3 middle = root.comparison.w > .5 ? float3(.92,.92,.92) : float3(1,.82,.3);
        color = amount <= .5 ? lerp(float3(.18,.48,.85),middle,amount*2)
            : lerp(middle,float3(.94,.22,.055),(amount-.5)*2);
        if (v.uv.y > .5) color = float3(.1,1,.8);
        if (v.uv.x < 0) color = float3(1,0,1);
        if (v.uv.x < -1.5) color = float3(.56,.61,.67);
        return float4(color,1);
    }
    if (root.comparison.z > 3.5) { return float4(uvGradient(root.comparison.z > 4.5 ? v.uv.y : v.uv.x,root.comparison.xy),1); }
    if (root.comparison.z > 2.5) {
        float light = .6+.4*abs(dot(normalize(v.normal+1e-8),normalize(float3(.35,.6,.7))));
        return float4(uvGridColor(color*light, v.uv, root.comparison.xy),1);
    }
    if (root.comparison.z > 1.5) {
        float amount = saturate(v.uv.x);
        if (root.comparison.w > .5) amount = 1-amount;
        color = amount <= .5 ? lerp(float3(.18,.48,.85),float3(1,.82,.3),amount*2)
            : lerp(float3(1,.82,.3),float3(.94,.22,.055),(amount-.5)*2);
        if (v.uv.x < 0) color = float3(1,0,1);
        if (v.uv.x < -1.5) color = float3(.56,.61,.67);
    } else if (root.comparison.z > .5) {
        color = float3(.56,.61,.67);
        if (v.uv.x > root.comparison.x) {
            float amount = root.comparison.y > root.comparison.x ? saturate((v.uv.x-root.comparison.x)/(root.comparison.y-root.comparison.x)) : 1;
            color = lerp(float3(1,.77,.31),float3(.94,.22,.055),amount);
        }
    }
    float light = .6+.4*abs(dot(normalize(v.normal+1e-8),normalize(float3(.35,.6,.7))));
    return float4(color*light,1);
}
struct MarkerOutput { float4 color : SV_Target0; float4 id : SV_Target1; };
MarkerOutput fs_marker_mesh_edges(VertexOutput v, noperspective float3 bary : SV_Barycentrics) {
    return MarkerOutput(triangleEdgeColor(v,bary),float4(0));
}
MarkerOutput fs_marker_mesh_edges_pulled(EdgeVertexOutput v) {
    return MarkerOutput(triangleEdgeColor(v.mesh,v.bary),float4(0));
}
MarkerOutput fs_marker_point(VertexOutput v) {
    MarkerOutput o;
    o.color = fs_point_sprite(v);
    o.id = float4(v.id&255,(v.id>>8)&255,(v.id>>16)&255,v.id>>24)/255.0;
    return o;
}
MarkerOutput fs_marker_mesh(VertexOutput v) {
    if (root.color.a <= .000001) discard;
    return MarkerOutput(fs_mesh(v),float4(0));
}
MarkerOutput fs_marker_line(VertexOutput v) {
    if (root.color.a <= .000001) discard;
    return MarkerOutput(root.color,float4(0));
}
MarkerOutput fs_marker_comparison(VertexOutput v) { return MarkerOutput(fs_comparison(v),float4(0)); }

VertexOutput screenVertex(uint index,bool highlight) {
    VertexOutput v = {};
    float2 uv = float2((index>>1)&1,index&1);
    if (highlight) uv = (root.markerQuery.xy+(uv*2-1)*root.markerOptions.z)/root.markerQuery.zw;
    v.position = clipPosition(float4(uv.x*2-1,1-uv.y*2,0,1));
    v.uv = uv;
    return v;
}
VertexOutput vs_marker_screen(uint index : SV_VertexID) { return screenVertex(index,false); }
VertexOutput vs_marker_highlight(uint index : SV_VertexID) { return screenVertex(index,true); }
float4 fs_marker_composite(VertexOutput v) : SV_Target0 {
    return gpu_texture<Texture2D<float4>>(root.texture0).Sample(gpu_sampler(root.sampler),v.uv);
}
float4 loadMarkerSample(Texture2DMS<float4> texture,int2 pixel,uint sample) {
    __target_switch {
    case metal:
        // Slang 2026.18.3 emits signed coordinates for Texture2DMS.Load,
        // but Metal requires uint2. Callers check pixel bounds before loading.
        __intrinsic_asm "$0.read(uint2($1), uint($2))";
    default:
        return texture.Load(pixel,sample);
    }
}
uint readId(int2 pixel,uint sample,bool msaa) {
    float4 value = msaa ? loadMarkerSample(gpu_texture<Texture2DMS<float4>>(root.texture0),pixel,sample)
        : gpu_texture<Texture2D<float4>>(root.texture0).Load(int3(pixel,0));
    uint4 bytes = uint4(round(value*255));
    return bytes.x | (bytes.y<<8) | (bytes.z<<16) | (bytes.w<<24);
}
void lookup(bool msaa) {
    uint best = 0; float bestDistance = 1e30;
    for (int i=0;i<49;++i) {
        int2 pixel = int2(floor(root.markerQuery.xy))+int2(i%7-3,i/7-3);
        if (any(pixel<0) || any(pixel>=int2(root.markerQuery.zw))) continue;
        float2 delta = float2(pixel)+.5-root.markerQuery.xy;
        float distance = dot(delta,delta);
        if (distance>9) continue;
        for (uint s=0;s<uint(root.markerOptions.x);++s) {
            uint id = readId(pixel,s,msaa);
            if (id != 0 && (best == 0 || distance<bestDistance || (distance==bestDistance && id<best))) { best=id; bestDistance=distance; }
        }
    }
    gpu_texture<RWTexture2D<float4>>(root.image1)[uint2(0,0)] = float4(best&65535,best>>16,0,0);
}
[numthreads(1,1,1)] void cs_marker_lookup_single() { lookup(false); }
[numthreads(1,1,1)] void cs_marker_lookup_msaa() { lookup(true); }
float4 highlight(VertexOutput v,bool msaa) {
    float4 winner = gpu_texture<Texture2D<float4>>(root.texture1).Load(int3(0,0,0));
    uint id = uint(winner.x)|(uint(winner.y)<<16);
    if (id == 0) discard;
    int2 pixel = int2(v.uv*root.markerQuery.zw);
    if (any(pixel<0) || any(pixel>=int2(root.markerQuery.zw))) discard;
    float coverage = 0;
    for (uint s=0;s<uint(root.markerOptions.x);++s) if (readId(pixel,s,msaa)==id) coverage++;
    if (coverage == 0) discard;
    return float4(1,.68,.05,coverage/root.markerOptions.x);
}
float4 fs_marker_highlight_single(VertexOutput v) : SV_Target0 { return highlight(v,false); }
float4 fs_marker_highlight_msaa(VertexOutput v) : SV_Target0 { return highlight(v,true); }
VertexOutput vs_imgui(uint index : SV_VertexID) {
    VertexOutput v = {};
    uint p = index*5;
    v.position = clipPosition(mul(root.modelViewProj,float4(root.vertices[p],root.vertices[p+1],0,1)));
    v.uv = float2(root.vertices[p+2],root.vertices[p+3]);
    uint color = asuint(root.vertices[p+4]);
    v.color = float4(color&255,(color>>8)&255,(color>>16)&255,color>>24)/255.0;
    return v;
}
float4 fs_imgui(VertexOutput v) : SV_Target0 {
    return v.color*gpu_texture<Texture2D<float4>>(root.texture0).Sample(gpu_sampler(root.sampler),v.uv);
}

// CPU chooses a stable parameter grid and supplies separable basis tables.
// Compute evaluates the rational control net and generates render topology.
uint ffHeader(uint slot) { return asuint(root.freeform[slot]); }
float3 ffVector(uint offset) { return float3(root.freeform[offset],root.freeform[offset+1],root.freeform[offset+2]); }
[numthreads(64,1,1)] void cs_freeform(uint3 thread : SV_DispatchThreadID) {
    uint nu=ffHeader(4), nv=ffHeader(5), id=thread.x;
    bool trimmed=ffHeader(15)!=0;
    if (id>=(trimmed ? ffHeader(14) : nu*nv)) return;
    uint x=trimmed ? asuint(root.freeform[ffHeader(13)+id*2]) : id%nu;
    uint y=trimmed ? asuint(root.freeform[ffHeader(13)+id*2+1]) : id/nu;
    uint bu=ffHeader(9)+x*19, bv=ffHeader(10)+y*19;
    uint firstU=asuint(root.freeform[bu]), firstV=asuint(root.freeform[bv]);
    uint controls=ffHeader(8), countU=ffHeader(2), pu=ffHeader(0), pv=ffHeader(1);
    float3 reference=ffVector(controls+(firstV*countU+firstU)*9);
    float3 position=0, du=0, dv=0, authoredNormal=0;
    float2 uv=0;
    float denominator=0, wu=0, wv=0, scale=0;
    for (uint j=0;j<=pv;++j) for (uint i=0;i<=pu;++i)
        scale=max(scale,root.freeform[controls+((firstV+j)*countU+firstU+i)*9+3]);
    for (uint j=0;j<=pv;++j) for (uint i=0;i<=pu;++i) {
        uint c=controls+((firstV+j)*countU+firstU+i)*9;
        float w=root.freeform[c+3]/scale;
        float b=root.freeform[bu+1+i]*root.freeform[bv+1+j]*w;
        float a=root.freeform[bu+10+i]*root.freeform[bv+1+j]*w;
        float d=root.freeform[bu+1+i]*root.freeform[bv+10+j]*w;
        float3 delta=ffVector(c)-reference;
        position+=b*delta; du+=a*delta; dv+=d*delta;
        denominator+=b; wu+=a; wv+=d;
        uv+=b*float2(root.freeform[c+4],root.freeform[c+5]);
        authoredNormal+=b*ffVector(c+6);
    }
    position/=denominator;
    du=(du-position*wu)/denominator; dv=(dv-position*wv)/denominator;
    position+=reference; uv/=denominator;
    float su=max(max(abs(du.x),abs(du.y)),abs(du.z));
    float sv=max(max(abs(dv.x),abs(dv.y)),abs(dv.z));
    float3 n=ffHeader(12)!=0 ? authoredNormal : cross(du/max(su,1e-30),dv/max(sv,1e-30));
    uint output=(ffHeader(6)+id)*8;
    // Preserve the CPU interior-limit normal at singular boundaries.
    if (dot(n,n)>1e-20 && all(isfinite(n))) {
        n=normalize(n);
        root.vertices[output+3]=n.x; root.vertices[output+4]=n.y; root.vertices[output+5]=n.z;
    }
    root.vertices[output]=position.x; root.vertices[output+1]=position.y; root.vertices[output+2]=position.z;
    if (ffHeader(11)!=0) { root.vertices[output+6]=uv.x; root.vertices[output+7]=1-uv.y; }
    // Trim connectivity is supplied by CPU constrained triangulation. Never
    // overwrite it with rectangular cells, which would fill the holes again.
    if (trimmed) return;
    uint a=ffHeader(6)+id;
    if (ffHeader(3)!=0) {
        if (x+1<nu && y+1<nv) {
            uint o=ffHeader(7)+(y*(nu-1)+x)*6;
            root.pointIds[o]=a; root.pointIds[o+1]=a+1; root.pointIds[o+2]=a+nu;
            root.pointIds[o+3]=a+1; root.pointIds[o+4]=a+nu+1; root.pointIds[o+5]=a+nu;
        }
    } else if (x+1<nu) {
        uint o=ffHeader(7)+x*2;
        root.pointIds[o]=a; root.pointIds[o+1]=a+1;
    }
}
