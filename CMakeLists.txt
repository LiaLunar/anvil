cmake_minimum_required(VERSION 3.25)
# version.txt is the only place the version is written; release-please bumps it.
file(STRINGS version.txt ANVIL_VERSION_TEXT LIMIT_COUNT 1 REGEX "^[0-9]+\\.[0-9]+\\.[0-9]+$")
if(NOT ANVIL_VERSION_TEXT)
    message(FATAL_ERROR "version.txt must hold a MAJOR.MINOR.PATCH version")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS version.txt)
project(anvil VERSION ${ANVIL_VERSION_TEXT} LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Anvil targets 64-bit Windows only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: version.dll must not depend on a redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ANVIL_BUILD_TESTS "Build unit tests" ON)
option(ANVIL_BUILD_EXAMPLES "Build the example plugins" ON)
option(ANVIL_BUILD_BENCH "Build the microbenchmarks (anvil_bench, not a ctest)" ON)

include(FetchContent)
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG v0.7.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tomlplusplus safetyhook)

# anvil_messages: the player's messages by code (why a plugin is skipped),
# their English words and the player's language (issue #158). anvil_i18n adds
# the catalogs and the lookup; only the overlay and the Manager link it, so
# version.dll carries no translations.
add_library(anvil_messages STATIC src/i18n/message.cpp)
target_include_directories(anvil_messages PUBLIC src)
target_compile_definitions(anvil_messages PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_library(anvil_i18n STATIC src/i18n/translate.cpp src/i18n/cjk_font.cpp src/i18n/catalog_fr.cpp
    src/i18n/catalog_de.cpp src/i18n/catalog_es.cpp src/i18n/catalog_zh_hans.cpp src/i18n/catalog_zh_hant.cpp
    src/i18n/catalog_ja.cpp src/i18n/catalog_ko.cpp)
# The CJK fonts are resources of each binary that draws text (cjk_font.hpp):
# target_sources(<target> PRIVATE ${ANVIL_CJK_FONTS_RC}).
set(ANVIL_CJK_FONTS_RC ${CMAKE_CURRENT_SOURCE_DIR}/src/i18n/fonts/fonts.rc CACHE INTERNAL "")
target_link_libraries(anvil_i18n PUBLIC anvil_messages)
if(MSVC)
    target_compile_options(anvil_messages PRIVATE /W4 /WX /utf-8 /permissive- /EHsc)
    target_compile_options(anvil_i18n PRIVATE /W4 /WX /utf-8 /permissive- /EHsc)
endif()

# anvil_core: the rules shared by version.dll and anvil.exe (manifests, SemVer,
# the resolver, the game build identity, symbols.toml and its verification
# against a PE image, settings.toml and the player's values, instruction
# boundaries (Zydis, from SafetyHook's fetch), the log). Nothing here may
# depend on running inside the game process, so the CLI applies the same rules
# as the loader.
add_library(anvil_core STATIC
    src/core/anvil_toml_edit.cpp
    src/core/crash_history.cpp
    src/core/discovery.cpp
    src/core/files.cpp
    src/core/game_build.cpp
    src/core/instructions.cpp
    src/core/log.cpp
    src/core/manifest.cpp
    src/core/mod_plugins.cpp
    src/core/pe_image.cpp
    src/core/playset.cpp
    src/core/plugin_hash.cpp
    src/core/resolver.cpp
    src/core/semver.cpp
    src/core/settings.cpp
    src/core/symbols.cpp
    src/core/trust.cpp)
target_include_directories(anvil_core PUBLIC include src)
target_link_libraries(anvil_core PUBLIC anvil_messages tomlplusplus::tomlplusplus Zydis bcrypt)
target_compile_definitions(anvil_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    ANVIL_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(anvil_core PUBLIC /W4 /utf-8 /permissive- /EHsc)
endif()

# anvil_loader: the parts that act on the running game (startup, the change
# registry, the plugin API 2 host table, symbols.toml checks against the
# running image and the verified symbols per plugin, services, SEH guards,
# crash capture and safe mode).
# Linked into version.dll and the tests, never into anvil.exe.
add_library(anvil_loader STATIC
    src/core/benchmark.cpp
    src/core/benchmark_record.cpp
    src/core/changes.cpp
    src/core/control.cpp
    src/core/crash.cpp
    src/core/crash_report.cpp
    src/core/first_opens.cpp
    src/core/gameplay_policy.cpp
    src/core/host.cpp
    src/core/plugin_settings.cpp
    src/core/plugin_symbols.cpp
    src/core/runtime.cpp
    src/core/safe_mode.cpp
    src/core/seh_guard.cpp
    src/core/services.cpp
    src/core/suspension.cpp
    src/core/symbol_check.cpp)
target_link_libraries(anvil_loader PUBLIC anvil_core safetyhook::safetyhook)

# version.dll = proxy + loader
# File version shown in the DLL's Properties, from the same version.txt.
configure_file(src/proxy/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/anvil_version.rc @ONLY)
add_library(version_dll SHARED src/anvil_main.cpp src/proxy/version_proxy.cpp src/proxy/version.def
    ${CMAKE_CURRENT_BINARY_DIR}/anvil_version.rc)
target_link_libraries(version_dll PRIVATE anvil_loader)
set_target_properties(version_dll PROPERTIES OUTPUT_NAME version)

# anvil.exe = the command-line tool for plugin authors (design §16). Its
# commands live in anvil_tool so the tests can run them in-process.
add_library(anvil_tool STATIC src/cli/check.cpp src/cli/game.cpp src/cli/gen.cpp)
target_link_libraries(anvil_tool PUBLIC anvil_core)
add_executable(anvil src/cli/anvil_cli.cpp)
target_link_libraries(anvil PRIVATE anvil_tool)

# `anvil gen <symbols> <dir>` at build time; <out_var> receives the headers.
function(anvil_gen_headers out_var symbols dir)
    add_custom_command(
        OUTPUT ${dir}/symbols.gen.h ${dir}/symbols.gen.hpp
        COMMAND anvil gen ${symbols} ${dir}
        DEPENDS anvil ${symbols}
        COMMENT "anvil gen ${symbols}")
    set(${out_var} ${dir}/symbols.gen.h ${dir}/symbols.gen.hpp PARENT_SCOPE)
endfunction()

# `anvil gen <settings> <dir>` at build time: settings.gen.h and settings.gen.hpp.
function(anvil_gen_settings_headers out_var settings dir)
    add_custom_command(
        OUTPUT ${dir}/settings.gen.h ${dir}/settings.gen.hpp
        COMMAND anvil gen --settings ${settings} ${dir}
        DEPENDS anvil ${settings}
        COMMENT "anvil gen ${settings}")
    set(${out_var} ${dir}/settings.gen.h ${dir}/settings.gen.hpp PARENT_SCOPE)
endfunction()

# The tree builds its examples the way plugin repositories do: through the
# AnvilPlugin package of the SDK (sdk/cmake), pointed at this tree's headers and
# `anvil` target. The folder <build>/plugin/<id>/ holds each plugin as Anvil
# loads it (one folder for every configuration).
set(ANVIL_SDK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(ANVIL_SDK_TOOL anvil)
set(ANVIL_SDK_VERSION ${PROJECT_VERSION})
include(sdk/cmake/AnvilPluginConfig.cmake)

# A test plugin of this tree: like anvil_add_plugin, but with a symbols.toml
# shared by several plugins (tests/plugins/symbols.toml), so it is copied
# rather than read from the plugin folder. Warning free.
function(anvil_add_tree_plugin target)
    cmake_parse_arguments(ARG "" "ID;DIR;OUTPUT_NAME;SYMBOLS" "SOURCES" ${ARGN})
    set(out ${CMAKE_BINARY_DIR}/plugin/${ARG_ID})
    anvil_gen_headers(headers ${ARG_SYMBOLS} ${CMAKE_CURRENT_BINARY_DIR}/gen/${ARG_ID})
    add_library(${target} SHARED ${ARG_SOURCES} ${headers})
    target_include_directories(${target} PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/gen/${ARG_ID})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME} RUNTIME_OUTPUT_DIRECTORY ${out}$<0:>)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /permissive-)
    endif()
    configure_file(${ARG_DIR}/plugin.toml ${out}/plugin.toml COPYONLY)
    configure_file(${ARG_SYMBOLS} ${out}/symbols.toml COPYONLY)
