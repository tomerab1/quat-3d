// Unsafe material (a dotted import naming a sub-path).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
import material.secret;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
