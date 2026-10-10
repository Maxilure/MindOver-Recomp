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
    src/input_record.cpp         # every controller state the game reads -> <log>-inputs.txt; replay
    src/debug_console.cpp        # --debug_console: a socket that answers questions about the running game
    src/game_folder.cpp          # saves / settings / controls inside the game folder (user/)
    src/settings_list.cpp        # --list_settings: every setting written out for the launcher
    src/crash_report.cpp         # a crash writes its named call stack into the log + crash-*.txt
    src/debug/host_symbols.cpp   # host code address -> function name / original PowerPC instruction
    src/debug/guest_stack.cpp    # the game's call stack (PowerPC back chain), read fault-safely
    src/debug/write_watchpoints.cpp  # debug console `who`: which code writes a memory range
    src/debug/xbox_calls.cpp     # the Xbox calls meter: which Xbox services the game calls (Linux)
    src/session_log.cpp          # every log: version/system header, event logs on, old logs cleaned
    src/frame_rate.cpp           # --fps_cap=60: lifts the game's 30 fps pacing; --debug_log_fps lows
    src/fixed_step.cpp           # --fixed_step=N / console `clock`: every frame exactly 1/N s (exact replays)
    src/audio_trace.cpp          # --debug_audio_trace: logs every sound the game asks for, by name
    src/ground_physics.cpp       # above 30 fps: keeps characters on bumpy ground; --debug_ground_trace
    src/png_writer.cpp           # tiny dependency-free PNG writer (F10 photos)
    src/overlay_banner.cpp       # the short on-screen message when F9 / F8 switch pictures
    src/fps_overlay.cpp          # our own frame-rate counter over the picture, game font (Options -> Display)
    src/input/bindings.cpp       # keyboard + mouse: the actions, their keys, controls.toml
    src/input/keyboard_mouse.cpp # keyboard + mouse -> a virtual 360 controller (Spin = stick circles)
    src/input/players.cpp        # which device plays as which player (local co-op)
    src/input/controls_menu.cpp  # F6: the Controls menu (rebind keys, players)
    src/saves/pc_save_drive.cpp  # the saves as plain PC files: Radical's save drive, natively
    src/saves/save_files.cpp     # the save files on disk: find, rename, set aside ("delete")
    src/saves/save_library.cpp   # any number of saves in one scrolling list, rename / delete
    src/saves/rename_screen.cpp  # X on a save: a rename screen of its own in the front end (menus + in game)
    src/saves/quick_load.cpp     # --load_save: start straight into a save (no intros, title, menus)
    src/players/more_players.cpp  # room for 4 players in the co-op tables, --local_players (findings/26)
    src/players/more_players_hud.cpp  # players 3-4's HUD in the bottom corners (findings/26)
    src/players/more_players_frontend.cpp  # players 3-4's combo meters, lock-on arrows, counter prompts, reticles
    src/players/more_players_loading.cpp  # players 3-4's paw prints on the loading screen (findings/26)
    src/players/more_players_markers.cpp  # players 3-4's own marker pictures (findings/26)
    src/cheats/cheats.cpp        # F5 cheats for testing: level ups, mojo, god mode, speed, freeze, HUD
    src/cheats/spawn.cpp         # the cheat menu's Spawn section: titans (knocked out or not), enemies, pickups
    src/cheats/free_camera.cpp   # the cheat menu's free camera (fly the view with keys + mouse)
    src/cheats/cheat_menu.cpp    # F5: the Cheats menu
    src/players/coop_camera.cpp  # the co-op camera: frames every player (camera rework)
    src/players/menu_input.cpp  # which player's controller a menu listens to (findings/26)
    src/players/who_plays.cpp    # no profiles: who plays = which controllers play (the game's sign-in, PC version)
    src/players/lost_controller.cpp  # a player's controller is gone: pause + message anyone answers (findings/26)
    src/data/data_patcher.cpp    # changed copies of game data files, built from the player's disc at run time
    src/ui/scrooby.cpp           # helpers to drive the game's own menu library (find / set texts, menus)
    src/ui/game_font.cpp         # the game's own font read from the disc data, for text we draw ourselves
    src/options/options_page.cpp      # the Options screen's page, rebuilt in the in-game menus package
    src/options/options_settings.cpp  # what the Options screen offers: tabs, rows, live changes, saving
    src/options/options_menu.cpp      # Pause -> Options: tabs, rows, values (hooks on the game's menu driver)
    src/data/fight_tree.cpp      # fight trees (.bfig): new nodes, C++ decisions for new front end states
    src/pddi/intercept.cpp       # wraps every call into the game's renderer (PDDI) + D3D
    src/pddi/trace.cpp           # --debug_pddi_trace_dir: log every renderer call of chosen frames
    src/native/native_renderer.cpp  # our Vulkan renderer (F9: emulated <-> native picture)
    src/native/recorder.cpp         # shadow mode: turns the game's renderer calls into a frame list
    src/native/texture_cache.cpp    # the game's textures -> Vulkan images (untile, byte swap)
    src/native/write_watch.cpp      # page write tracking: textures re-hashed only when written
    src/native/buffer_cache.cpp     # the game's vertex/index buffers -> Vulkan buffers (uploaded once)
    src/native/render_targets.cpp   # the game's render targets (surfaces) + resolves -> Vulkan images
    src/native/ab_capture.cpp       # F10 photos + debug: the same frame from both renderers
    src/native/native_window.cpp    # F8 / --native_window: a 2nd window with the native picture
    src/native/spotter.cpp          # loud log banners when a hunted material (bump) shows up
    src/native/scan.cpp             # with --trace: the first time a gap shows up, record the frame
)

