$output v_texcoord0
#define MARKER_HIGHLIGHT 1
#include <bgfx/bgfx_shader.sh>
uniform vec4 u_markerQuery;
uniform vec4 u_markerOptions;
void main() {
    vec2 uv=vec2(float((uint(gl_VertexID)>>1u)&1u),float(uint(gl_VertexID)&1u));
#if MARKER_HIGHLIGHT
    vec2 pixel=u_markerQuery.xy+(uv*2.0-1.0)*u_markerOptions.z;
    uv=pixel/u_markerQuery.zw;
#endif
    gl_Position=vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,0.0,1.0);
    v_texcoord0=uv;
}
