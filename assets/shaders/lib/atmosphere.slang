// atmosphere.slang — physically-based sky model (Phase 11, Slice 11.1).
//
// Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique": Earth's atmosphere as Rayleigh + Mie + ozone media, factored into
// three LUTs — transmittance (static), multiple scattering (static), and a
// per-frame sky-view LUT the background samples. Units are kilometres.

// Planet geometry.
public static const float ATMOS_GROUND_RADIUS = 6360.0; // km
public static const float ATMOS_TOP_RADIUS    = 6460.0; // km
// The camera sits this far above the ground for sky evaluation (the engine's
// world units are metres and scenes are small, so a fixed viewpoint suffices
// until planetary-scale traversal exists).
public static const float ATMOS_VIEW_HEIGHT = 6360.2; // km (200 m up)

// Rayleigh: scattering only, exponential falloff (8 km scale height).
static const float3 RAYLEIGH_SCATTER = float3(0.005802, 0.013558, 0.033100); // /km
// Mie: scattering + absorption, 1.2 km scale height, Cornette-Shanks g.
static const float  MIE_SCATTER    = 0.003996;
static const float  MIE_ABSORB     = 0.004440;
public static const float MIE_PHASE_G = 0.8;
// Ozone: absorption only, tent profile centred at 25 km (15 km half-width).
static const float3 OZONE_ABSORB = float3(0.000650, 0.001881, 0.000085);

public static const float ATMOS_PI = 3.14159265358979;

// Medium properties at altitude h (km above ground).
public struct MediumSample {
    public float3 rayleigh_scatter;
    public float  mie_scatter;
    public float3 extinction;
}

public MediumSample sample_medium(float h) {
    float rayleigh_density = exp(-h / 8.0);
    float mie_density = exp(-h / 1.2);
    float ozone_density = max(0.0, 1.0 - abs(h - 25.0) / 15.0);

    MediumSample m;
    m.rayleigh_scatter = RAYLEIGH_SCATTER * rayleigh_density;
    m.mie_scatter = MIE_SCATTER * mie_density;
    m.extinction = m.rayleigh_scatter + (MIE_SCATTER + MIE_ABSORB) * mie_density +
                   OZONE_ABSORB * ozone_density;
    return m;
}

public float rayleigh_phase(float cos_theta) {
    return 3.0 / (16.0 * ATMOS_PI) * (1.0 + cos_theta * cos_theta);
}

// Cornette-Shanks approximation for the Mie phase function.
public float mie_phase(float cos_theta) {
    float g = MIE_PHASE_G;
    float g2 = g * g;
    float num = 3.0 * (1.0 - g2) * (1.0 + cos_theta * cos_theta);
    float den = 8.0 * ATMOS_PI * (2.0 + g2) * pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5);
    return num / den;
}

// Distance from a point at radius r (zenith cosine mu) to the atmosphere top.
// Assumes the ray hits the top shell (mu paths that hit the ground are the
// caller's concern).
public float distance_to_top(float r, float mu) {
    float disc = r * r * (mu * mu - 1.0) + ATMOS_TOP_RADIUS * ATMOS_TOP_RADIUS;
    return max(0.0, -r * mu + sqrt(max(disc, 0.0)));
}

public float distance_to_ground(float r, float mu) {
    float disc = r * r * (mu * mu - 1.0) + ATMOS_GROUND_RADIUS * ATMOS_GROUND_RADIUS;
    if (disc < 0.0) return -1.0;
    float d = -r * mu - sqrt(disc);
    return d >= 0.0 ? d : -1.0;
}

public bool intersects_ground(float r, float mu) {
    return mu < 0.0 && r * r * (mu * mu - 1.0) +
                           ATMOS_GROUND_RADIUS * ATMOS_GROUND_RADIUS >= 0.0;
}

