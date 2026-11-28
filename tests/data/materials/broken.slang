// Test material with a compile error on line 9, column 33 (`input.uvv`).

import material;

struct Broken : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        // The next line misspells a MaterialInput field.
        s.albedo = float3(input.uvv, 0.0);
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = Broken;
