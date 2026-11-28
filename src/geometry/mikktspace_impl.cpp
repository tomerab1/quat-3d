// The vendored MikkTSpace implementation (Mikkelsen 2008), compiled as C++
// inside engine_geometry so it follows the kernel's floating-point rules (no
// contraction; see the root CMakeLists.txt). Its directory is a SYSTEM
// include path, so MSVC's /external:W0 covers the vendored code's warnings
// as it does for other third-party headers; -w does on clang / GCC.
#include <mikktspace.c>