// ---------------------------------------------------------------------------
// Transmittance LUT mapping (Bruneton's distance parameterisation): texel uv
// <-> (r, mu) so precision concentrates near the horizon.
// ---------------------------------------------------------------------------
public float2 transmittance_uv_from(float r, float mu) {
    float H = sqrt(ATMOS_TOP_RADIUS * ATMOS_TOP_RADIUS -
                   ATMOS_GROUND_RADIUS * ATMOS_GROUND_RADIUS);
    float rho = sqrt(max(r * r - ATMOS_GROUND_RADIUS * ATMOS_GROUND_RADIUS, 0.0));
    float d = distance_to_top(r, mu);
    float d_min = ATMOS_TOP_RADIUS - r;
    float d_max = rho + H;
    float x_mu = (d - d_min) / max(d_max - d_min, 1e-6);
    float x_r = rho / H;
    return float2(x_mu, x_r);
}

public void transmittance_params_from_uv(float2 uv, out float r, out float mu) {
    float H = sqrt(ATMOS_TOP_RADIUS * ATMOS_TOP_RADIUS -
                   ATMOS_GROUND_RADIUS * ATMOS_GROUND_RADIUS);
    float rho = H * uv.y;
    r = sqrt(rho * rho + ATMOS_GROUND_RADIUS * ATMOS_GROUND_RADIUS);
    float d_min = ATMOS_TOP_RADIUS - r;
    float d_max = rho + H;
    float d = d_min + uv.x * (d_max - d_min);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

public float3 sample_transmittance(Texture2D lut, SamplerState samp, float r, float mu) {
    return lut.SampleLevel(samp, transmittance_uv_from(r, mu), 0.0).rgb;
}

// Transmittance toward a light from radius r, 0 when the planet blocks it
// (the LUT is only defined for rays that reach the top of the atmosphere).
public float3 sun_transmittance_above_ground(Texture2D lut, SamplerState samp, float r,
                                             float mu) {
    return intersects_ground(r, mu) ? float3(0.0, 0.0, 0.0) : sample_transmittance(lut, samp, r, mu);
}

// ---------------------------------------------------------------------------
// Multiple-scattering LUT mapping: x = sun zenith cosine, y = altitude.
// ---------------------------------------------------------------------------
public float3 sample_multiscatter(Texture2D lut, SamplerState samp, float r, float mu_sun) {
    float2 uv = float2(mu_sun * 0.5 + 0.5,
                       (r - ATMOS_GROUND_RADIUS) /
                           (ATMOS_TOP_RADIUS - ATMOS_GROUND_RADIUS));
    return lut.SampleLevel(samp, uv, 0.0).rgb;
}

// ---------------------------------------------------------------------------
// Sky-view LUT mapping (Hillaire): lat-long around the camera with a
// square-root latitude warp that concentrates texels at the horizon. The
// x axis is the azimuth relative to the sun.
// ---------------------------------------------------------------------------
public float2 skyview_uv_from(float view_zenith_cos, float light_azimuth_cos,
                              bool below_horizon) {
    float view_zenith = acos(clamp(view_zenith_cos, -1.0, 1.0));
    float horizon = acos(clamp(ATMOS_GROUND_RADIUS / ATMOS_VIEW_HEIGHT, -1.0, 1.0));
    float beta = ATMOS_PI * 0.5 - horizon; // horizon angle below zenith-90
    float v;
    if (!below_horizon) {
        float coord = 1.0 - sqrt(saturate(1.0 - view_zenith / (ATMOS_PI * 0.5 + beta)));
        v = coord * 0.5;
    } else {
        float coord = (view_zenith - (ATMOS_PI * 0.5 + beta)) / (ATMOS_PI * 0.5 - beta);
        v = 0.5 + 0.5 * sqrt(saturate(coord));
    }
    float u = acos(clamp(light_azimuth_cos, -1.0, 1.0)) / ATMOS_PI;
    return float2(u, v);
}

public void skyview_params_from_uv(float2 uv, out float view_zenith, out float light_azimuth) {
    float horizon = acos(clamp(ATMOS_GROUND_RADIUS / ATMOS_VIEW_HEIGHT, -1.0, 1.0));
    float beta = ATMOS_PI * 0.5 - horizon;
    if (uv.y < 0.5) {
        float coord = 1.0 - uv.y * 2.0;
        view_zenith = (1.0 - coord * coord) * (ATMOS_PI * 0.5 + beta);
    } else {
        float coord = uv.y * 2.0 - 1.0;
        view_zenith = (ATMOS_PI * 0.5 + beta) + coord * coord * (ATMOS_PI * 0.5 - beta);
    }
    light_azimuth = uv.x * ATMOS_PI;
}

// The sky-view LUT stores luminance x a per-update scale (so twilight and
// night skies keep RGBA16F precision; R22.2.1) and log2(1 / scale) in alpha,
// constant over the image, so bilinear filtering decodes exactly.
public float3 decode_skyview(float4 texel) {
    return texel.rgb * exp2(texel.a);
}

// Samples the sky-view LUT for a world-space view direction (y up) and sun
// direction (pointing FROM the scene TOWARD the sun). Returns the sky
// luminance per unit extra-atmospheric sun irradiance (decoded).
public float3 sample_skyview(Texture2D lut, SamplerState samp, float3 view_dir,
                             float3 sun_dir) {
    float view_zenith_cos = view_dir.y;
    float horizon_cos = -sqrt(max(1.0 - (ATMOS_GROUND_RADIUS * ATMOS_GROUND_RADIUS) /
                                            (ATMOS_VIEW_HEIGHT * ATMOS_VIEW_HEIGHT),
                                  0.0));
    bool below = view_zenith_cos < horizon_cos;

    // Azimuth of the view relative to the sun, in the horizontal plane.
    float3 view_h = normalize(float3(view_dir.x, 0.0, view_dir.z) +
                              float3(1e-5, 0.0, 0.0));
    float3 sun_h = normalize(float3(sun_dir.x, 0.0, sun_dir.z) + float3(1e-5, 0.0, 0.0));
    float azimuth_cos = dot(view_h, sun_h);

    float2 uv = skyview_uv_from(view_zenith_cos, azimuth_cos, below);
    return decode_skyview(lut.SampleLevel(samp, uv, 0.0));
}

// Ambient light on the cloud layer (LUT units) from the sky near the zenith:
// 0.8 of it plus a bluish floor for the clouds' undersides. The floor scales
// with the sky's brightness (full by a clear day's zenith of ~0.05 per unit
// sun, R22.2.1): a constant would light clouds at night, where the
// pre-exposure magnifies the sky ~10⁷ times.
public float3 cloud_ambient_from_zenith(float3 zenith) {
    float day = saturate(dot(zenith, float3(0.2126, 0.7152, 0.0722)) / 0.05);
    return zenith * 0.8 + float3(0.008, 0.011, 0.016) * day;
}

// ---------------------------------------------------------------------------
// In-scattering along a ray (Hillaire 2020, sec. 5): single scattering with
// the real phase functions through the transmittance LUT plus the
// multiple-scattering LUT's isotropic term, integrated analytically per step.
// From radius r along zenith cosine mu for `length` km; the sun's zenith
// cosine is taken as constant along the ray (rays from the ground are short
// next to the planet radius; Hillaire's ground-level sky-view does the same).
// Returns luminance per unit sun irradiance; `throughput` is the ray's
// transmittance.
// ---------------------------------------------------------------------------
public float3 integrate_inscattering(Texture2D transmittance_lut, Texture2D multiscatter_lut,
                                     SamplerState samp, float r, float mu, float mu_sun,
                                     float cos_theta, float length, int steps,
                                     out float3 throughput) {
    float dt = length / float(steps);
    float phase_r = rayleigh_phase(cos_theta);
    float phase_m = mie_phase(cos_theta);
    float3 luminance = float3(0.0, 0.0, 0.0);
    throughput = float3(1.0, 1.0, 1.0);
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + 0.5) * dt;
        float rt = sqrt(r * r + t * t + 2.0 * r * t * mu);
        rt = clamp(rt, ATMOS_GROUND_RADIUS + 1e-4, ATMOS_TOP_RADIUS);
        MediumSample m = sample_medium(rt - ATMOS_GROUND_RADIUS);
        float3 step_trans = exp(-m.extinction * dt);
        float3 sun_trans = intersects_ground(rt, mu_sun)
                               ? float3(0.0, 0.0, 0.0)
                               : sample_transmittance(transmittance_lut, samp, rt, mu_sun);
        float3 multi = sample_multiscatter(multiscatter_lut, samp, rt, mu_sun);
        float3 scatter_phased = m.rayleigh_scatter * phase_r + m.mie_scatter.xxx * phase_m;
        float3 scatter_total = m.rayleigh_scatter + m.mie_scatter.xxx;
        float3 s = scatter_phased * sun_trans + scatter_total * multi;
        luminance += throughput * (s - s * step_trans) / max(m.extinction, 1e-6);
        throughput *= step_trans;
    }
    return luminance;
}

