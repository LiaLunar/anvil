# anvil-overlay: built through the AnvilPlugin package like any plugin
# repository; within the Anvil tree the package is already loaded. It also
# reads the core's private anvil.control header (src/control), so it builds
# only inside the Anvil tree.
cmake_minimum_required(VERSION 3.25)
project(anvil_overlay LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

# Dear ImGui, pinned to a release tag and linked statically: only the core,
# the Win32 and the Direct3D 9 backends. No gamepad (XInput), no default IME
# or clipboard functions (the overlay brings its own, see src/ui.cpp).
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(anvil_overlay_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(anvil_overlay_imgui PUBLIC ${imgui_SOURCE_DIR})
target_compile_definitions(anvil_overlay_imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS
    IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(anvil_overlay_imgui PROPERTIES COMPILE_WARNING_AS_ERROR OFF POSITION_INDEPENDENT_CODE ON)
set(ANVIL_OVERLAY_IMGUI_LICENSE ${imgui_SOURCE_DIR}/LICENSE.txt CACHE INTERNAL "")

anvil_add_plugin(anvil_overlay SOURCES src/plugin.cpp src/ui.cpp src/notices.cpp src/control_client.cpp)
target_include_directories(anvil_overlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/control)
target_link_libraries(anvil_overlay PRIVATE anvil_overlay_imgui anvil_i18n)
target_compile_definitions(anvil_overlay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
