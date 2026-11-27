// Test material that the source check must accept: the forbidden constructs
// below only appear inside comments, e.g. #include "/etc/hostname",
// __include "x" and import "../lib/material.slang".
/* A block comment may span lines:
#include "/etc/hostname"
import "../../x.slang";
*/

__exported import material;
import pbr;

struct TrickyButSafe : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        float importance = 0.5; // an identifier that merely starts with "import"
        float my__include = 0.25;
        s.roughness = importance + my__include;
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = TrickyButSafe;