endfunction()

# The hello plugins in C and C++: plain plugin repositories (CMakeLists.txt,
# plugin.toml, symbols.toml, sources) that find the package already loaded.
if(ANVIL_BUILD_EXAMPLES)
    set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
    add_subdirectory(examples/hello-c)
    add_subdirectory(examples/hello-cpp)
    unset(CMAKE_COMPILE_WARNING_AS_ERROR)
endif()

# anvil-game, the standard plugin of design §4 and §13.1 (readiness, frames,
# main-thread work, sessions), built like any plugin repository and released
# with Anvil in anvil/plugins/anvil-game/.
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
add_subdirectory(plugins/anvil-game)
# anvil-overlay, the standard plugin of design §15 (notifications and the
# panel on Direct3D 9, through the core's anvil.control), released next to it.
add_subdirectory(plugins/anvil-overlay)
# anvil-log-throttle, the standard plugin that counts the repeats of a line of
# the game's logs instead of writing them (anvil#172), released next to them.
add_subdirectory(plugins/anvil-log-throttle)
unset(CMAKE_COMPILE_WARNING_AS_ERROR)
# The standard plugins ship only inside the package, so their version is
# Anvil's: release-please bumps their manifests with version.txt.
foreach(_standard anvil-game anvil-overlay anvil-log-throttle)
    set(_manifest plugins/${_standard}/plugin.toml)
    file(STRINGS ${_manifest} _manifest_version LIMIT_COUNT 1 REGEX "^version = \"[^\"]*\"$")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_manifest})
    if(NOT _manifest_version STREQUAL "version = \"${PROJECT_VERSION}\"")
        message(FATAL_ERROR "${_manifest} must have version = \"${PROJECT_VERSION}\" (version.txt), found '${_manifest_version}'")
    endif()