if(WIN32)
    add_executable(crash_mom WIN32 ${CRASHMOM_SOURCES})
else()
    add_executable(crash_mom ${CRASHMOM_SOURCES})
endif()

# Links the generated code + runtime, adds the codegen dependency.
rexglue_setup_target(crash_mom)

# -----------------------------------------------------------------------------
# The Xbox calls meter (src/debug/xbox_calls.h), Linux only
# -----------------------------------------------------------------------------
# Every Xbox service the game imports is a C function __imp__<Name> in the
# SDK's runtime library, called directly by the generated code. The linker's
# --wrap=<symbol> sends those calls to __wrap_<symbol> instead (the original
# stays reachable as __real_<symbol>). Here we read the import names from the
# generated function table (lines "{ 0x824BB9BC, __imp__XamShowSigninUI },"),
# write one counting wrapper per name into <build>/xbox_calls_wraps.cpp and
# add one --wrap per name. Configure re-runs when the table changes (a new
# codegen). Windows' linker has no --wrap: no meter there.
set(_xbox_table ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/crash_mom_init.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS ${_xbox_table})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_xbox_table})
    file(STRINGS ${_xbox_table} _xbox_lines REGEX "__imp__[A-Za-z0-9_]+ }")
    set(_xbox_names)
    foreach(_line ${_xbox_lines})
        string(REGEX MATCH "__imp__[A-Za-z0-9_]+" _name "${_line}")
        list(APPEND _xbox_names ${_name})
    endforeach()
    list(REMOVE_DUPLICATES _xbox_names)
    list(SORT _xbox_names)
    set(_wraps "// GENERATED by CMakeLists.txt (the Xbox calls meter, src/debug/xbox_calls.h). Do not edit.\n")
    string(APPEND _wraps "#include <cstdint>\n#include <rex/ppc/context.h>\n#include \"debug/xbox_calls.h\"\n")
    set(_table "static const char* const kNames[] = {\n")
    set(_index 0)
    foreach(_name ${_xbox_names})
        string(REPLACE "__imp__" "" _short ${_name})
        string(APPEND _wraps
            "extern \"C\" void __real_${_name}(PPCContext&, uint8_t*);\n"
            "extern \"C\" void __wrap_${_name}(PPCContext& ctx, uint8_t* base) {\n"
            "  xbox_calls::Hit(${_index}, uint32_t(ctx.lr));\n"
            "  __real_${_name}(ctx, base);\n}\n")
        string(APPEND _table "  \"${_short}\",\n")
        target_link_options(crash_mom PRIVATE "LINKER:--wrap=${_name}")
        math(EXPR _index "${_index} + 1")
    endforeach()
    string(APPEND _table "};\n")
    string(APPEND _wraps "${_table}"
        "static const bool kRegistered = (xbox_calls::SetNames(kNames, ${_index}), true);\n")
    # file(CONFIGURE)-style "only when changed": keeps the build from relinking for nothing.
    set(_wraps_file ${CMAKE_CURRENT_BINARY_DIR}/xbox_calls_wraps.cpp)
    set(_old "")
    if(EXISTS ${_wraps_file})
        file(READ ${_wraps_file} _old)
    endif()
    if(NOT _old STREQUAL _wraps)
        file(WRITE ${_wraps_file} "${_wraps}")
    endif()
    target_sources(crash_mom PRIVATE ${_wraps_file})
    message(STATUS "Xbox calls meter: ${_index} imports wrapped")