// ---------------------------------------------------------------------------
// Ground (R22.2.1, issues #13 / #98). The planet surface below the horizon
// is Lambertian with ATMOS_GROUND_ALBEDO (also the multiple-scattering LUT's
// ground bounce), lit by
//   - each light's transmitted direct beam, E · T(mu) · max(mu, 0),
//   - the skylight that light scatters down, E · E_sky(mu) (the sky
//     irradiance LUT: the cosine-weighted hemisphere of in-scattered
//     luminance per unit irradiance, Bruneton & Neyret 2008's irradiance
//     texture),
//   - the natural night-sky floor (stars, airglow, zodiacal light: ~10⁻³ lux
//     on a clear moonless night, Jensen et al. 2001),
// and seen through the camera-to-ground transmittance and in-scattering of
// the sky-view LUT (aerial perspective), so it fades into the horizon.
// Irradiances are relative to the extra-atmospheric sun (the LUT's unit).
// ---------------------------------------------------------------------------
// Grey-green terrestrial albedo (vegetation and soil average ~0.2-0.3).
public static const float3 ATMOS_GROUND_ALBEDO = float3(0.24, 0.25, 0.21);

// Sky irradiance LUT: SKY_IRRADIANCE_TEXELS texels over the light's zenith
// cosine in [SKY_IRRADIANCE_MU_MIN, 1] (RGBA32F, read with Load and
// interpolated geometrically: the irradiance spans decades across twilight).
public static const uint  SKY_IRRADIANCE_TEXELS = 128;
public static const float SKY_IRRADIANCE_MU_MIN = -0.5;

