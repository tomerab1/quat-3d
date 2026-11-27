// octahedral.slang — decoding the GBuffer's octahedral world normal
// (gbuffer.slang writes it with oct_encode into an RG16F target).

public float3 oct_decode(float2 e) {
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
