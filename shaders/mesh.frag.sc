$input v_normal

#include <bgfx/bgfx_shader.sh>

void main()
{
    vec3 normal = normalize(v_normal);
    vec3 light = normalize(vec3(0.35, 0.6, 0.7));
    float diffuse = max(dot(normal, light), 0.0);
    vec3 color = vec3_splat(0.18) + diffuse * vec3(0.65, 0.74, 0.86);
    gl_FragColor = vec4(color, 1.0);
}
