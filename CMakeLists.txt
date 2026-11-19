cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(PunchOutWiiQilinComp VERSION 0.13.6 LANGUAGES C CXX)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit hosts only (see docs/roadmap-and-design-targets.md)")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)

# Floating point is bit-exact with the guest: the host compiler must not fuse
# a multiply and an add on its own (Broadway's fmadd family is fused, and is
# written as std::fma; nothing else is).
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# ---- The DOL loader: runtime code too (no std::format) ----
add_library(qilin_dol STATIC recomp/src/dol.cpp recomp/src/sha256.cpp)
target_include_directories(qilin_dol PUBLIC recomp/src)
target_link_libraries(qilin_dol PRIVATE qilin_warnings)

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/dol_json.cpp
    recomp/src/decoder.cpp
    recomp/src/cfg.cpp
    recomp/src/lift.cpp
    recomp/src/idle.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src)
target_link_libraries(qilin_core PUBLIC qilin_dol PRIVATE qilin_warnings)
qilin_tool(qilin_core)

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

# ---- Runtime (what players run: no std::format) ----
add_library(qilin_rt STATIC runtime/src/memory.cpp runtime/src/system.cpp)
target_include_directories(qilin_rt PUBLIC runtime/include)
target_link_libraries(qilin_rt PRIVATE qilin_warnings)

enable_testing()

