// Unsafe material (taking an address, issue #110).
// expect-line: 9
// The registry must refuse it before compiling it.
import material;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        let p = &s;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = M;
