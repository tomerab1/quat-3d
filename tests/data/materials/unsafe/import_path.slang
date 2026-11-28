// Unsafe material (a string import climbing out of the tree).
// expect-line: 5
// The registry must refuse it before writing it to disk.
import material;
import "../../lib/material.slang";

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
