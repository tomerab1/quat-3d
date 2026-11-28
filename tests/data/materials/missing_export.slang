// Test material that implements IMaterial but never exports SurfaceMaterial:
// the link fails with an unresolved external symbol.

import material;

struct Orphan : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        return input.standard;
    }
}
