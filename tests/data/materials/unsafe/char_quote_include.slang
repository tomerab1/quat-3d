// Unsafe material (a fake string hiding __include).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
static const int q = '"'; __include "/etc/passwd";

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
