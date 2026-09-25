# =============================================================================
# Crash: Mind over Mutant -- native PC port
# =============================================================================
#
# What gets built into the `crash_mom` executable:
#
#   generated/default/*.cpp  the game's PowerPC code, translated to C++ by
#                            `rexglue codegen` (see crash_mom_manifest.toml)
#   ReXGlue runtime          Xbox 360 kernel, memory, file system, GPU,
#                            audio and input (found via find_package, see
#                            docs/01-building.md for how the SDK is installed)
#   src/*                    OUR code: app setup, hooks, fixes, enhancements
#
# All the SDK plumbing (finding the SDK, the codegen step, compile flags
# for the generated code) lives in generated/rexglue.cmake. That file is
# owned by ReXGlue: regenerate it with `rexglue migrate`, don't edit it.
# =============================================================================

cmake_minimum_required(VERSION 3.25)
project(crash_mom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Our own source files. Add new hook / fix / feature files here.
set(CRASHMOM_SOURCES
    src/main.cpp          # entry point: hands control to ReXGlue's app framework
    src/debug_frame_capture.cpp  # --debug_capture_dir: periodic screenshots for debugging
    src/debug_input_script.cpp   # --debug_input_script: scripted button presses for test runs
    src/frame_rate.cpp           # --fps_cap=60: lifts the game's 30 fps pacing (midasm hooks)
    src/pddi/intercept.cpp       # wraps every call into the game's renderer (PDDI) + D3D
    src/pddi/trace.cpp           # --debug_pddi_trace_dir: log every renderer call of chosen frames
    src/native/native_renderer.cpp  # our Vulkan renderer (F9: emulated <-> native picture)
)

if(WIN32)
    add_executable(crash_mom WIN32 ${CRASHMOM_SOURCES})
else()
    add_executable(crash_mom ${CRASHMOM_SOURCES})
endif()

# Links the generated code + runtime, adds the codegen dependency.
rexglue_setup_target(crash_mom)

# -----------------------------------------------------------------------------
# Shaders of the native renderer (src/native/shaders/*.vert / *.frag)
# -----------------------------------------------------------------------------
# GLSL is compiled to SPIR-V at build time by glslc (from shaderc; on Linux
# the "shaderc" package, on Windows it ships with the Vulkan SDK). Each
# shader becomes <build>/shaders/<name>.h holding "{0x07230203,...}", which
# native_renderer.cpp #includes into a uint32_t array.
find_program(GLSLC_EXECUTABLE glslc REQUIRED)
set(CRASHMOM_SHADERS
    src/native/shaders/test_scene.vert   # milestone 2 test picture
    src/native/shaders/test_scene.frag
    src/native/shaders/present.vert      # our frame -> the window's picture
    src/native/shaders/present.frag
)
set(_shader_headers)
foreach(_shader ${CRASHMOM_SHADERS})
    get_filename_component(_name ${_shader} NAME)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/shaders/${_name}.h)
    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.0 -O -mfmt=c
                -o ${_out} ${CMAKE_CURRENT_SOURCE_DIR}/${_shader}
        DEPENDS ${_shader}
        COMMENT "glslc ${_name}"
        VERBATIM)
    list(APPEND _shader_headers ${_out})
endforeach()
# Listing the headers as sources makes the build generate them first.
target_sources(crash_mom PRIVATE ${_shader_headers})
target_include_directories(crash_mom PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shaders)

# The SDK's <rex/ui/vulkan/presenter.h> (used by the native renderer) includes
# RenderDoc's API header, which the SDK doesn't install: take it from the SDK
# source tree, like xexdis does for the disassembler headers.
target_include_directories(crash_mom PRIVATE thirdparty/rexglue-sdk/thirdparty/renderdoc)

# Copy ReXGlue's shared libraries next to the executable on Linux.
# The SDK already sets the executable's library search path to its own
# folder ($ORIGIN), and it does this copy on Windows and macOS, but not on
# Linux. Without it, launching fails with:
#   error while loading shared libraries: librexruntimerd.so: cannot open ...
#   rex::runtime      the fake Xbox 360: kernel, memory, VFS, audio, input
#   rex::gpu-xenos    Xenos GPU emulation (command processor + shader translation)
#   rex::TracyClient  Tracy profiler client
#
# This is a separate always-run target, NOT a POST_BUILD step of crash_mom:
# a POST_BUILD step only runs when crash_mom relinks, and nothing relinks it
# when only the GPU plugin changes (it's loaded at runtime via dlopen, not
# linked). We found this out when an SDK GPU change silently didn't reach
# the game: the staged librexgpu-xenosrd.so was hours older than the
# installed one. copy_if_different makes the every-build copy cheap.
if(UNIX AND NOT APPLE)
    set(_stage_cmds)
    foreach(_lib rex::runtime rex::gpu-xenos rex::TracyClient)
        if(TARGET ${_lib})
            list(APPEND _stage_cmds
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_lib}> $<TARGET_FILE_DIR:crash_mom>)
        endif()
    endforeach()
    add_custom_target(crash_mom_stage_libs ALL ${_stage_cmds} VERBATIM)
    add_dependencies(crash_mom_stage_libs crash_mom)  # the exe's folder exists by then
endif()

# -----------------------------------------------------------------------------
# Developer tools (not part of the game)
# -----------------------------------------------------------------------------

# xexdis: disassemble any address range of default.xex (see the file header).
# Build:  cmake --build --preset linux-amd64-relwithdebinfo --target xexdis
# The binutils disassembler headers aren't installed by the SDK, so we take
# them from the SDK source tree (the compiled code is in rex::disasm).
add_executable(xexdis EXCLUDE_FROM_ALL tools/xexdis/xexdis.cpp)
target_include_directories(xexdis PRIVATE thirdparty/rexglue-sdk/thirdparty/disasm)
target_link_libraries(xexdis PRIVATE rex::runtime rex::disasm)
