// Unsafe material (an import of a module that is not in the library).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
import hostname;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
