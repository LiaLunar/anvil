# anvil-log-throttle: built through the AnvilPlugin package like any plugin
# repository; within the Anvil tree the package is already loaded.
cmake_minimum_required(VERSION 3.25)
project(anvil_log_throttle LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(anvil_log_throttle SOURCES src/plugin.cpp src/throttle.cpp)
