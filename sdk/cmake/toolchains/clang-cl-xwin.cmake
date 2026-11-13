# Cross-compiles for Windows x64 on Linux with clang-cl (no Visual Studio), for Anvil and its plugins:
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=<this file> -DXWIN_DIR=<folder made by `xwin splat --output <folder>`>
#
# Needs on PATH: clang-cl, lld-link, llvm-lib, llvm-rc, llvm-mt, and Wine (the build runs anvil.exe for
# `anvil gen`; set ANVIL_WINE to a wrapper script to choose the Wine prefix and options).
# XWIN_DIR may also come from the environment. See tests/wine.md for running the tests under Wine.

if(NOT XWIN_DIR AND DEFINED ENV{XWIN_DIR})
    set(XWIN_DIR "$ENV{XWIN_DIR}")
endif()
if(NOT XWIN_DIR OR NOT EXISTS "${XWIN_DIR}/crt/include")
    message(FATAL_ERROR "XWIN_DIR must name the output of `xwin splat` (it has crt/ and sdk/): '${XWIN_DIR}'")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# The wrapper adds /EHa only to files that use __try/__except: without it clang-cl drops MSVC-style SEH
# protection for bodies without calls (a guarded read of bad memory then crashes unhandled), and with it
# everywhere `catch (...)` would also swallow hardware faults, which MSVC's /EHsc never does.
set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/clang-cl-eha")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/clang-cl-eha")
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_inc "/imsvc ${XWIN_DIR}/crt/include /imsvc ${XWIN_DIR}/sdk/include/ucrt /imsvc ${XWIN_DIR}/sdk/include/um /imsvc ${XWIN_DIR}/sdk/include/shared /imsvc ${XWIN_DIR}/sdk/include/winrt")
# Newer clang reports a few things MSVC does not; the projects build with /WX, so these stay off:
# -Wmissing-field-initializers, -Wcharacter-conversion, and the unused /MP that CMake adds for MSVC.
set(_quiet "/clang:-Wno-missing-field-initializers /clang:-Wno-character-conversion -Wno-unused-command-line-argument")
set(CMAKE_C_FLAGS_INIT "${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_inc} ${_quiet}")
set(_lib "/libpath:${XWIN_DIR}/crt/lib/x86_64 /libpath:${XWIN_DIR}/sdk/lib/um/x86_64 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_RC_FLAGS_INIT "/I ${XWIN_DIR}/sdk/include/um /I ${XWIN_DIR}/sdk/include/shared")

# The project asks for the static CRT; xwin's default splat has no dynamic debug CRT.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if(DEFINED ENV{ANVIL_WINE})
    set(CMAKE_CROSSCOMPILING_EMULATOR "$ENV{ANVIL_WINE}")
else()
    find_program(_anvil_wine wine)
    if(_anvil_wine)
        set(CMAKE_CROSSCOMPILING_EMULATOR "${_anvil_wine}")
    endif()
endif()
