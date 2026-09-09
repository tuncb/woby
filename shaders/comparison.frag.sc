$input v_normal, v_texcoord0

#include <bgfx/bgfx_shader.sh>

uniform vec4 u_color;
uniform vec4 u_comparison;

void main()
{
    vec3 color = u_color.rgb;
    if (u_comparison.z > 1.5)
    {
        float amount = clamp(v_texcoord0.x, 0.0, 1.0);
        if (u_comparison.w > 0.5) { amount = 1.0 - amount; }
        color = amount <= 0.5
            ? mix(vec3(0.18, 0.48, 0.85), vec3(1.0, 0.82, 0.3), amount * 2.0)
            : mix(vec3(1.0, 0.82, 0.3), vec3(0.94, 0.22, 0.055), (amount - 0.5) * 2.0);
        if (v_texcoord0.x < 0.0) { color = vec3(1.0, 0.0, 1.0); }
        if (v_texcoord0.x < -1.5) { color = vec3(0.56, 0.61, 0.67); }
    }
    else if (u_comparison.z > 0.5)
    {
        color = vec3(0.56, 0.61, 0.67);
        if (v_texcoord0.x > u_comparison.x)
        {
            float amount = 1.0;
            if (u_comparison.y > u_comparison.x)
            {
                amount = clamp((v_texcoord0.x - u_comparison.x) / (u_comparison.y - u_comparison.x), 0.0, 1.0);
            }
            color = mix(vec3(1.0, 0.77, 0.31), vec3(0.94, 0.22, 0.055), amount);
        }
    }
    vec3 normal = normalize(v_normal + vec3_splat(0.00000001));
    float light = 0.6 + 0.4 * abs(dot(normal, normalize(vec3(0.35, 0.6, 0.7))));
    gl_FragColor = vec4(color * light, 1.0);
}
