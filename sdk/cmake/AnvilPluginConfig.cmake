# The AnvilPlugin CMake package of the Anvil SDK (design §16).
#
#   include(FetchContent)
#   FetchContent_Declare(anvil_sdk URL <anvil-sdk-windows-x64.zip of a release> URL_HASH SHA256=<hash>)
#   FetchContent_MakeAvailable(anvil_sdk)         # or find_package(AnvilPlugin REQUIRED)
#   anvil_add_plugin(<target> SOURCES src/plugin.cpp ...)
#
# anvil_add_plugin reads plugin.toml and symbols.toml in the calling
# CMakeLists.txt's folder, runs `anvil gen`, builds the DLL with the static CRT
# and lays out
#   <build>/plugin/<id>/{plugin.toml, symbols.toml, settings.toml?, <entry>.dll}
# It adds the targets <target> (the DLL), <target>-check (`anvil check`, offline
# unless EU4_EXE is set) and, when EU4_DIR is set, <target>-deploy.
#
# The Anvil source tree includes this file too, with ANVIL_SDK_INCLUDE_DIR and
# ANVIL_SDK_TOOL set to its own headers and `anvil` target, so the examples
# build the way plugin repositories do.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "AnvilPlugin: Anvil plugins are Windows x64 DLLs")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "AnvilPlugin: Anvil plugins are 64-bit; configure with -A x64")
endif()

# The plugin API this SDK builds for (ANVIL_API_VERSION in anvil_plugin.h).
set(_anvil_plugin_api 2)

get_filename_component(_anvil_sdk_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED ANVIL_SDK_INCLUDE_DIR)
    set(ANVIL_SDK_INCLUDE_DIR "${_anvil_sdk_dir}/include")
endif()
if(NOT DEFINED ANVIL_SDK_TOOL)
    set(ANVIL_SDK_TOOL "${_anvil_sdk_dir}/bin/anvil.exe")
endif()
if(NOT DEFINED ANVIL_SDK_VERSION)
    set(ANVIL_SDK_VERSION "unknown")
    if(EXISTS "${_anvil_sdk_dir}/version.txt")
        file(STRINGS "${_anvil_sdk_dir}/version.txt" ANVIL_SDK_VERSION LIMIT_COUNT 1)
    endif()
endif()
if(NOT TARGET ${ANVIL_SDK_TOOL} AND NOT EXISTS "${ANVIL_SDK_TOOL}")
    message(FATAL_ERROR "AnvilPlugin: ${ANVIL_SDK_TOOL} is missing; the SDK archive is incomplete")
endif()
# FetchContent_MakeAvailable includes this file from the SDK's own directory
# scope, so anvil_add_plugin (global) reads its settings from global properties.
set_property(GLOBAL PROPERTY ANVIL_SDK_INCLUDE_DIR "${ANVIL_SDK_INCLUDE_DIR}")
set_property(GLOBAL PROPERTY ANVIL_SDK_TOOL "${ANVIL_SDK_TOOL}")
set_property(GLOBAL PROPERTY ANVIL_SDK_VERSION "${ANVIL_SDK_VERSION}")
set_property(GLOBAL PROPERTY ANVIL_SDK_PLUGIN_API "${_anvil_plugin_api}")

# The value of a top-level string key of plugin.toml (the text before the first
# [table]); empty when absent.
function(_anvil_manifest_value out text key)
    string(REGEX REPLACE "\n[ \t]*\\[.*$" "" top "\n${text}")
    if(top MATCHES "\n[ \t]*${key}[ \t]*=[ \t]*\"([^\"\n]*)\"")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

function(_anvil_manifest_number out text key)
    string(REGEX REPLACE "\n[ \t]*\\[.*$" "" top "\n${text}")
    if(top MATCHES "\n[ \t]*${key}[ \t]*=[ \t]*([0-9]+)")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# anvil_add_plugin(<target> SOURCES <files>...)
