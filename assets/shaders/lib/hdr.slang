// hdr.slang — writing radiance to the RGBA16F HDR targets.
//
// Half floats top out at 65504; a larger value written to an RGBA16F image
// rounds to +Inf on some drivers (CI's lavapipe) and saturates on others.
// An Inf there becomes NaN downstream (bloom's Karis weighting, TAA blends,
// Inf - Inf) and spreads as a black block (issue #45). Every pass that writes
// radiance into an HDR target clamps it with hdr_store. NaN inputs stay NaN:
// they are bugs to fix at their source, not to hide here.

// Largest finite half float.
public static const float HDR_MAX = 65504.0;

// `c` clamped to the HDR target's finite range.
public float3 hdr_store(float3 c) {
    return min(c, float3(HDR_MAX, HDR_MAX, HDR_MAX));
}
