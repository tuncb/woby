$input v_normal

#include <bgfx/bgfx_shader.sh>

uniform vec4 u_color;

void main()
{
    vec3 normal = normalize(v_normal);
    vec3 light = normalize(vec3(0.35, 0.6, 0.7));
    float diffuse = max(dot(normal, light), 0.0);
    vec3 litColor = u_color.rgb * (vec3_splat(0.32) + diffuse * 0.68);
    gl_FragColor = vec4(litColor, u_color.a);
}
