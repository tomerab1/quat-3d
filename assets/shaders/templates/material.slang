// A material shader for quat-3d. Save it as assets/shaders/materials/<name>.slang
// (or send it with shader.create) and select it on an entity with the
// primitive component's `shader` field.
//
// The module implements IMaterial (lib/material.slang): one static function
// that turns a shading point into surface attributes. It runs in the deferred
// GBuffer pass, so lighting, shadows, reflections, bloom and exposure apply.
//
// MaterialInput (what you get):
//   uv          float2  mesh texture coordinates
//   world_pos   float3  world-space position (metres)
//   normal      float3  world-space vertex normal (unit)
//   view_dir    float3  unit vector from the surface toward the camera
//   time        float   seconds; animate with it
//   params      PbrMaterialParams: base_color_factor, emissive_factor,
//               metallic_factor, roughness_factor and shader_params[0..1]
//               (two float4s of free parameters set per entity)
//   standard    SurfaceSample: the built-in PBR result (start from it)
//
// SurfaceSample (what you return; values outside the ranges are clamped):
//   albedo      float3  linear base colour, 0..1
//   normal_ts   float3  tangent-space normal, (0, 0, 1) = unperturbed
//   metallic    float   0..1
//   roughness   float   0..1
//   occlusion   float   0..1 ambient occlusion
//   emissive    float3  emitted radiance, linear HDR (can exceed 1: glows)
//   alpha       float   below 0.5 cuts the pixel out (shadows ignore it)
//   clearcoat, clearcoat_roughness   float  optional varnish layer
//
// Keep the last line: it tells the engine which struct is the material.

import material;

struct MyMaterial : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;

        // A slowly pulsing checkerboard tinted by the entity's colour.
        float2 cell = floor(input.uv * 8.0);
        float checker = fmod(cell.x + cell.y, 2.0);
        float pulse = 0.5 + 0.5 * sin(input.time * 2.0);
        s.albedo = input.params.base_color_factor.rgb * lerp(0.35, 1.0, checker);
        s.roughness = lerp(0.8, 0.3, checker);
        s.emissive = input.params.emissive_factor.rgb * checker * pulse;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = MyMaterial;
