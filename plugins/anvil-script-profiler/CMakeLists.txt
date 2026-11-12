# anvil-script-profiler: built through the AnvilPlugin package like any plugin
# repository; within the Anvil tree the package is already loaded.
cmake_minimum_required(VERSION 3.25)
project(anvil_script_profiler LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(anvil_script_profiler SOURCES src/plugin.cpp src/recorder.cpp src/owners.cpp src/report.cpp
    src/texts.cpp)
target_compile_definitions(anvil_script_profiler PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(anvil_script_profiler PRIVATE shell32 ole32)