public float sky_irradiance_mu_from_texel(uint x) {
    return SKY_IRRADIANCE_MU_MIN +
           (1.0 - SKY_IRRADIANCE_MU_MIN) * float(x) / float(SKY_IRRADIANCE_TEXELS - 1);
}

public float3 sample_sky_irradiance(Texture2D lut, float mu_light) {
    float x = saturate((mu_light - SKY_IRRADIANCE_MU_MIN) / (1.0 - SKY_IRRADIANCE_MU_MIN)) *
              float(SKY_IRRADIANCE_TEXELS - 1);
    uint i0 = min(uint(x), SKY_IRRADIANCE_TEXELS - 2);
    float f = x - float(i0);
    float3 a = max(lut.Load(int3(int(i0), 0, 0)).rgb, 1e-30);
    float3 b = max(lut.Load(int3(int(i0) + 1, 0, 0)).rgb, 1e-30);
    return exp2(lerp(log2(a), log2(b), f));
}

// Irradiance on horizontal ground from one light of extra-atmospheric
// irradiance `e` (relative) at zenith cosine `mu_light`.
public float3 ground_irradiance_from(Texture2D transmittance_lut, Texture2D sky_irradiance_lut,
                                     SamplerState samp, float mu_light, float e) {
    if (e <= 0.0) return float3(0.0, 0.0, 0.0);
    float r = ATMOS_GROUND_RADIUS + 1e-3;
    float3 direct = mu_light > 0.0
                        ? sample_transmittance(transmittance_lut, samp, r, mu_light) * mu_light
                        : float3(0.0, 0.0, 0.0);
    return e * (direct + sample_sky_irradiance(sky_irradiance_lut, mu_light));
}

