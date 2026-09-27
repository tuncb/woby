$input

#include <bgfx/bgfx_shader.sh>

uniform vec4 u_color;

void main()
{
    if (u_color.a <= 0.000001) { discard; }
    gl_FragData[1] = vec4_splat(0.0);
    gl_FragData[0] = u_color;
}
