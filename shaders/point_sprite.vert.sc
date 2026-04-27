$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx/bgfx_shader.sh>

uniform vec4 u_pointParams;

void main()
{
    vec4 center = mul(u_modelViewProj, vec4(a_position, 1.0));
    vec2 ndcPerPixel = vec2(2.0 / u_pointParams.y, 2.0 / u_pointParams.z);
    center.xy += a_texcoord0 * u_pointParams.x * ndcPerPixel * center.w;
    gl_Position = center;
    v_texcoord0 = a_texcoord0;
}
