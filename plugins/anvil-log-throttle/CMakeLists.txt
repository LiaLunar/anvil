# anvil-log-throttle: built through the AnvilPlugin package like any plugin
# repository; within the Anvil tree the package is already loaded.
cmake_minimum_required(VERSION 3.25)
project(anvil_log_throttle LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(anvil_log_throttle SOURCES src/plugin.cpp src/throttle.cpp src/tally.cpp src/panel.cpp
    src/errors_model.cpp src/playset.cpp src/texts.cpp)
target_compile_definitions(anvil_log_throttle PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(anvil_log_throttle PRIVATE shell32 ole32)
