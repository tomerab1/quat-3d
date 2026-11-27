# ACL (Animation Compression Library, MIT) and its math library RTM (MIT).
# Both are header-only and not in vcpkg (vcpkg's "acl" port is POSIX access
# control lists), so they are fetched at configure time, pinned to the
# release commits: ACL v2.1.0 and the RTM v2.2.0 it was released against.
# Offline builds point FETCHCONTENT_SOURCE_DIR_ACL / FETCHCONTENT_SOURCE_DIR_RTM
# at local clones of those tags. Only the headers are used: SOURCE_SUBDIR
# names a directory without a CMakeLists.txt so neither project's own build
# (tests, tools, submodules) is added.
include(FetchContent)

FetchContent_Declare(acl
    GIT_REPOSITORY https://github.com/nfrechette/acl.git
    GIT_TAG        414689d5cff4286a7898487a46dc5e48005d38da # v2.1.0
    GIT_SUBMODULES ""
    SOURCE_SUBDIR  headers-only)
FetchContent_Declare(rtm
    GIT_REPOSITORY https://github.com/nfrechette/rtm.git
    GIT_TAG        d7982f2b2524feeb322f424b29cf43df30b5d5b7 # v2.2.0
    GIT_SUBMODULES ""
    SOURCE_SUBDIR  headers-only)
FetchContent_MakeAvailable(acl rtm)

# SYSTEM: third-party headers are exempt from the engine's warning flags.
add_library(acl_headers INTERFACE)
target_include_directories(acl_headers SYSTEM INTERFACE
    ${acl_SOURCE_DIR}/includes
    ${rtm_SOURCE_DIR}/includes)
