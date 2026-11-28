# Fixture: an engine target that links a sample target by name (no games/ or
# examples/ path anywhere), which only the link-graph walk can see.
cmake_minimum_required(VERSION 3.22)
project(link_layering_fixture LANGUAGES NONE)
include(${QUAT3D_CHECK})
add_library(engine_fixture INTERFACE)
add_library(sample_fixture_lib INTERFACE)
add_library(Third::party INTERFACE IMPORTED)
target_link_libraries(engine_fixture INTERFACE Third::party $<BUILD_INTERFACE:sample_fixture_lib>)
set_property(GLOBAL PROPERTY QUAT3D_LAYERING_ENGINE_TARGETS engine_fixture)
quat3d_check_link_layering()
