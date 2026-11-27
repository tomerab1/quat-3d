// Unsafe material (a quote in a character literal opening a fake string).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
static const int q = '"'; import "/etc/passwd";

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
