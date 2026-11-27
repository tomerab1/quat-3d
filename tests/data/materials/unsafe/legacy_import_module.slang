// Unsafe material (the legacy __import, even of a library module).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
__import material;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
