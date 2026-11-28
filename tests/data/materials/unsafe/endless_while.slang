// Unsafe material (a loop that never ends hangs the GPU, issue #110).
// expect-line: 9
// The registry must refuse it before compiling it.
import material;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        float x = 0.0;
        while ( true ) { x += 1.0; }
        return input.standard;
    }
}

export struct SurfaceMaterial : IMaterial = M;