# ---- The host platform: window, input and audio (SDL3), Vulkan ----
# Fetched at configure time, pinned, built statically: SDL3, the Vulkan headers
# and volk (which loads Vulkan at run time, so no SDK is needed to build or run).
option(QILIN_HOST "Build the host platform layer (window, Vulkan, input, audio)" ON)
if(QILIN_HOST)
    include(FetchContent)
    set(FETCHCONTENT_QUIET ON)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)  # synthetic input: not needed
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG release-3.4.20 GIT_SHALLOW ON)
    FetchContent_Declare(VulkanHeaders GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
                         GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON)
    # volk is one C file: fetched without its own CMake project (SOURCE_SUBDIR points nowhere).
    FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON
                         SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(SDL3 VulkanHeaders volk)
    add_library(qilin_volk STATIC ${volk_SOURCE_DIR}/volk.c)
    target_include_directories(qilin_volk PUBLIC ${volk_SOURCE_DIR})
    target_link_libraries(qilin_volk PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
    target_compile_definitions(qilin_volk PUBLIC VK_NO_PROTOTYPES)

    add_library(qilin_host STATIC host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp)
    target_include_directories(qilin_host PUBLIC host/include)
    target_link_libraries(qilin_host PUBLIC qilin_volk PRIVATE SDL3::SDL3-static qilin_warnings)
    # Vulkan structures are written {sType} with the rest zeroed: that's the idiom, not a mistake.
    # (On the sources, so it comes after -Wextra on the command line.)
    if(NOT MSVC)
        set_source_files_properties(host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp host/tests/vktest.cpp
                                    PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()

    # The Vulkan layer's own test: a device, an offscreen frame, read back. Skips
    # (exit 77) where there's no Vulkan 1.3 device, as on CI runners.
    add_executable(qilin_vktest host/tests/vktest.cpp)
    target_link_libraries(qilin_vktest PRIVATE qilin_host qilin_warnings)
    add_test(NAME qilin_vktest COMMAND qilin_vktest)
    set_tests_properties(qilin_vktest PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ---- Tests (no game files needed) ----
add_executable(qilin_tests recomp/tests/tests.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_rt qilin_warnings)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)

# The thread model (the game's scheduler and interrupt layer) with guest
# functions written in C++: no game files needed.
add_executable(punchout_threads_test punchout/tests/threads_test.cpp punchout/src/threads.cpp punchout/src/os.cpp punchout/src/restart.cpp)
target_include_directories(punchout_threads_test PRIVATE punchout/src)
target_link_libraries(punchout_threads_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(punchout_threads_test)
add_test(NAME punchout_threads_test COMMAND punchout_threads_test)
set_tests_properties(punchout_threads_test PROPERTIES TIMEOUT 30)
add_test(NAME punchout_threads_test_shutdown COMMAND punchout_threads_test)
set_tests_properties(punchout_threads_test_shutdown PROPERTIES TIMEOUT 30 ENVIRONMENT THREADS_TEST_LEAVE=shutdown
                     PASS_REGULAR_EXPRESSION "the game chose to power off: closing")
add_test(NAME punchout_threads_test_restart COMMAND punchout_threads_test)
set_tests_properties(punchout_threads_test_restart PROPERTIES TIMEOUT 30 ENVIRONMENT THREADS_TEST_LEAVE=restart
                     PASS_REGULAR_EXPRESSION "OSRestart started the program again(.|\n)*all passed")

# The settings file's reader and writer.
add_executable(punchout_settings_test punchout/tests/settings_test.cpp punchout/src/settings.cpp)
target_include_directories(punchout_settings_test PRIVATE punchout/src host/include)
target_link_libraries(punchout_settings_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(punchout_settings_test)
add_test(NAME punchout_settings_test COMMAND punchout_settings_test)

# The lifter's differential test: random encodings of every instruction in the
# slice, lifted at build time, run against an independent reference interpreter.
add_executable(qilin_difftest_gen recomp/tests/difftest/gen.cpp)
target_link_libraries(qilin_difftest_gen PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_difftest_gen)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp
    COMMAND qilin_difftest_gen ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp
    DEPENDS qilin_difftest_gen
    COMMENT "Generating the lifter's differential test cases")
add_executable(qilin_difftest
    recomp/tests/difftest/run.cpp
    recomp/tests/difftest/ref.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp)
target_include_directories(qilin_difftest PRIVATE recomp/tests/difftest)
target_link_libraries(qilin_difftest PRIVATE qilin_core qilin_rt qilin_warnings)
qilin_tool(qilin_difftest)
# Generated code: the lifter's output isn't held to our warning set (unused labels, locals).
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
add_test(NAME qilin_difftest COMMAND qilin_difftest)

# ---- The game: built only from the player's own main.dol ----
# cmake -DQILIN_GAME_DOL=<extracted disc>/sys/main.dol: qilin lift turns it
# into C++ at build time (into the build tree, never the source tree), with the
# library plan in data/R7PP01, and the punchout executable is built from that.
set(QILIN_GAME_DOL "" CACHE FILEPATH "Punch-Out!! (PAL, Rev 1) main.dol: builds the punchout executable")
if(QILIN_GAME_DOL)
    if(NOT QILIN_HOST)
        message(FATAL_ERROR "the game needs the host platform layer: configure with QILIN_HOST=ON")
    endif()
    set(QILIN_LIFT_FILES 32)
    set(gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set(gen_sources ${gen}/table.cpp ${gen}/replaced.cpp)
    math(EXPR last "${QILIN_LIFT_FILES} - 1")
    foreach(i RANGE ${last})
        string(LENGTH "${i}" len)
        if(len EQUAL 1)
            set(i "00${i}")
        elseif(len EQUAL 2)
            set(i "0${i}")
        endif()
        list(APPEND gen_sources ${gen}/lifted_${i}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${gen}/functions.hpp ${gen_sources}
        COMMAND qilin lift ${QILIN_GAME_DOL} -o ${gen} --plan ${CMAKE_CURRENT_SOURCE_DIR}/data/R7PP01/plan.tsv
                --files ${QILIN_LIFT_FILES}
        DEPENDS qilin ${QILIN_GAME_DOL} ${CMAKE_CURRENT_SOURCE_DIR}/data/R7PP01/plan.tsv
        COMMENT "Lifting the game's main.dol to C++")
    add_executable(punchout punchout/src/main.cpp punchout/src/boot.cpp punchout/src/os.cpp punchout/src/dvd.cpp punchout/src/vi.cpp punchout/src/threads.cpp punchout/src/sc.cpp punchout/src/gpu.cpp punchout/src/video.cpp punchout/src/wpad.cpp punchout/src/nand.cpp punchout/src/hw.cpp punchout/src/dsp.cpp punchout/src/ax.cpp punchout/src/recorder.cpp punchout/src/texture.cpp punchout/src/renderer.cpp punchout/src/settings.cpp punchout/src/restart.cpp ${gen_sources})
    target_include_directories(punchout PRIVATE punchout/src ${gen})
    target_link_libraries(punchout PRIVATE qilin_rt qilin_dol qilin_host)
    # Our own sources get the warning set; the lifter's output doesn't (unused labels and locals).
    set_source_files_properties(punchout/src/main.cpp punchout/src/boot.cpp punchout/src/os.cpp punchout/src/dvd.cpp punchout/src/vi.cpp punchout/src/threads.cpp punchout/src/sc.cpp punchout/src/gpu.cpp punchout/src/video.cpp punchout/src/wpad.cpp punchout/src/nand.cpp punchout/src/hw.cpp punchout/src/dsp.cpp punchout/src/ax.cpp punchout/src/recorder.cpp punchout/src/texture.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W4;/permissive-,-Wall;-Wextra;-Wpedantic;-Wshadow>")
    # The renderer writes Vulkan structures {sType} with the rest zeroed, as the host layer does.
    set_source_files_properties(punchout/src/renderer.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W4;/permissive-,-Wall;-Wextra;-Wpedantic;-Wshadow;-Wno-missing-field-initializers>")
    if(MSVC)
        set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "/w;/bigobj")
        # Lifted code calls nest as deeply as the guest's: a large host stack.
        target_link_options(punchout PRIVATE /STACK:16777216)
    else()
        set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endif()
