# PROTOTYPE (eu4-engine-lab#11), throwaway: branch prototype/script-errors-panel, never merged.
cmake_minimum_required(VERSION 3.25)
project(prototype_script_errors LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(prototype_script_errors SOURCES src/plugin.cpp src/model.cpp src/rows.cpp)
target_compile_definitions(prototype_script_errors PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(prototype_script_errors_dump src/dump.cpp src/model.cpp src/rows.cpp)
target_compile_features(prototype_script_errors_dump PRIVATE cxx_std_20)
target_compile_options(prototype_script_errors_dump PRIVATE /utf-8)
target_compile_definitions(prototype_script_errors_dump PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(prototype_script_errors PRIVATE shell32 ole32)
