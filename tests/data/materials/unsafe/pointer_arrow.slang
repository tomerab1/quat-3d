// Unsafe material (member access through a pointer, issue #110).
// expect-line: 9
// The registry must refuse it before compiling it.
import material;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        s.roughness = input.params_ptr->roughness_factor;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = M;
