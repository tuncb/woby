$input a_position, a_normal, a_texcoord0
$output v_normal

#include <bgfx/bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;
}
