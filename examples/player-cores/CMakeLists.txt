cmake_minimum_required(VERSION 3.25)
project(player_cores LANGUAGES CXX)

# A plugin without game addresses: it uses only the services of the standard
# plugins anvil-game (anvil-game.world) and anvil-map (anvil-map.modes).
# Within the Anvil tree, the package is already loaded; elsewhere see
# examples/hello-cpp for FetchContent or find_package.
if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(player_cores SOURCES player_cores.cpp)
