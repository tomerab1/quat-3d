// Unsafe material (a string import of an absolute path).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
import "/etc/hostname";

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
