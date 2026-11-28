// Unsafe material (the Ptr<T> pointer type, issue #110).
// expect-line: 6
// The registry must refuse it before compiling it.
import material;

float read(Ptr<float> p) { return 1.0; }

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
