// Unsafe material (a pointer-typed helper parameter, issue #110).
// expect-line: 6
// The registry must refuse it before compiling it.
import material;

float read(float4 *p) { return p.x; }

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
