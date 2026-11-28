// Unsafe material (dereferencing a pointer, issue #110).
// expect-line: 8
// The registry must refuse it before compiling it.
import material;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        return *input.standard_ptr;
    }
}

export struct SurfaceMaterial : IMaterial = M;