// ---------------------------------------------------------------------------
// Volumetric cloud layer (Phase 11.2). Marched PER PIXEL by the lighting
// background (and per texel by the IBL bake) — the sky-view LUT is far too
// coarse for cloud shapes. Luminance is returned in sky-view LUT units; the
// caller applies the same brightness scale it uses for the LUT sky.
//
// All artistic parameters arrive as CloudParams (packed into two float4 push
// constants by the CPU — see renderer/cloud_settings.hpp), so the editor can
// drive them live. The per-pixel `jitter` offsets the march start within one
// step (interleaved gradient noise), turning step banding into fine noise.
// ---------------------------------------------------------------------------
static const int CLOUD_STEPS = 28;
static const int CLOUD_SUN_STEPS = 5;

public struct CloudParams {
    public float coverage;     // 0..1 sky fraction; <= 0 disables the layer
    public float extinction;   // /km at full density
    public float bottom_km;    // shell bottom altitude above the ground
    public float thickness_km; // shell thickness
    public float freq;         // 1 / base feature size in km
    public float detail;       // edge erosion strength (0..1)
    public float wind_x;       // accumulated wind drift (km, world xz)
    public float wind_z;
};

// Unpacks the two push-constant float4s (matches CloudSettings::pack on the
// CPU): a = (coverage, extinction, bottom_km, thickness_km), b = (freq,
// detail, wind drift x, wind drift z).
public CloudParams unpack_cloud_params(float4 a, float4 b) {
    CloudParams cp;
    cp.coverage = a.x;
    cp.extinction = a.y;
    cp.bottom_km = a.z;
    cp.thickness_km = a.w;
    cp.freq = b.x;
    cp.detail = b.y;
    cp.wind_x = b.z;
    cp.wind_z = b.w;
    return cp;
}

// Per-pixel march jitter (Jimenez interleaved gradient noise).
public float interleaved_gradient_noise(float2 px) {
    return frac(52.9829189 * frac(0.06711056 * px.x + 0.00583715 * px.y));
}

// Sin-free hash (Dave Hoskins' hash12). frac(sin(dot)) hashes need full-
// precision sin at huge arguments — fine on llvmpipe, but hardware fast-trig
// (NVIDIA) collapses the lattice into blocky cells. Never hash through sin.
float cloud_hash(float2 p) {
    float3 q = frac(float3(p.x, p.y, p.x) * 0.1031);
    q += dot(q, float3(q.y, q.z, q.x) + 33.33);
    return frac((q.x + q.y) * q.z);
}

float cloud_value_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = cloud_hash(i);
    float b = cloud_hash(i + float2(1, 0));
    float c = cloud_hash(i + float2(0, 1));
    float d = cloud_hash(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float cloud_fbm(float2 p) {
    // Rotate between octaves so lattice axes don't line up across scales
    // (axis-aligned streaking otherwise).
    const float2x2 rot = float2x2(0.80, 0.60, -0.60, 0.80);
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += amp * cloud_value_noise(p);
        p = mul(rot, p) * 2.17 + float2(13.7, 9.1);
        amp *= 0.5;
    }
    return v;
}

