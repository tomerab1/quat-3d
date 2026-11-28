// Unsafe material (a raw buffer-device-address load, issue #110).
// expect-line: 9
// The registry must refuse it before compiling it.
import material;

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        s.roughness = vk::RawBufferLoad<float>(uint64_t(4096));
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = M;
