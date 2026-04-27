$input v_texcoord0

#include <bgfx/bgfx_shader.sh>

uniform vec4 u_color;

void main()
{
    if (length(v_texcoord0) > 0.5) {
        discard;
    }

    gl_FragColor = u_color;
}
