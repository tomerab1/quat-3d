// Unsafe material (a string escape before a newline cannot hide the next line).
// expect-line: 7
// The registry must refuse it before writing it to disk.
import material;
static const float k = 1.0; // "
static const int s = 0; "a\
#include "/etc/hostname"

struct M : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) { return input.standard; }
}

export struct SurfaceMaterial : IMaterial = M;
