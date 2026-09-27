$input v_texcoord0
#define MARKER_MSAA 1
#define MARKER_COMPACT 1
#include <bgfx/bgfx_shader.sh>
#if MARKER_MSAA
SAMPLER2DMS(s_markerIds,0);
#else
SAMPLER2D(s_markerIds,0);
#endif
SAMPLER2D(s_markerResult,1);
uniform vec4 u_markerQuery;
uniform vec4 u_markerOptions;
void main() {
    vec4 winner=texelFetch(s_markerResult,ivec2(0,0),0);
    if (winner.x==0.0 && winner.y==0.0) { discard; }
    ivec2 pixel=ivec2(v_texcoord0*u_markerQuery.zw);
    if (pixel.x<0 || pixel.y<0 || pixel.x>=int(u_markerQuery.z) || pixel.y>=int(u_markerQuery.w)) { discard; }
    float coverage=0.0;
    for (int s=0;s<int(u_markerOptions.x);++s) {
#if MARKER_MSAA
        vec4 v=texelFetch(s_markerIds,pixel,s);
#else
        vec4 v=texelFetch(s_markerIds,pixel,0);
#endif
#if MARKER_COMPACT
        uvec4 bytes=uvec4(round(v*255.0));
        v=vec4(float(bytes.x|(bytes.y<<8u)),float(bytes.z|(bytes.w<<8u)),0.0,0.0);
#endif
        if (v.x==winner.x && v.y==winner.y && v.w==winner.w) { coverage+=1.0; }
    }
    if (coverage==0.0) { discard; }
    gl_FragColor=vec4(1.0,.68,.05,coverage/u_markerOptions.x);
}
