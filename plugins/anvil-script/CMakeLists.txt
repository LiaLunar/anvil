# anvil-script: built through the AnvilPlugin package like any plugin
# repository; within the Anvil tree the package is already loaded. It also
# reads the core's private anvil.control header (src/control), so it builds
# only inside the Anvil tree.
cmake_minimum_required(VERSION 3.25)
project(anvil_script LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(anvil_script SOURCES src/plugin.cpp src/registry.cpp)
target_include_directories(anvil_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/control)
