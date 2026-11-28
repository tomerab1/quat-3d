// Unsafe material (a pointer to a struct the module declares, issue #110).
// expect-line: 7
// The registry must refuse it before compiling it.
import material;

struct Node { float value; }
float read(Node* node) { return 1.0; }

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
