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

// Samples the sky-view LUT for a world-space view direction (y up) and sun
// direction (pointing FROM the scene TOWARD the sun). rgb = sky luminance,
// a = cloud transmittance along the ray (1 = clear).
public float4 sample_skyview(Texture2D lut, SamplerState samp, float3 view_dir,
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
    return lut.SampleLevel(samp, uv, 0.0);
}
