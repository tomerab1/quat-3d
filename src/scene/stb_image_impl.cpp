// Single translation unit that emits the stb_image implementation. Compiled
// permissively (COMPILE_OPTIONS "-w") because stb is not warning-clean under the
// engine's strict flags. Everywhere else includes <stb_image.h> for declarations
// only; engine code decodes through asset::decode_png_jpeg, which checks the
// header against the engine's texture limits first.
//
// We decode exclusively from memory (glTF data URIs / buffer views / external
// files already loaded by fastgltf, HDRI files), so stdio is disabled. Only
// PNG, JPEG and Radiance HDR are compiled in (less untrusted-input surface;
// HDR is reached only through asset::decode_radiance_hdr), and
// STBI_MAX_DIMENSIONS is a second guard behind asset::TextureLimits.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR
#define STBI_MAX_DIMENSIONS 16384
#include <stb_image.h>
