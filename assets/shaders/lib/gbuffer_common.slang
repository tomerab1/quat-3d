// gbuffer_common.slang — the deferred GBuffer pass, shared by the built-in
// pass shader (gbuffer.slang) and the material-shader driver
// (material_pass_gbuffer.slang, ROADMAP A6).
//
// Rasterises meshes with the standard interleaved vertex layout and writes the
// deferred GBuffer: albedo, octahedral-encoded world normal, packed
// metallic/roughness/occlusion, clearcoat and emitted radiance. Surface
// attributes come from an IMaterial (lib/material.slang) chosen at compile
// time: gbuffer_fragment<M> is specialised for StandardMaterial by the
// built-in pass and for each authored material by the driver. Draws are
// instanced (R9.1): per-instance model + normal matrices are read through a
// buffer device address (lib/instancing.slang).
//
// Both programs declare the same parameters (this module), so one pipeline
// layout serves every material.

__exported import material;
import instancing;
import pbr;

// Mirrors engine::renderer::GBufferPushConstants (96 bytes).
public struct GBufferPushConstants {
    public float4x4      view_proj;
    public InstanceData* instances; // indexed by instance_index() (includes firstInstance)
    public float         time;      // material clock, seconds
    public float         pad;
    // xyz = camera position, w = 1; for an orthographic view w = 0 and xyz is
    // the unit direction toward the camera.
    public float4        camera;
}
[[vk::push_constant]] public GBufferPushConstants pc;

// Material parameter block (constant buffer + textures + sampler) in its own
// descriptor set, bound per draw via a descriptor buffer.
public ParameterBlock<PbrMaterial> g_material;

public struct GBufferVsIn {
    public float3 position : POSITION;
    public float3 normal   : NORMAL;
    public float2 uv       : TEXCOORD0;
    public float4 tangent  : TANGENT;   // xyz tangent, w bitangent sign
}

public struct GBufferVsOut {
    public float4 clip      : SV_Position;
    public float3 world_pos : POSITION0;
    public float3 world_n   : NORMAL;
    public float4 world_t   : TANGENT;
    public float2 uv        : TEXCOORD0;
}

public struct GBufferOut {
    public float4 albedo    : SV_Target0;   // rgb albedo, a = alpha
    public float2 normal    : SV_Target1;   // oct-encoded world normal
    public float4 material  : SV_Target2;   // r metallic, g roughness, b occlusion
    public float2 clearcoat : SV_Target3;   // r coat strength, g coat perceptual roughness
    public float4 emissive  : SV_Target4;   // rgb emitted radiance (linear HDR)
}

public GBufferVsOut gbuffer_vertex(GBufferVsIn input) {
    InstanceData inst = pc.instances[instance_index()];
    GBufferVsOut o;
    float4 world = inst.world_position(input.position);
    o.clip = mul(pc.view_proj, world);
    o.world_pos = world.xyz;

    // Cofactor normal matrix (non-uniform scale) + mirrored tangent handedness.
    o.world_n = normalize(inst.world_normal(input.normal));
    float4 t = inst.world_tangent(input.tangent);
    o.world_t = float4(normalize(t.xyz), t.w);
    o.uv = input.uv;
    return o;
}

// Component-wise sign that maps zero to +1 (octahedral encoding convention).
float2 sign_nonzero(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Octahedral encode a unit vector into [-1, 1]^2 (stored in an RG16F target).
public float2 oct_encode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(float2(e.y, e.x))) * sign_nonzero(e);
    }
    return e;
}

// Shading point -> MaterialInput: interpolants, the push-constant clock and
// camera, the bound material's constants and its built-in PBR evaluation.
public MaterialInput gbuffer_material_input(GBufferVsOut input) {
    MaterialInput m;
    m.uv = input.uv;
    m.world_pos = input.world_pos;
    m.normal = normalize(input.world_n);
    m.view_dir = pc.camera.w != 0.0 ? normalize(pc.camera.xyz - input.world_pos)
                                    : normalize(pc.camera.xyz);
    m.time = pc.time;
    m.params = g_material.params;
    m.standard = g_material.evaluate(input.uv);
    return m;
}

// Evaluates material M at this fragment.
public SurfaceSample gbuffer_evaluate<M : IMaterial>(GBufferVsOut input) {
    return M::evaluate(gbuffer_material_input(input));
}

// Packs surface attributes into the GBuffer targets.
public GBufferOut gbuffer_pack(GBufferVsOut input, SurfaceSample s) {
    // Build the world-space normal from the tangent-space material normal.
    float3 n = normalize(input.world_n);
    float3 t = normalize(input.world_t.xyz - n * dot(n, input.world_t.xyz));
    float3 b = cross(n, t) * input.world_t.w;
    float3 world_n = normalize(t * s.normal_ts.x + b * s.normal_ts.y + n * s.normal_ts.z);

    // Specular AA at GBuffer-write time (the deferred lighting pass is compute
    // and has no derivatives): fold sub-pixel normal variance into roughness.
    float roughness = pbr_specular_aa(world_n, saturate(s.roughness));
    float coat_roughness = pbr_specular_aa(world_n, saturate(s.clearcoat_roughness));

    GBufferOut o;
    o.albedo    = float4(saturate(s.albedo), s.alpha);
    o.normal    = oct_encode(world_n);
    o.material  = float4(saturate(s.metallic), roughness, saturate(s.occlusion), 1.0);
    o.clearcoat = float2(saturate(s.clearcoat), coat_roughness);
    o.emissive  = float4(max(s.emissive, float3(0.0)), 0.0);
    return o;
}
