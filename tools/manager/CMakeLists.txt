# AnvilManager.exe, the player's installer and management tool (design §15,
# issue #59). anvil_manager_core holds what it does without a window: finding
# the game, the install, repair and uninstall plans for the package it
# carries, plugin zips (checked with anvil check's own code, anvil_tool), the
# release check, the problem report and the start-up benchmark (its series,
# result, report, and the runner that starts the game) and the plugin
# catalog. The release check is anvil_net's, shared with anvil-overlay. The
# tests link it directly.

# miniz, pinned to a release tag and compiled here from its sources (its own
# CMakeLists.txt is not used): reads and, for the tests, writes zip archives.
include(FetchContent)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(miniz)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniz/miniz_export.h "#pragma once\n#define MINIZ_EXPORT\n")
add_library(anvil_miniz STATIC
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(anvil_miniz PUBLIC ${miniz_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/miniz)
# Archives are read from memory: no stdio, no time functions.
target_compile_definitions(anvil_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(anvil_miniz PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
set(ANVIL_MINIZ_LICENSE ${miniz_SOURCE_DIR}/LICENSE CACHE INTERNAL "")

add_library(anvil_manager_core STATIC
    src/benchmark_report.cpp
    src/benchmark_run.cpp
    src/benchmark_send.cpp
    src/benchmark_series.cpp
    src/benchmark_setup.cpp
    src/catalog.cpp
    src/game.cpp
    src/install.cpp
    src/json.cpp
    src/mods.cpp
    src/plugin_zip.cpp
    src/problem_report.cpp
    src/start_menu.cpp
    src/zip.cpp)
target_include_directories(anvil_manager_core PUBLIC src)
# wbemuuid, ole32, oleaut32: the antivirus names from Security Center (WMI).
# shell32, uuid: the Start menu shortcut.
target_link_libraries(anvil_manager_core PUBLIC anvil_tool anvil_i18n anvil_net PRIVATE anvil_miniz advapi32 wbemuuid ole32 oleaut32 shell32 uuid)
if(MSVC)
    target_compile_options(anvil_manager_core PRIVATE /W4 /WX)
endif()

# The package AnvilManager.exe carries: `cmake --install --component anvil` of
# this build into a staging folder, zipped, linked in as resource ANVIL_PACKAGE.
# The Manager therefore always installs the Anvil of its own version.
set(ANVIL_MANAGER_STAGE ${CMAKE_CURRENT_BINARY_DIR}/package)
set(ANVIL_MANAGER_PACKAGE ${CMAKE_CURRENT_BINARY_DIR}/anvil-package.zip)
file(GLOB ANVIL_STANDARD_TRANSLATIONS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/plugins/*/translations/*.toml)
add_custom_command(
    OUTPUT ${ANVIL_MANAGER_PACKAGE}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ANVIL_MANAGER_STAGE}
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --component anvil
        --prefix ${ANVIL_MANAGER_STAGE}
    COMMAND ${CMAKE_COMMAND} -E chdir ${ANVIL_MANAGER_STAGE}
        ${CMAKE_COMMAND} -E tar cf ${ANVIL_MANAGER_PACKAGE} --format=zip -- version.dll anvil
    DEPENDS version_dll anvil_game anvil_overlay anvil_log_throttle anvil_map anvil_script anvil_inspector ${PROJECT_SOURCE_DIR}/dist/anvil.toml
        ${PROJECT_SOURCE_DIR}/dist/plugins-README.txt ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/README.md ${ANVIL_STANDARD_TRANSLATIONS}
    COMMENT "Anvil package for AnvilManager.exe"
    VERBATIM)
add_custom_target(anvil_manager_package DEPENDS ${ANVIL_MANAGER_PACKAGE})

# Dear ImGui (the tag anvil-overlay pins) with the Win32 and Direct3D 11
# backends, for the Manager's window.
FetchContent_GetProperties(imgui)
if(NOT imgui_SOURCE_DIR)
    message(FATAL_ERROR "Dear ImGui sources not found (fetched by plugins/anvil-overlay)")
endif()
add_library(anvil_manager_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(anvil_manager_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(anvil_manager_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(anvil_manager_imgui PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# The plugin catalog of this build (ADR 0002), linked in as ANVIL_CATALOG.
set(ANVIL_MANAGER_CATALOG ${PROJECT_SOURCE_DIR}/catalog/plugins.json)
configure_file(app/manager.rc.in ${CMAKE_CURRENT_BINARY_DIR}/manager.rc @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/manager.rc PROPERTIES
    OBJECT_DEPENDS "${ANVIL_MANAGER_PACKAGE};${ANVIL_MANAGER_CATALOG}")
add_executable(anvil_manager WIN32
    app/benchmark_tab.cpp
    app/main.cpp
    app/manager_app.cpp
    app/platform.cpp
    app/AnvilManager.manifest
    ${CMAKE_CURRENT_BINARY_DIR}/manager.rc
    ${ANVIL_CJK_FONTS_RC})
add_dependencies(anvil_manager anvil_manager_package)
target_link_libraries(anvil_manager PRIVATE anvil_manager_core anvil_manager_imgui d3d11 dxgi winhttp shell32 ole32)
set_target_properties(anvil_manager PROPERTIES OUTPUT_NAME AnvilManager)
if(MSVC)
    target_compile_options(anvil_manager PRIVATE /W4 /WX)
    # The manifest in app/ says asInvoker; static imports come from System32
    # only, so the Manager never loads the game folder's version.dll.
    target_link_options(anvil_manager PRIVATE /MANIFESTUAC:NO /DEPENDENTLOADFLAG:0x800)
endif()
