// Unsafe material (the legacy __import spelling).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
__import "/etc/passwd";

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
