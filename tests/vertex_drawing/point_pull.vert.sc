$output v_texcoord0

#include <bgfx/bgfx_compute.sh>

BUFFER_RO(u_meshData, vec4, 0);
BUFFER_RO(u_pointIds, uint, 1);
uniform vec4 u_pointParams;
uniform vec4 u_pointBase;

void main()
{
    uint pointBase = uint(u_pointBase.x) + uint(u_pointBase.y) * 65536u;
    uint vertexIndex = u_pointIds[pointBase + uint(gl_InstanceID)];
    vec3 position = u_meshData[vertexIndex * 2u].xyz;
    uint cornerIndex = uint(gl_VertexID);
    // BR, TR, BL, TL: same BL-to-TR diagonal as the production triangles.
    vec2 corner = vec2(1.0 - float((cornerIndex >> 1u) & 1u), float(cornerIndex & 1u)) - vec2_splat(0.5);
    vec4 center = mul(u_modelViewProj, vec4(position, 1.0));
    vec2 ndcPerPixel = vec2(2.0 / u_pointParams.y, 2.0 / u_pointParams.z);
    center.xy += corner * u_pointParams.x * ndcPerPixel * center.w;
    gl_Position = center;
    v_texcoord0 = corner;
}
