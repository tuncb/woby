#include <bgfx/bgfx_compute.sh>
BUFFER_RO(b_positions, vec4, 0);
BUFFER_RO(b_pointIds, uint, 1);
BUFFER_RO(b_order, uint, 2);
BUFFER_RO(b_clusters, vec4, 3);
BUFFER_RO(b_groups, vec4, 4);
BUFFER_RW(b_winners, uvec4, 5);
uniform mat4 u_hoverView;
uniform mat4 u_hoverProjection;
uniform vec4 u_hoverQuery; // cursor pixels, viewport pixels
uniform vec4 u_hoverDispatch; // cluster count, culling enabled, homogeneous depth, unused
SHARED uvec4 sharedBest[256];
SHARED uint sharedVisible;

vec4 groupTransform(uint base, vec4 p) {
    return b_groups[base]*p.x+b_groups[base+1u]*p.y+b_groups[base+2u]*p.z+b_groups[base+3u]*p.w;
}
bool wins(uvec4 a, uvec4 b) {
    if (a.z==0xffffffffu) { return false; }
    if (b.z==0xffffffffu) { return true; }
    float az=uintBitsToFloat(a.x),bz=uintBitsToFloat(b.x);
    float ad=uintBitsToFloat(a.y),bd=uintBitsToFloat(b.y);
    return az<bz || (az==bz && (ad<bd || (ad==bd && a.z<b.z)));
}
bool outsidePlane(vec4 p, vec3 lo, vec3 hi) {
    vec3 farPoint=vec3(p.x>=0.0 ? hi.x:lo.x,p.y>=0.0 ? hi.y:lo.y,p.z>=0.0 ? hi.z:lo.z);
    float magnitude=abs(p.w)+1.0+dot(abs(p.xyz),max(abs(lo),abs(hi))+vec3_splat(1.0));
    return dot(p,vec4(farPoint,1.0)) < -0.00001*magnitude;
}
bool clusterVisible(uint base, vec3 lo, vec3 hi, float radius) {
    vec4 c0=b_groups[base+4u],c1=b_groups[base+5u],c2=b_groups[base+6u],c3=b_groups[base+7u];
    vec4 x=vec4(c0.x,c1.x,c2.x,c3.x),y=vec4(c0.y,c1.y,c2.y,c3.y),w=vec4(c0.w,c1.w,c2.w,c3.w);
    vec2 low=vec2(2.0*(u_hoverQuery.x-radius)/u_hoverQuery.z-1.0,1.0-2.0*(u_hoverQuery.y+radius)/u_hoverQuery.w);
    vec2 high=vec2(2.0*(u_hoverQuery.x+radius)/u_hoverQuery.z-1.0,1.0-2.0*(u_hoverQuery.y-radius)/u_hoverQuery.w);
    return !outsidePlane(x-low.x*w,lo,hi) && !outsidePlane(high.x*w-x,lo,hi)
        && !outsidePlane(y-low.y*w,lo,hi) && !outsidePlane(high.y*w-y,lo,hi) && !outsidePlane(w,lo,hi);
}
NUM_THREADS(256,1,1)
void main() {
    uint cluster=gl_WorkGroupID.x+gl_WorkGroupID.y*32768u;
    uint lane=gl_LocalInvocationID.x;
    if (cluster>=uint(u_hoverDispatch.x)) { return; }
    uvec4 best=uvec4(0u,0u,0xffffffffu,0xffffffffu);
    if (cluster<uint(u_hoverDispatch.x)) {
        vec4 lo=b_clusters[cluster*3u],hi=b_clusters[cluster*3u+1u];
        uint group= floatBitsToUint(b_clusters[cluster*3u+2u].x),base=group*9u;
        vec4 settings=b_groups[base+8u];
        if (lane==0u) {
            sharedVisible=settings.y>0.0 && (u_hoverDispatch.y<0.5 || clusterVisible(base,lo.xyz,hi.xyz,settings.x)) ? 1u:0u;
            if (sharedVisible==0u) { b_winners[cluster]=best; }
        }
        barrier();
        if (sharedVisible==0u) { return; }
        uint count=floatBitsToUint(hi.w);
        if (lane<count) {
            uint rank=b_order[floatBitsToUint(lo.w)+lane];
            vec3 position=b_positions[b_pointIds[rank]*2u].xyz;
            vec4 world=groupTransform(base,vec4(position,1.0));
            vec4 eye=mul(u_hoverView,world),clip=mul(u_hoverProjection,eye);
            if (clip.w>0.000001) {
                vec3 ndc=clip.xyz/clip.w;
                vec2 screen=vec2(ndc.x*0.5+0.5,0.5-ndc.y*0.5)*u_hoverQuery.zw;
                vec2 delta=screen-u_hoverQuery.xy;
                float distance=dot(delta,delta);
                if (abs(ndc.x)<=1.0 && abs(ndc.y)<=1.0 && ndc.z>=(u_hoverDispatch.z>0.5 ? -1.0:0.0) && ndc.z<=1.0
                    && distance<=settings.x*settings.x) {
                    best=uvec4(floatBitsToUint(ndc.z),floatBitsToUint(distance),rank,group);
                }
            }
        }
    }
    sharedBest[lane]=best;
    barrier();
    for (uint stride=128u;stride>0u;stride>>=1u) {
        if (lane<stride && wins(sharedBest[lane+stride],sharedBest[lane])) { sharedBest[lane]=sharedBest[lane+stride]; }
        barrier();
    }
    if (lane==0u && cluster<uint(u_hoverDispatch.x)) { b_winners[cluster]=sharedBest[0]; }
}