endif()

# The source tree's path, for game_folder.cpp: a build run from inside the
# source tree keeps the player's files in <repo>/user/ (gitignored), so wiping
# out/ never deletes saves. Installed builds use <folder>/program -> <folder>.
target_compile_definitions(crash_mom PRIVATE CRASHMOM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# The port's version (VERSION.txt, e.g. 0.1.0-alpha) for the first line of
# every log (session_log.cpp). Re-read at the next build when the file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION.txt)
file(READ VERSION.txt CRASHMOM_VERSION)
string(STRIP "${CRASHMOM_VERSION}" CRASHMOM_VERSION)
target_compile_definitions(crash_mom PRIVATE CRASHMOM_VERSION="${CRASHMOM_VERSION}")

# -----------------------------------------------------------------------------
# Shaders of the native renderer (src/native/shaders/*.vert / *.frag)
# -----------------------------------------------------------------------------
# GLSL is compiled to SPIR-V at build time by glslc (from shaderc; on Linux
# the "shaderc" package, on Windows it ships with the Vulkan SDK). Each
# shader becomes <build>/shaders/<name>.h holding "{0x07230203,...}", which
# native_renderer.cpp #includes into a uint32_t array.
find_program(GLSLC_EXECUTABLE glslc REQUIRED)
set(CRASHMOM_SHADERS
    src/native/shaders/simple.vert       # the game's "simple" material (2D screens, most of the world)
    src/native/shaders/simple.frag
    src/native/shaders/bink.vert         # the game's movie material (Bink video frames)
    src/native/shaders/bink.frag
    src/native/shaders/lit.vert          # the "simple" material's lit variant (lights, no vertex colours)
    src/native/shaders/lit.frag
    src/native/shaders/character.vert    # the character material (Crash and co.: normal map, rim light)
    src/native/shaders/character.frag
    src/native/shaders/shadow.vert       # the soft shadow under characters (a blurred stencil copy)
    src/native/shaders/shadow.frag
    src/native/shaders/reflect.vert      # reflective surfaces: lit + a sphere-mapped reflection
    src/native/shaders/reflect.frag
    src/native/shaders/particle.vert     # sprites that fade out where they meet the scene
    src/native/shaders/particle.frag
    src/native/shaders/water.vert        # water: see-through, wobbly, reflecting
    src/native/shaders/water.frag
    src/native/shaders/dof.vert          # depth of field: the distance blurred, one full-screen pass
    src/native/shaders/dof.frag
    src/native/shaders/fxgrid.frag      # screen-wide glows / tints (double mojo flash); simple.vert
    src/native/shaders/bumpmega.vert    # normal-mapped, see-through surfaces (TK blocks, ice)
    src/native/shaders/bumpmega.frag
    src/native/shaders/depth_copy.vert   # one sample of a multisampled depth/stencil surface -> copies
    src/native/shaders/depth_copy.frag
    src/native/shaders/present.vert      # our frame -> the window's picture
    src/native/shaders/present.frag
)
# Included by the shaders above (#include): a change must recompile them.
set(CRASHMOM_SHADER_INCLUDES
    src/native/shaders/simple_params.glsl
    src/native/shaders/lit_params.glsl
    src/native/shaders/particle_params.glsl
    src/native/shaders/water_params.glsl
    src/native/shaders/dof_params.glsl
    src/native/shaders/lighting.glsl
    src/native/shaders/palette.glsl
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
        DEPENDS ${_shader} ${CRASHMOM_SHADER_INCLUDES}
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
# stb_image (PNG reader) for players 3-4's marker pictures (src/players/more_players_markers.cpp).
target_include_directories(crash_mom PRIVATE thirdparty/rexglue-sdk/thirdparty/stb)

# Copy ReXGlue's shared libraries next to the executable (Linux and Windows).
# The SDK already sets the executable's library search path to its own
# folder ($ORIGIN), and it copies the LINKED ones on Windows and macOS, but
# nothing on Linux. Without it, launching fails with:
#   error while loading shared libraries: librexruntimerd.so: cannot open ...
# On Windows the SDK's own copy misses the GPU plugin (loaded at run time,
# not linked): without rexgpu-xenos*.dll next to the exe the game can't draw.
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
if(NOT APPLE)
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

# The port's own assets (assets/: players 3-4's marker pictures), copied next
# to the exe on every build (assets/ is read from there at run time).
add_custom_target(crash_mom_stage_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:crash_mom>/assets
    VERBATIM)
add_dependencies(crash_mom_stage_assets crash_mom)  # the exe's folder exists by then

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