#
# Reads plugin.toml (id, entry, symbols, settings) in CMAKE_CURRENT_SOURCE_DIR.
# Optional: PLUGIN_DIR <dir> for a folder other than the current one.
function(anvil_add_plugin target)
    cmake_parse_arguments(ARG "" "PLUGIN_DIR" "SOURCES" ${ARGN})
    get_property(ANVIL_SDK_INCLUDE_DIR GLOBAL PROPERTY ANVIL_SDK_INCLUDE_DIR)
    get_property(ANVIL_SDK_TOOL GLOBAL PROPERTY ANVIL_SDK_TOOL)
    get_property(ANVIL_SDK_VERSION GLOBAL PROPERTY ANVIL_SDK_VERSION)
    get_property(ANVIL_SDK_PLUGIN_API GLOBAL PROPERTY ANVIL_SDK_PLUGIN_API)
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "anvil_add_plugin(${target}): unknown arguments ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "anvil_add_plugin(${target}): SOURCES is required")
    endif()
    if(ARG_PLUGIN_DIR)
        get_filename_component(plugin_dir "${ARG_PLUGIN_DIR}" ABSOLUTE)
    else()
        set(plugin_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(manifest_path "${plugin_dir}/plugin.toml")
    if(NOT EXISTS "${manifest_path}")
        message(FATAL_ERROR "anvil_add_plugin(${target}): ${manifest_path} does not exist")
    endif()
    # Reconfigure when the manifest changes: id and entry name the output.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest_path}")
    file(READ "${manifest_path}" manifest)
    _anvil_manifest_value(id "${manifest}" id)
    _anvil_manifest_value(entry "${manifest}" entry)
    _anvil_manifest_value(symbols "${manifest}" symbols)
    _anvil_manifest_value(settings "${manifest}" settings)
    _anvil_manifest_number(api "${manifest}" api)
    if(NOT id MATCHES "^[a-z0-9-]+$")
        message(FATAL_ERROR "anvil_add_plugin(${target}): plugin.toml needs an id of [a-z0-9-]+ (found '${id}')")
    endif()
    if(NOT entry MATCHES "^[^/\\\\:]+\\.dll$")
        message(FATAL_ERROR "anvil_add_plugin(${target}): plugin.toml needs an entry that is a .dll file name (found '${entry}')")
    endif()
    if(NOT api STREQUAL ANVIL_SDK_PLUGIN_API)
        message(FATAL_ERROR "anvil_add_plugin(${target}): plugin.toml says api = '${api}'; "
                "Anvil SDK ${ANVIL_SDK_VERSION} builds plugins for api = ${ANVIL_SDK_PLUGIN_API}")
    endif()
    string(REGEX REPLACE "\\.dll$" "" output_name "${entry}")

    set(out_dir "${CMAKE_BINARY_DIR}/plugin/${id}")
    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/anvil-gen/${target}")
    set(generated)
    set(copied "${manifest_path}")

    if(symbols)
        set(symbols_path "${plugin_dir}/${symbols}")
        if(NOT EXISTS "${symbols_path}")
            message(FATAL_ERROR "anvil_add_plugin(${target}): ${symbols_path} (plugin.toml `symbols`) does not exist")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${symbols_path}")
        add_custom_command(
            OUTPUT "${gen_dir}/symbols.gen.h" "${gen_dir}/symbols.gen.hpp"
            COMMAND ${ANVIL_SDK_TOOL} gen "${symbols_path}" "${gen_dir}"
            DEPENDS ${ANVIL_SDK_TOOL} "${symbols_path}"
            COMMENT "anvil gen ${symbols}"
            VERBATIM)
        list(APPEND generated "${gen_dir}/symbols.gen.h" "${gen_dir}/symbols.gen.hpp")
        list(APPEND copied "${symbols_path}")
    endif()
    if(settings)
        set(settings_path "${plugin_dir}/${settings}")
        if(NOT EXISTS "${settings_path}")
            message(FATAL_ERROR "anvil_add_plugin(${target}): ${settings_path} (plugin.toml `settings`) does not exist")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${settings_path}")
        list(APPEND copied "${settings_path}")
        # The typed accessors: settings.gen.h (CFG_<NAME>) and settings.gen.hpp (cfg::<name>).
        add_custom_command(
            OUTPUT "${gen_dir}/settings.gen.h" "${gen_dir}/settings.gen.hpp"
            COMMAND ${ANVIL_SDK_TOOL} gen --settings "${settings_path}" "${gen_dir}"
            DEPENDS ${ANVIL_SDK_TOOL} "${settings_path}"
            COMMENT "anvil gen ${settings}"
            VERBATIM)
        list(APPEND generated "${gen_dir}/settings.gen.h" "${gen_dir}/settings.gen.hpp")
    endif()

    add_library(${target} SHARED ${ARG_SOURCES} ${generated})
    target_include_directories(${target} PRIVATE "${ANVIL_SDK_INCLUDE_DIR}" "${gen_dir}")
    # C++20 is the wrapper's minimum (anvil.hpp); C11 for plain C sources.
    # Only for the languages the calling project enabled: the global list also
    # has those of Anvil's own project when the SDK comes from its sources.
    if(CMAKE_CXX_COMPILER_LOADED)
        target_compile_features(${target} PRIVATE cxx_std_20)
    endif()
    if(CMAKE_C_COMPILER_LOADED)
        target_compile_features(${target} PRIVATE c_std_11)
    endif()
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        # Static CRT: the DLL must not depend on a redistributable being installed.
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        # One folder for every configuration (the generator expression keeps
        # multi-config generators from appending $<CONFIG>).
        RUNTIME_OUTPUT_DIRECTORY "${out_dir}$<0:>")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    endif()
    # plugin.toml, symbols.toml and settings.toml next to the DLL, and the
    # plugin translations, translations/<code>.toml (design §5.4).
    file(GLOB translations CONFIGURE_DEPENDS "${plugin_dir}/translations/*.toml")
    set(copy_translations)
    if(translations)
        set(copy_translations
            COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}/translations"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${translations} "${out_dir}/translations")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${copied} "${out_dir}"
        ${copy_translations}
        COMMENT "plugin folder ${out_dir}"
        VERBATIM)

    # `anvil check`: the schemas and symbols.lock, or with EU4_EXE the symbols
    # against that eu4.exe (which rewrites symbols.lock).
    if(DEFINED EU4_EXE AND NOT EU4_EXE STREQUAL "")
        set(check_args --exe "${EU4_EXE}")
    else()
        set(check_args)
    endif()
    add_custom_target(${target}-check
        COMMAND ${ANVIL_SDK_TOOL} check "${plugin_dir}" ${check_args}
        DEPENDS ${ANVIL_SDK_TOOL}
        COMMENT "anvil check ${id}"
        VERBATIM)
    if(DEFINED EU4_DIR AND NOT EU4_DIR STREQUAL "")
        add_custom_target(${target}-deploy
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${out_dir}" "${EU4_DIR}/anvil/plugins/${id}"
            DEPENDS ${target}
            COMMENT "deploy ${id} to ${EU4_DIR}/anvil/plugins/${id}"
            VERBATIM)
    endif()
endfunction()
