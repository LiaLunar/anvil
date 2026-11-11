# PROTOTYPE (eu4-engine-lab#154): throwaway, on prototype/script-profiler only.
cmake_minimum_required(VERSION 3.25)
project(prototype_script_profiler LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(prototype_script_profiler SOURCES src/plugin.cpp)
target_compile_definitions(prototype_script_profiler PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(prototype_script_profiler PRIVATE shell32 ole32)
