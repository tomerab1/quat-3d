// skinning.slang — GPU vertex skinning compute pass (Phase 5, Slice 5.4).
//
// Reads the base (bind-pose) vertex buffer + per-vertex joints/weights + the
// per-joint skinning matrices, and writes a skinned vertex buffer with the same
// interleaved layout as asset::Vertex, so the GBuffer pass can bind it unchanged.
// Buffers are accessed as raw bytes (ByteAddressBuffer) and joint matrices are
// blended column-by-column, so there is no struct/matrix layout dependency.

[[vk::binding(0)]] ByteAddressBuffer   g_vertices_in;
[[vk::binding(1)]] ByteAddressBuffer   g_skin;
[[vk::binding(2)]] ByteAddressBuffer   g_joints;
[[vk::binding(3)]] RWByteAddressBuffer g_vertices_out;

struct PushConstants {
    uint vertex_count;
}
[[vk::push_constant]] PushConstants pc;

// Byte strides of the three vertex-indexed buffers and the joint matrix array.
static const uint VTX_STRIDE  = 48; // asset::Vertex  (pos12 + normal12 + uv8 + tangent16)
static const uint SKIN_STRIDE = 32; // asset::SkinVertex (uint4 joints + float4 weights)
static const uint MAT_STRIDE  = 64; // glm::mat4

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

    // Weighted blend of the joint matrices, column by column (column-major).
    float4 c0 = float4(0, 0, 0, 0);
    float4 c1 = c0;
    float4 c2 = c0;
    float4 c3 = c0;
    [unroll]
    for (int k = 0; k < 4; ++k) {
        uint  j = joints[k];
        float w = weights[k];
        uint  mbase = j * MAT_STRIDE;
        c0 += w * g_joints.Load<float4>(mbase + 0);
        c1 += w * g_joints.Load<float4>(mbase + 16);
        c2 += w * g_joints.Load<float4>(mbase + 32);
        c3 += w * g_joints.Load<float4>(mbase + 48);
    }

    // M * point (w=1) and M_3x3 * vector for the basis.
    float3 spos = (c0 * pos.x + c1 * pos.y + c2 * pos.z + c3).xyz;
    float3 snrm = normalize(c0.xyz * nrm.x + c1.xyz * nrm.y + c2.xyz * nrm.z);
    float3 stan = normalize(c0.xyz * tan.x + c1.xyz * tan.y + c2.xyz * tan.z);

    uint obase = i * VTX_STRIDE;
    g_vertices_out.Store<float3>(obase + 0, spos);
    g_vertices_out.Store<float3>(obase + 12, snrm);
    g_vertices_out.Store<float2>(obase + 24, uv);
    g_vertices_out.Store<float4>(obase + 32, float4(stan, tan.w));
}
