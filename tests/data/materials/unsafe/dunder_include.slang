// Unsafe material (the __include declaration).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
__include "/etc/hostname";

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
