# sdk_toolchain_rules (ROADMAP P8): the compiler-match rule quat3dConfig.cmake
# applies (cmake/Quat3dToolchainCheck.cmake), in script mode:
#   cmake -DCHECK=<path>/Quat3dToolchainCheck.cmake -P toolchain_check_test.cmake
cmake_policy(VERSION 3.22)
include("${CHECK}")

set(failures 0)
# expect(<accept|refuse> <regex or ""> sdk_id sdk_ver sdk_lib id ver lib)
function(expect verdict pattern)
    quat3d_toolchain_problem(problem ${ARGN})
    if(verdict STREQUAL "accept" AND NOT problem STREQUAL "")
        message(SEND_ERROR "expected ${ARGN} to be accepted, got: ${problem}")
    elseif(verdict STREQUAL "refuse" AND problem STREQUAL "")
        message(SEND_ERROR "expected ${ARGN} to be refused")
    elseif(verdict STREQUAL "refuse" AND NOT problem MATCHES "${pattern}")
        message(SEND_ERROR "refusal of ${ARGN} does not match '${pattern}': ${problem}")
    endif()
endfunction()

# Linux SDK: clang 18 + libc++.
expect(accept "" Clang 18.1.3 libc++ Clang 18.1.8 libc++)
expect(refuse "GNU 13.2.0.*Clang 18.1.3" Clang 18.1.3 libc++ GNU 13.2.0 libstdc++)
expect(refuse "major versions must match" Clang 18.1.3 libc++ Clang 19.1.0 libc++)
expect(refuse "against libstdc\\+\\+.*built against libc\\+\\+" Clang 18.1.3 libc++ Clang 18.1.3 libstdc++)
# macOS SDK: Apple clang is not LLVM clang.
expect(accept "" AppleClang 16.0.0.16000026 libc++ AppleClang 16.0.0.16000026 libc++)
expect(refuse "Clang 16.*AppleClang 16" AppleClang 16.0.0.16000026 libc++ Clang 16.0.0 libc++)
# Windows SDK: MSVC 19.44 (v143); newer v14x accepted, older or another major refused.
expect(accept "" MSVC 19.44.35211.0 msvc MSVC 19.44.35211.0 msvc)
expect(accept "" MSVC 19.44.35211.0 msvc MSVC 19.50.35700.0 msvc)
expect(refuse "needs MSVC 19.44 or newer" MSVC 19.44.35211.0 msvc MSVC 19.38.33130.0 msvc)
expect(refuse "uses Clang" MSVC 19.44.35211.0 msvc Clang 18.1.3 msvc)
