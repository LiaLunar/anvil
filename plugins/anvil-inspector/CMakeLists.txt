# anvil-inspector: built through the AnvilPlugin package like any plugin
# repository; within the Anvil tree the package is already loaded. It has no
# game addresses: everything it shows comes from anvil-game.world.
cmake_minimum_required(VERSION 3.25)
project(anvil_inspector LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(anvil_inspector SOURCES src/plugin.cpp src/panel.cpp src/snapshot.cpp src/rows.cpp src/texts.cpp)
target_compile_definitions(anvil_inspector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
