// Unsafe material (a buffer device address cast to a pointer, issue #110).
// expect-line: 9
// The registry must refuse it before compiling it.
import material;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        uint64_t address = 0x1000;
        float value = *(float*)address;
        SurfaceSample s = input.standard;
        s.roughness = value;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = M;
