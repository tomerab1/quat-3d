// skinning.slang — GPU vertex skinning compute pass (Phase 5, Slice 5.4).
//
// Reads the base (bind-pose) vertex buffer + per-vertex joints/weights + the
// per-joint skinning matrices, and writes a skinned vertex buffer with the same
// interleaved layout as asset::Vertex, so the GBuffer pass can bind it unchanged.
// Buffers are accessed as raw bytes (ByteAddressBuffer) and joint matrices are
// blended column-by-column, so there is no struct/matrix layout dependency.
//
// Velocity (R11): when write_previous is set, the same vertex is also skinned
// with last frame's palette (g_prev_joints) and its position written to
// g_prev_positions (one float4 per vertex), which the GBuffer reads to compute
// the vertex's motion.

ByteAddressBuffer   g_vertices_in;
ByteAddressBuffer   g_skin;
ByteAddressBuffer   g_joints;
RWByteAddressBuffer g_vertices_out;
ByteAddressBuffer   g_prev_joints;
RWByteAddressBuffer g_prev_positions;

struct PushConstants {
    uint vertex_count;
    uint joint_count;    // matrices in g_joints / g_prev_joints; always >= 1
    uint write_previous; // 1: also skin with g_prev_joints into g_prev_positions
    uint pad;
}
[[vk::push_constant]] PushConstants pc;

// Byte strides of the vertex-indexed buffers and the joint matrix array.
static const uint VTX_STRIDE  = 48; // asset::Vertex  (pos12 + normal12 + uv8 + tangent16)
static const uint SKIN_STRIDE = 32; // asset::SkinVertex (uint4 joints + float4 weights)
static const uint MAT_STRIDE  = 64; // glm::mat4
static const uint PREV_STRIDE = 16; // float4 previous position

// Weighted blend of the joint matrices, column by column (column-major).
// Indices are clamped: one past the skin's joints would read out of bounds
// (the loader rejects weighted ones; zero-weight ones still load).
float4x4 blend_columns(ByteAddressBuffer joints, uint4 indices, float4 weights) {
    float4 c0 = float4(0, 0, 0, 0);
    float4 c1 = c0;
    float4 c2 = c0;
    float4 c3 = c0;
    [unroll]
    for (int k = 0; k < 4; ++k) {
        uint  j = min(indices[k], pc.joint_count - 1);
        float w = weights[k];
        uint  mbase = j * MAT_STRIDE;
        c0 += w * joints.Load<float4>(mbase + 0);
        c1 += w * joints.Load<float4>(mbase + 16);
        c2 += w * joints.Load<float4>(mbase + 32);
        c3 += w * joints.Load<float4>(mbase + 48);
    }
    // Rows of the returned matrix are the blended columns.
    return float4x4(c0, c1, c2, c3);
}

float3 skin_point(float4x4 cols, float3 p) {
    return (cols[0] * p.x + cols[1] * p.y + cols[2] * p.z + cols[3]).xyz;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.vertex_count) {
        return;
    }

    uint vbase = i * VTX_STRIDE;
    float3 pos = g_vertices_in.Load<float3>(vbase + 0);
    float3 nrm = g_vertices_in.Load<float3>(vbase + 12);
    float2 uv  = g_vertices_in.Load<float2>(vbase + 24);
    float4 tan = g_vertices_in.Load<float4>(vbase + 32);

    uint   sbase   = i * SKIN_STRIDE;
    uint4  joints  = g_skin.Load<uint4>(sbase + 0);
    float4 weights = g_skin.Load<float4>(sbase + 16);

    // M * point (w=1) and M_3x3 * vector for the basis.
    float4x4 m = blend_columns(g_joints, joints, weights);
    float3 spos = skin_point(m, pos);
    float3 snrm = normalize(m[0].xyz * nrm.x + m[1].xyz * nrm.y + m[2].xyz * nrm.z);
    float3 stan = normalize(m[0].xyz * tan.x + m[1].xyz * tan.y + m[2].xyz * tan.z);

    uint obase = i * VTX_STRIDE;
    g_vertices_out.Store<float3>(obase + 0, spos);
    g_vertices_out.Store<float3>(obase + 12, snrm);
    g_vertices_out.Store<float2>(obase + 24, uv);
    g_vertices_out.Store<float4>(obase + 32, float4(stan, tan.w));

    if (pc.write_previous != 0) {
        float4x4 pm = blend_columns(g_prev_joints, joints, weights);
        g_prev_positions.Store<float4>(i * PREV_STRIDE, float4(skin_point(pm, pos), 1.0));
    }
}
