// Single translation unit that emits the stb_image implementation. Compiled
// permissively (COMPILE_OPTIONS "-w") because stb is not warning-clean under the
// engine's strict flags. Everywhere else includes <stb_image.h> for declarations
// only.
//
// We decode exclusively from memory (glTF data URIs / buffer views / external
// files already loaded by fastgltf), so stdio is disabled.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include <stb_image.h>
