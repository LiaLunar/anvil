cmake_minimum_required(VERSION 3.25)
project(hello_script LANGUAGES CXX)

# A plugin repository gets the package from the SDK archive of a release (see
# examples/hello-cpp). Within the Anvil tree, the package is already loaded.
if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(hello_script SOURCES hello.cpp)
