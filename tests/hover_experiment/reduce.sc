#include <bgfx/bgfx_compute.sh>
BUFFER_RO(b_input, uvec4, 0);
BUFFER_RW(b_output, uvec4, 1);
UIMAGE2D_WO(t_result, rgba32ui, 2);
uniform vec4 u_hoverReduce; // count, final pass
SHARED uvec4 sharedBest[256];
bool wins(uvec4 a,uvec4 b) {
    if (a.z==0xffffffffu) { return false; }
    if (b.z==0xffffffffu) { return true; }
    float az=uintBitsToFloat(a.x),bz=uintBitsToFloat(b.x),ad=uintBitsToFloat(a.y),bd=uintBitsToFloat(b.y);
    return az<bz || (az==bz && (ad<bd || (ad==bd && a.z<b.z)));
}
NUM_THREADS(256,1,1)
void main() {
    uint lane=gl_LocalInvocationID.x,group=gl_WorkGroupID.x;
    uvec4 best=uvec4(0u,0u,0xffffffffu,0xffffffffu);
    uint begin=u_hoverReduce.y>0.5 ? lane:group*256u+lane;
    uint end=u_hoverReduce.y>0.5 ? uint(u_hoverReduce.x):min(uint(u_hoverReduce.x),(group+1u)*256u);
    for (uint i=begin;i<end;i+=256u) { uvec4 v=b_input[i]; if (wins(v,best)) { best=v; } }
    sharedBest[lane]=best; barrier();
    for (uint stride=128u;stride>0u;stride>>=1u) {
        if (lane<stride && wins(sharedBest[lane+stride],sharedBest[lane])) { sharedBest[lane]=sharedBest[lane+stride]; }
        barrier();
    }
    if (lane==0u) {
        if (u_hoverReduce.y>0.5) { imageStore(t_result,ivec2(0,0),sharedBest[0]); }
        else { b_output[group]=sharedBest[0]; }
    }
}
