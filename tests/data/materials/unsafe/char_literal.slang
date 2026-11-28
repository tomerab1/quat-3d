// Unsafe material (a plain character literal).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
static const int c = 'a';

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
