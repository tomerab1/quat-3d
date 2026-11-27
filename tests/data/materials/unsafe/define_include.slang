// Unsafe material (a macro that could hide __include).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
#define GRAB __include

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