endforeach()

# AnvilManager.exe, the player's installer and management tool (design §15).
add_subdirectory(tools/manager)
# The resource compiler does not track the files fonts.rc names.
file(GLOB ANVIL_CJK_FONT_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/i18n/fonts/*.otf)
set_source_files_properties(${ANVIL_CJK_FONTS_RC} DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} plugins/anvil-overlay tools/manager
    PROPERTIES OBJECT_DEPENDS "${ANVIL_CJK_FONT_FILES}")

# anvil_bench: microbenchmarks per kind of change and of the start-up parsers
# (design §17). CI runs it as its own step against bench/baseline-ci.json; the
# lab runs it pinned through bench/run-lab.ps1. Not part of ctest.
# anvil_bench --memory measures the memory budgets; it reads version.dll's
# headers next to it and copies dist/anvil.toml for the core start-up.
if(ANVIL_BUILD_BENCH)
    add_executable(anvil_bench bench/anvil_bench.cpp bench/memory.cpp)
    target_link_libraries(anvil_bench PRIVATE anvil_loader)
    target_compile_definitions(anvil_bench PRIVATE ANVIL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(anvil_bench version_dll)
endif()

if(ANVIL_BUILD_TESTS)
    enable_testing()
    add_executable(anvil_tests tests/test_main.cpp tests/manifest_tests.cpp tests/resolver_tests.cpp
        tests/change_tests.cpp tests/gameplay_tests.cpp tests/settings_tests.cpp tests/control_tests.cpp
        tests/benchmark_tests.cpp)
    target_link_libraries(anvil_tests PRIVATE anvil_loader)
    target_compile_definitions(anvil_tests PRIVATE ANVIL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(anvil_tests version_dll)
    add_test(NAME anvil_tests COMMAND anvil_tests $<TARGET_FILE:version_dll>)
    # Crash capture and safe mode; crashes for real in child processes of itself.
    # version.dll: the stack guarantee its DllMain gives new threads.
    add_executable(anvil_crash_tests tests/crash_tests.cpp)
    target_link_libraries(anvil_crash_tests PRIVATE anvil_loader)
    add_dependencies(anvil_crash_tests version_dll)
    add_test(NAME anvil_crash_tests COMMAND anvil_crash_tests $<TARGET_FILE:version_dll>)
    add_executable(anvil_symbols_tests tests/test_symbols.cpp)
    target_link_libraries(anvil_symbols_tests PRIVATE anvil_loader)
    add_test(NAME anvil_symbols_tests COMMAND anvil_symbols_tests)

    add_executable(anvil_game_tests tests/game_tests.cpp)
    target_link_libraries(anvil_game_tests PRIVATE anvil_tool)
    add_dependencies(anvil_game_tests anvil)
    add_test(NAME anvil_game_tests COMMAND anvil_game_tests $<TARGET_FILE:anvil>)

    # Plugins in mods (design §14, #60): the playset, the folder hash,
    # trust.toml, the decisions and approved copies, on made-up playsets.
    add_executable(anvil_workshop_tests tests/workshop_tests.cpp)
    target_link_libraries(anvil_workshop_tests PRIVATE anvil_core)
    target_compile_definitions(anvil_workshop_tests PRIVATE ANVIL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME anvil_workshop_tests COMMAND anvil_workshop_tests)

    # The loader's start-up with plugins in mods, in child processes of the
    # test (tests/startup_tests.cpp), with mod-probe: a plugin without symbols.
    set(ANVIL_MOD_PROBE_DIR ${CMAKE_BINARY_DIR}/test-plugins/mod-probe)
    add_library(anvil_mod_probe SHARED tests/plugins/mod-probe/mod_probe.c)
    target_include_directories(anvil_mod_probe PRIVATE include)
    set_target_properties(anvil_mod_probe PROPERTIES OUTPUT_NAME mod-probe
        RUNTIME_OUTPUT_DIRECTORY ${ANVIL_MOD_PROBE_DIR}$<0:> PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins$<0:>)
    if(MSVC)
        target_compile_options(anvil_mod_probe PRIVATE /W4 /WX)
    endif()
    configure_file(tests/plugins/mod-probe/plugin.toml ${ANVIL_MOD_PROBE_DIR}/plugin.toml COPYONLY)
    add_executable(anvil_startup_tests tests/startup_tests.cpp)
    target_link_libraries(anvil_startup_tests PRIVATE anvil_loader)
    add_dependencies(anvil_startup_tests anvil_mod_probe)
    add_test(NAME anvil_startup_tests COMMAND anvil_startup_tests ${ANVIL_MOD_PROBE_DIR})

    # The Manager's core: zips, Steam's files, install plans, plugin zips, the
    # start-up benchmark (anvil_loader: the core's own start record writer).
    add_executable(anvil_manager_tests tests/manager_tests.cpp tests/manager_benchmark_tests.cpp tests/manager_report_tests.cpp)
    target_link_libraries(anvil_manager_tests PRIVATE anvil_manager_core anvil_loader)
    target_compile_definitions(anvil_manager_tests PRIVATE ANVIL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(anvil_manager_tests version_dll)
    add_test(NAME anvil_manager_tests COMMAND anvil_manager_tests $<TARGET_FILE:version_dll>)

    # AnvilManager.exe carries the package of this build and reads it back.
    add_test(NAME anvil_manager_check_package COMMAND anvil_manager --check-package)

    add_executable(anvil_cli_tests tests/test_cli.cpp)
    target_link_libraries(anvil_cli_tests PRIVATE anvil_tool)
    add_dependencies(anvil_cli_tests anvil)
    add_test(NAME anvil_cli_tests COMMAND anvil_cli_tests $<TARGET_FILE:anvil>)

    # The SDK from Anvil's sources in a plugin project with C++ only (#81):
    # configuring must not need the C language.
    set(sdk_cxx_only_args -G "${CMAKE_GENERATOR}")
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND sdk_cxx_only_args -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    add_test(NAME anvil_sdk_cxx_only
        COMMAND ${CMAKE_COMMAND} ${sdk_cxx_only_args} -S ${CMAKE_CURRENT_SOURCE_DIR}/tests/sdk/cxx-only
            -B ${CMAKE_CURRENT_BINARY_DIR}/sdk-cxx-only -DANVIL_ROOT=${CMAKE_CURRENT_SOURCE_DIR})

    # anvil gen on the design §5.2 example and on a settings.toml with every
    # type: the headers must compile, warning free, as C11 and C++23 and
    # declare what the fixtures say.
    set(ANVIL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen_compile)
    add_custom_command(
        OUTPUT ${ANVIL_GEN_DIR}/symbols.gen.h ${ANVIL_GEN_DIR}/symbols.gen.hpp
        COMMAND anvil gen ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gen/symbols.toml ${ANVIL_GEN_DIR}
        DEPENDS anvil ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gen/symbols.toml
        COMMENT "anvil gen tests/fixtures/gen/symbols.toml")
    anvil_gen_settings_headers(gen_settings_headers ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gen/settings.toml
        ${ANVIL_GEN_DIR})
    add_executable(anvil_gen_compile tests/gen_compile.c tests/gen_compile.cpp
        ${ANVIL_GEN_DIR}/symbols.gen.h ${ANVIL_GEN_DIR}/symbols.gen.hpp ${gen_settings_headers})
    target_include_directories(anvil_gen_compile PRIVATE include ${ANVIL_GEN_DIR})
    set_target_properties(anvil_gen_compile PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(anvil_gen_compile PRIVATE /W4 /WX /utf-8)
    endif()
    add_test(NAME anvil_gen_compile COMMAND anvil_gen_compile)

    # Plugin API 2 end to end: the host table loads the hello examples and the
    # test plugins against a synthetic image (tests/plugin_tests.cpp).
    if(ANVIL_BUILD_EXAMPLES)
        set(ANVIL_TEST_PLUGINS ${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins)
        anvil_add_tree_plugin(anvil_probe_plugin ID probe DIR ${ANVIL_TEST_PLUGINS}/probe OUTPUT_NAME probe
            SYMBOLS ${ANVIL_TEST_PLUGINS}/symbols.toml SOURCES tests/plugins/probe/probe.cpp)
        anvil_add_tree_plugin(anvil_throwing_plugin ID throwing DIR ${ANVIL_TEST_PLUGINS}/throwing
            OUTPUT_NAME throwing SYMBOLS ${ANVIL_TEST_PLUGINS}/symbols.toml SOURCES tests/plugins/throwing/throwing.cpp)
        add_executable(anvil_plugin_tests tests/plugin_tests.cpp)
        target_link_libraries(anvil_plugin_tests PRIVATE anvil_loader)
        add_dependencies(anvil_plugin_tests anvil_probe_plugin anvil_throwing_plugin hello_c hello_cpp)
        add_test(NAME anvil_plugin_tests COMMAND anvil_plugin_tests ${CMAKE_BINARY_DIR}/plugin)
    endif()

    # anvil-game end to end on a synthetic image laid out like eu4.exe, with
    # the gameplay policy listening (tests/anvil_game_plugin_tests.cpp).
    add_executable(anvil_game_plugin_tests tests/anvil_game_plugin_tests.cpp plugins/anvil-game/src/session.cpp
        plugins/anvil-game/src/work_queue.cpp)
    target_link_libraries(anvil_game_plugin_tests PRIVATE anvil_loader)
    add_dependencies(anvil_game_plugin_tests anvil_game)
    add_test(NAME anvil_game_plugin_tests COMMAND anvil_game_plugin_tests ${CMAKE_BINARY_DIR}/plugin)

    # anvil-overlay's notices and key matching, and anvil check on its folder
    # (tests/overlay_tests.cpp).
    add_executable(anvil_overlay_tests tests/overlay_tests.cpp plugins/anvil-overlay/src/notices.cpp
        plugins/anvil-overlay/src/control_client.cpp)
    target_include_directories(anvil_overlay_tests PRIVATE src/control)
    target_link_libraries(anvil_overlay_tests PRIVATE anvil_loader anvil_i18n)
    add_test(NAME anvil_overlay_tests COMMAND anvil_overlay_tests)

    # The player's language (issue #158): messages by code, the language from
    # EU4's settings, and catalogs that translate every marked text of the
    # overlay's and the Manager's sources (tests/i18n_tests.cpp).
    add_executable(anvil_i18n_tests tests/i18n_tests.cpp ${ANVIL_CJK_FONTS_RC})
    target_link_libraries(anvil_i18n_tests PRIVATE anvil_core anvil_i18n)
    target_compile_definitions(anvil_i18n_tests PRIVATE ANVIL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # ImGui's stb_truetype reads the CJK fonts as the atlas does.
    FetchContent_GetProperties(imgui)
    target_include_directories(anvil_i18n_tests PRIVATE ${imgui_SOURCE_DIR})
    add_test(NAME anvil_i18n_tests COMMAND anvil_i18n_tests ${CMAKE_CURRENT_SOURCE_DIR})
    # The same on a real Direct3D 9 device with this test as the game image;
    # passes with a note where no device can be created.
    add_executable(anvil_overlay_d3d_tests tests/overlay_d3d_tests.cpp)
    target_include_directories(anvil_overlay_d3d_tests PRIVATE src/control)
    target_link_libraries(anvil_overlay_d3d_tests PRIVATE anvil_loader d3d9)
    add_dependencies(anvil_overlay_d3d_tests anvil_overlay)
    add_test(NAME anvil_overlay_d3d_tests COMMAND anvil_overlay_d3d_tests ${CMAKE_BINARY_DIR}/plugin)
    add_test(NAME anvil_overlay_check COMMAND anvil check ${CMAKE_CURRENT_SOURCE_DIR}/plugins/anvil-overlay)

    # anvil-log-throttle's throttling on its own, and the plugin end to end on
    # a synthetic image (tests/log_throttle_tests.cpp).
    add_executable(anvil_log_throttle_tests tests/log_throttle_tests.cpp plugins/anvil-log-throttle/src/throttle.cpp)
    target_link_libraries(anvil_log_throttle_tests PRIVATE anvil_loader)
    add_dependencies(anvil_log_throttle_tests anvil_log_throttle)
    add_test(NAME anvil_log_throttle_tests COMMAND anvil_log_throttle_tests ${CMAKE_BINARY_DIR}/plugin)

    # The symbols.lock of anvil-game and of every example matches its
    # symbols.toml (anvil check without --exe; the locks come from anvil check
    # --exe on the real eu4.exe).
    add_test(NAME anvil_game_symbols_lock COMMAND anvil check ${CMAKE_CURRENT_SOURCE_DIR}/plugins/anvil-game)
    add_test(NAME anvil_log_throttle_symbols_lock COMMAND anvil check ${CMAKE_CURRENT_SOURCE_DIR}/plugins/anvil-log-throttle)
    foreach(example hello-c hello-cpp hello-rust)
        add_test(NAME anvil_${example}_symbols_lock COMMAND anvil check ${CMAKE_CURRENT_SOURCE_DIR}/examples/${example})
    endforeach()

    # The wrapper's signature checks: each source must fail to compile with
    # the static_assert, and the compiler's note must name the symbol. The
    # targets are not part of ALL; each test builds one.
    anvil_gen_headers(wrong_headers ${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/symbols.toml
        ${CMAKE_CURRENT_BINARY_DIR}/gen/wrong-signature)
    foreach(wrong detour call)
        add_library(anvil_wrong_${wrong} STATIC EXCLUDE_FROM_ALL tests/plugins/wrong-signature/wrong_${wrong}.cpp
            ${wrong_headers})
        target_include_directories(anvil_wrong_${wrong} PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/gen/wrong-signature)
        add_test(NAME anvil_wrong_${wrong}
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target anvil_wrong_${wrong})
    endforeach()
    set_tests_properties(anvil_wrong_detour PROPERTIES PASS_REGULAR_EXPRESSION
        "the detour does not match the signature symbols.toml declares.*sym::Types::add_one")
    set_tests_properties(anvil_wrong_call PROPERTIES PASS_REGULAR_EXPRESSION
        "the arguments do not match the signature symbols.toml declares.*sym::Types::twice")

    add_test(NAME anvil_cli_version COMMAND anvil --version)
    set_tests_properties(anvil_cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^anvil ${PROJECT_VERSION}[\r\n]*$")
    add_test(NAME anvil_cli_help COMMAND anvil --help)
    set_tests_properties(anvil_cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: anvil <command>")
    # gen and check without their arguments, like unknown commands, exit non-zero (2).
    add_test(NAME anvil_cli_gen COMMAND anvil gen)
    add_test(NAME anvil_cli_check COMMAND anvil check)
    add_test(NAME anvil_cli_unknown COMMAND anvil frobnicate)
    set_tests_properties(anvil_cli_gen anvil_cli_check anvil_cli_unknown PROPERTIES WILL_FAIL TRUE)
endif()

# Release layout (cmake --install ... --component anvil): <prefix>/version.dll, <prefix>/anvil/...
# anvil.exe is not part of this package; it ships with the SDK archive (component sdk).
install(TARGETS version_dll RUNTIME DESTINATION . COMPONENT anvil)
install(FILES dist/anvil.toml DESTINATION anvil COMPONENT anvil)
install(FILES dist/plugins-README.txt DESTINATION anvil/plugins RENAME README.txt COMPONENT anvil)
install(FILES $<TARGET_FILE:anvil_game> plugins/anvil-game/plugin.toml plugins/anvil-game/symbols.toml
    DESTINATION anvil/plugins/anvil-game COMPONENT anvil)
install(FILES $<TARGET_FILE:anvil_overlay> plugins/anvil-overlay/plugin.toml plugins/anvil-overlay/settings.toml
    DESTINATION anvil/plugins/anvil-overlay COMPONENT anvil)
install(FILES $<TARGET_FILE:anvil_log_throttle> plugins/anvil-log-throttle/plugin.toml
    plugins/anvil-log-throttle/symbols.toml plugins/anvil-log-throttle/settings.toml
    DESTINATION anvil/plugins/anvil-log-throttle COMPONENT anvil)
install(FILES LICENSE README.md DESTINATION anvil/docs COMPONENT anvil)
# Third-party code compiled into version.dll: ship its license texts.
# Zydis is declared inside SafetyHook's scope; fetch its properties here.
FetchContent_GetProperties(zydis)
if(NOT zydis_SOURCE_DIR)
    message(FATAL_ERROR "Zydis sources not found (needed for its license text)")
endif()
install(FILES ${tomlplusplus_SOURCE_DIR}/LICENSE DESTINATION anvil/docs/third-party RENAME tomlplusplus-LICENSE.txt COMPONENT anvil)
install(FILES ${safetyhook_SOURCE_DIR}/LICENSE DESTINATION anvil/docs/third-party RENAME safetyhook-LICENSE.txt COMPONENT anvil)
install(FILES ${zydis_SOURCE_DIR}/LICENSE DESTINATION anvil/docs/third-party RENAME zydis-LICENSE.txt COMPONENT anvil)
install(FILES ${zydis_SOURCE_DIR}/dependencies/zycore/LICENSE DESTINATION anvil/docs/third-party RENAME zycore-LICENSE.txt COMPONENT anvil)
install(FILES ${ANVIL_OVERLAY_IMGUI_LICENSE} DESTINATION anvil/docs/third-party RENAME imgui-LICENSE.txt COMPONENT anvil)
# miniz is compiled into AnvilManager.exe, which ships next to version.dll.
install(FILES ${ANVIL_MINIZ_LICENSE} DESTINATION anvil/docs/third-party RENAME miniz-LICENSE.txt COMPONENT anvil)
# The CJK fonts are resources of anvil_overlay.dll and AnvilManager.exe (src/i18n/fonts).
install(FILES src/i18n/fonts/OFL.txt DESTINATION anvil/docs/third-party RENAME SourceHanSans-OFL.txt COMPONENT anvil)

# AnvilManager.exe (cmake --install ... --component manager): next to eu4.exe in
# the release zip, and a release asset of its own. Not in component anvil: the
# Manager carries that package (tools/manager).
install(TARGETS anvil_manager RUNTIME DESTINATION . COMPONENT manager)

# SDK archive (cmake --install ... --component sdk): what plugin authors fetch
# (design §16). FetchContent_MakeAvailable runs the CMakeLists.txt at its root;
# find_package(AnvilPlugin) reads cmake/. The version file lets
# find_package(AnvilPlugin 1.0) ask for a compatible release.
include(CMakePackageConfigHelpers)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/sdk/AnvilPluginConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
install(FILES include/anvil/anvil_plugin.h include/anvil/anvil.hpp include/anvil/anvil_symbols.hpp
    include/anvil/anvil_game.h include/anvil/anvil_benchmark.h include/anvil/anvil_benchmark.hpp
    DESTINATION include/anvil COMPONENT sdk)
install(TARGETS anvil RUNTIME DESTINATION bin COMPONENT sdk)
install(FILES sdk/cmake/AnvilPluginConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/sdk/AnvilPluginConfigVersion.cmake
    DESTINATION cmake COMPONENT sdk)
install(FILES sdk/CMakeLists.txt sdk/README.md version.txt LICENSE DESTINATION . COMPONENT sdk)
install(FILES ${tomlplusplus_SOURCE_DIR}/LICENSE DESTINATION third-party RENAME tomlplusplus-LICENSE.txt COMPONENT sdk)
install(FILES ${zydis_SOURCE_DIR}/LICENSE DESTINATION third-party RENAME zydis-LICENSE.txt COMPONENT sdk)
install(FILES ${zydis_SOURCE_DIR}/dependencies/zycore/LICENSE DESTINATION third-party RENAME zycore-LICENSE.txt COMPONENT sdk)
