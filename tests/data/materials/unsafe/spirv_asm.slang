// Unsafe material (inline SPIR-V: bypasses the compiler's checks, issue #110).
// expect-line: 8
// The registry must refuse it before compiling it.
import material;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        float x = spirv_asm { OpUndef $$float; };
        SurfaceSample s = input.standard;
        s.roughness = x;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = M;
