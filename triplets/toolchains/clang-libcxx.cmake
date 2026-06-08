# vcpkg chainload toolchain — compile ports with clang + libc++ so their static
# libraries are ABI-compatible with the engine (which links libc++).
# Referenced from triplets/x64-linux-libcxx.cmake.
set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)

set(CMAKE_CXX_FLAGS_INIT           "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-stdlib=libc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++")