// Density at a world-space point (km, planet-centred altitude supplied).
float cloud_density(float3 p, float altitude_km, float bottom_r, float top_r, CloudParams cp) {
    float h01 = saturate((altitude_km - bottom_r) / max(top_r - bottom_r, 1e-3));
    float profile = smoothstep(0.0, 0.20, h01) * (1.0 - smoothstep(0.5, 1.0, h01));
    if (profile <= 0.0) return 0.0;

    // Wind advects the whole field; the detail layer drifts a little faster so
    // shapes churn and evolve instead of translating rigidly.
    float2 wind = float2(cp.wind_x, cp.wind_z);
    float2 uv = (p.xz + wind) * cp.freq;
    float base = cloud_fbm(uv);
    float coverage_t = 1.0 - cp.coverage;
    float d = saturate((base - coverage_t) / max(1.0 - coverage_t, 1e-3));
    // Erode edges more than cores for billowy silhouettes.
    float detail = cloud_fbm(uv * 5.31 + 17.7 + wind * cp.freq * 2.3);
    d = saturate(d - (1.0 - d) * detail * cp.detail);
    return d * d * (3.0 - 2.0 * d) * profile;
}

float cloud_shell_distance(float r, float mu, float shell_radius) {
    float disc = r * r * (mu * mu - 1.0) + shell_radius * shell_radius;
    if (disc < 0.0) return -1.0;
    return -r * mu + sqrt(disc);
}

float cloud_hg_phase(float cos_theta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * ATMOS_PI * pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5));
}

// Marches the cloud shell along `dir` (world space, y up) from the standard
// view height. `ambient` is the sky light bathing the clouds, in sky-view LUT
// units (callers sample the sky-view LUT near the zenith for it). `jitter`
// (0..1, per pixel) offsets the march start within one step to break up step
// banding. Returns rgb = in-scattered luminance (LUT units), a = transmittance
// (1 = clear).
public float4 march_clouds(float3 dir, float3 sun_dir, Texture2D transmittance_lut,
                           SamplerState samp, float3 ambient, CloudParams cp, float jitter) {
    if (cp.coverage <= 0.001) return float4(0.0, 0.0, 0.0, 1.0); // layer disabled
    float bottom_r = ATMOS_GROUND_RADIUS + cp.bottom_km;
    float top_r = bottom_r + cp.thickness_km;

    float r = ATMOS_VIEW_HEIGHT;
    float mu = dir.y;
    if (mu <= 0.02) return float4(0.0, 0.0, 0.0, 1.0); // below/at horizon: clear

    float t_out = cloud_shell_distance(r, mu, top_r);
    if (t_out <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);
    float t_in = max(cloud_shell_distance(r, mu, bottom_r), 0.0);
    float march_len = t_out - t_in;
    if (march_len <= 0.0 || march_len > 90.0) return float4(0.0, 0.0, 0.0, 1.0);

    float mu_sun = clamp(sun_dir.y, -1.0, 1.0);
    float cos_theta = dot(dir, sun_dir);
    // A sun below the horizon does not light the clouds directly (the LUT
    // has no meaning for rays into the planet; at night's pre-exposure any
    // leak would glow).
    float3 sun_trans = sun_transmittance_above_ground(
        transmittance_lut, samp, clamp(r + 2.0, ATMOS_GROUND_RADIUS, ATMOS_TOP_RADIUS), mu_sun);
    float3 sun_energy = sun_trans * 0.45;
    float phase = lerp(cloud_hg_phase(cos_theta, 0.55), cloud_hg_phase(cos_theta, -0.2), 0.3) +
                  0.12;
    // Aerial perspective: distant banks dissolve into haze.
    float distance_fade = exp(-t_in * 0.045);

    float dt = march_len / CLOUD_STEPS;
    float trans = 1.0;
    float3 lum = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < CLOUD_STEPS; ++i) {
        if (trans < 0.01) break;
        float t = t_in + (float(i) + jitter) * dt;
        float3 p = dir * t;
        float altitude = sqrt(r * r + t * t + 2.0 * r * t * mu);
        float dens = cloud_density(p, altitude, bottom_r, top_r, cp);
        if (dens <= 0.002) continue;

        float sigma = dens * cp.extinction;
        float shadow_depth = 0.0;
        float ds = cp.thickness_km / float(CLOUD_SUN_STEPS);
        for (int s = 1; s <= CLOUD_SUN_STEPS; ++s) {
            float3 sp = p + sun_dir * (float(s) * ds);
            float s_alt = altitude + sun_dir.y * float(s) * ds;
            shadow_depth += cloud_density(sp, s_alt, bottom_r, top_r, cp) * ds;
        }
        float light = exp(-shadow_depth * cp.extinction * 0.6);
        float powder = 1.0 - exp(-2.0 * sigma * dt);

        float3 scatter = sun_energy * light * phase * (0.7 + 0.3 * powder) + ambient;
        float step_trans = exp(-sigma * dt);
        lum += trans * scatter * (1.0 - step_trans);
        trans *= step_trans;
    }
    lum *= distance_fade;
    trans = lerp(1.0, trans, distance_fade);
    return float4(lum, trans);
}

