// Compile-only prototype. Production programs and bindings are unchanged.
#include "../../shaders/native/woby.slang"
VertexOutput vs_quality_faces(uint corner : SV_VertexID) {
    uint sourceCorner = triangleIndexOffset() + corner;
    uint face = sourceCorner / 3;
    uint sourceVertex = root.pointIds[sourceCorner];
    uint record = face * 8; // Four values, flat normal, reserved word.
    VertexOutput v = {};
    v.position = clipPosition(mul(root.modelViewProj, float4(positionAt(sourceVertex), 1)));
    // Preserve vs_comparison: its stored flat normal is not transformed here.
    v.normal = float3(root.freeform[record+4], root.freeform[record+5], root.freeform[record+6]);
    v.uv = float2(root.freeform[record + uint(root.comparison.x)], 0);
    return v;
}
