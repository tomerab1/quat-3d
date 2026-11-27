// Single translation unit that emits the stb_image_write implementation (PNG
// encoding for render.screenshot). Compiled permissively (COMPILE_OPTIONS
// "-w") because stb is not warning-clean under the engine's strict flags.
// Images are encoded to memory only (stbi_write_png_to_func), so stdio is
// disabled.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
