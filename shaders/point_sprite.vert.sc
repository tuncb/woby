$output v_texcoord0

#include <bgfx/bgfx_compute.sh>

BUFFER_RO(u_meshData, vec4, 0);
BUFFER_RO(u_pointIds, uint, 1);
uniform vec4 u_pointParams[2];

void main()
{
    uint pointBase = uint(u_pointParams[1].x) + uint(u_pointParams[1].y) * 65536u;
    uint vertexIndex = u_pointIds[pointBase + uint(gl_InstanceID)];
    // Vertex is 32 bytes: position, normal, UV. Its first vec4 contains XYZ.
    vec3 position = u_meshData[vertexIndex * 2u].xyz;
    uint cornerIndex = uint(gl_VertexID);
    // BR, TR, BL, TL: two triangles sharing the BL-to-TR diagonal.
    vec2 corner = vec2(1.0 - float((cornerIndex >> 1u) & 1u), float(cornerIndex & 1u)) - vec2_splat(0.5);
    vec4 center = mul(u_modelViewProj, vec4(position, 1.0));
    vec2 ndcPerPixel = vec2(2.0 / u_pointParams[0].y, 2.0 / u_pointParams[0].z);
    center.xy += corner * u_pointParams[0].x * ndcPerPixel * center.w;
    gl_Position = center;
    v_texcoord0 = corner;
}
