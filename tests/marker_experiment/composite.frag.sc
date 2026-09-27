$input v_texcoord0
#include <bgfx/bgfx_shader.sh>
SAMPLER2D(s_markerColor,0);
void main() { gl_FragColor=texture2D(s_markerColor,v_texcoord0); }
