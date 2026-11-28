# Refusal probe for the sdk_config_refusals test (ROADMAP P8): asks for
# PROBE_VERSION, so the test can check
# that a wrong version and a mismatched compiler fail at configure time with a
# clear message instead of at link time.
cmake_minimum_required(VERSION 3.22)
project(quat3d_probe LANGUAGES CXX)
if(NOT PROBE_VERSION)
    message(FATAL_ERROR "-DPROBE_VERSION=<major.minor> is required")
endif()
find_package(quat3d ${PROBE_VERSION} CONFIG REQUIRED)
message(STATUS "quat3d ${quat3d_VERSION} accepted")