// ---------------------------------------------------------------------------
// Cloud shadows (R13.1). Transmittance of the sun's light through the cloud
// layer along a line parallel to the sun: march it through the cloud shell
// and integrate the same density the sky shows (so shadows sit under the
// visible clouds), with the same multiple-scattering softening (x 0.6)
// march_clouds lights the clouds with. `offset_km` is a point of the line
// below the clouds, relative to the camera in km (the sky's frame;
// CloudParams' wind offset carries the camera's world position while the
// scene has weather, so clouds and their shadows stay anchored to the
// world). CloudShadowPass evaluates it once per texel of the cloud shadow
// map (cloud_shadow.slang); the lighting pass only samples that map. The
// CPU mirror is engine::renderer::cloud_sun_transmittance_reference.
//
// `sun_dir` must be at least CLOUD_SHADOW_MIN_SUN_Y above the horizon (the
// pass clamps the sun's elevation): at low sun the path through the thin
// shell grows long, and instead of giving up (a sudden switch to full sun)
// the march follows up to CLOUD_SHADOW_MAX_PATH_KM of it, so an overcast
// sky stays dark down to sunset. 1 = clear.
// ---------------------------------------------------------------------------
public static const int   CLOUD_SHADOW_STEPS = 16;
public static const float CLOUD_SHADOW_MIN_SUN_Y = 0.0523; // sin(3 deg)
public static const float CLOUD_SHADOW_MAX_PATH_KM = 80.0;

public float cloud_sun_transmittance(float3 offset_km, float3 sun_dir, CloudParams cp) {
    if (cp.coverage <= 0.001) return 1.0;
    float bottom_r = ATMOS_GROUND_RADIUS + cp.bottom_km;
    float top_r = bottom_r + cp.thickness_km;
    // Planet-centred position: the camera stands at ATMOS_VIEW_HEIGHT.
    float3 p0 = float3(offset_km.x, ATMOS_VIEW_HEIGHT + offset_km.y, offset_km.z);
    float r = length(p0);
    float mu = dot(p0, sun_dir) / r;
    float t_out = cloud_shell_distance(r, mu, top_r);
    if (t_out <= 0.0) return 1.0;
    float t_in = r < bottom_r ? max(cloud_shell_distance(r, mu, bottom_r), 0.0) : 0.0;
    float len = min(t_out - t_in, CLOUD_SHADOW_MAX_PATH_KM);
    if (len <= 0.0) return 1.0;
    float dt = len / float(CLOUD_SHADOW_STEPS);
    float depth = 0.0;
    for (int i = 0; i < CLOUD_SHADOW_STEPS; ++i) {
        float t = t_in + (float(i) + 0.5) * dt; // midpoint rule
        float3 p = p0 + sun_dir * t;
        float3 rel = p - float3(0.0, ATMOS_VIEW_HEIGHT, 0.0);
        depth += cloud_density(rel, length(p), bottom_r, top_r, cp) * dt;
    }
    return exp(-depth * cp.extinction * 0.6);
}
