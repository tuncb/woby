#include <bgfx/bgfx_compute.sh>
#if MARKER_MSAA
SAMPLER2DMS(s_markerIds,0);
#else
SAMPLER2D(s_markerIds,0);
#endif
IMAGE2D_WO(t_markerResult,rgba32f,1);
IMAGE2D_WO(t_markerAudit,rgba32f,2);
uniform vec4 u_markerQuery;
uniform vec4 u_markerOptions; // sample count, audit enabled, highlight radius, unused
uint markerId(vec4 p) { return uint(p.x) | (uint(p.y)<<16u); }
NUM_THREADS(1,1,1)
void main() {
    vec4 best=vec4_splat(0.0); float bestDistance=1e30;
    for (int i=0;i<49;++i) {
        ivec2 p=ivec2(floor(u_markerQuery.xy))+ivec2(i%7-3,i/7-3);
        bool inside=p.x>=0 && p.y>=0 && p.x<int(u_markerQuery.z) && p.y<int(u_markerQuery.w);
        vec2 delta=vec2(p)+vec2_splat(.5)-u_markerQuery.xy;
        float distance=dot(delta,delta);
        for (int s=0;s<int(u_markerOptions.x);++s) {
            vec4 v=vec4_splat(0.0);
            if (inside) {
#if MARKER_MSAA
                v=texelFetch(s_markerIds,p,s);
#else
                v=texelFetch(s_markerIds,p,0);
#endif
            }
#if MARKER_COMPACT
            uvec4 bytes=uvec4(round(v*255.0));
            v=vec4(float(bytes.x|(bytes.y<<8u)),float(bytes.z|(bytes.w<<8u)),0.0,0.0);
#endif
            if (u_markerOptions.y>.5) { imageStore(t_markerAudit,ivec2(i*4+s,0),v); }
            if (inside && distance<=9.0 && markerId(v)!=0u &&
                (markerId(best)==0u || distance<bestDistance || (distance==bestDistance &&
                (v.z<best.z || (v.z==best.z && (markerId(v)<markerId(best) || (markerId(v)==markerId(best) && v.w<best.w))))))) {
                best=v; bestDistance=distance;
            }
        }
    }
    imageStore(t_markerResult,ivec2(0,0),best);
}
