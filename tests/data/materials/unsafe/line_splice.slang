// Unsafe material (a backslash line splice outside a string).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
static const float k = 1.0; \

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
