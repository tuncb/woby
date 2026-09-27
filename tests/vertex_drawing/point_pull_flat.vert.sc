$output v_texcoord0

#include <bgfx/bgfx_compute.sh>

BUFFER_RO(u_meshData, vec4, 0);
BUFFER_RO(u_pointIds, uint, 1);
uniform vec4 u_pointParams;
uniform vec4 u_pointBase;

void main()
{
    uint pointBase = uint(u_pointBase.x) + uint(u_pointBase.y) * 65536u;
    uint vertexIndex = u_pointIds[pointBase + uint(gl_VertexID) / 6u];
    vec3 position = u_meshData[vertexIndex * 2u].xyz;
    uint cornerIndex = uint(gl_VertexID) % 6u;
    // BL, BR, TR, BL, TR, TL: exactly the production triangle order.
    vec2 corner = vec2((cornerIndex == 1u || cornerIndex == 2u || cornerIndex == 4u) ? 0.5 : -0.5,
        (cornerIndex == 2u || cornerIndex >= 4u) ? 0.5 : -0.5);
    vec4 center = mul(u_modelViewProj, vec4(position, 1.0));
    vec2 ndcPerPixel = vec2(2.0 / u_pointParams.y, 2.0 / u_pointParams.z);
    center.xy += corner * u_pointParams.x * ndcPerPixel * center.w;
    gl_Position = center;
    v_texcoord0 = corner;
}
