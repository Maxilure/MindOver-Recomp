# =============================================================================
# The launcher: a small program that starts the game (crash_mom_launcher)
# =============================================================================
#
# A project of its own, NOT part of the game's build: it holds none of the
# game's code (no generated/ files, no ReXGlue runtime), so it can be built
# once and handed out ready-made, while the game itself is always built on
# the player's own PC from their own disc.
#
# What it links:
#   SDL3   window, input, the 2D renderer that draws the UI, file/URL opening
#   ImGui  the UI itself (the same library as the game's F5/F6 menus)
# Both come from the SOURCE folders inside the ReXGlue SDK submodule
# (thirdparty/rexglue-sdk/thirdparty/...): no SDK build or install needed,
# just `git submodule update --init --recursive`. SDL3 is compiled in as a
# static library, so the result is one self-contained executable.
#
# Build (from the repo root; ~1 min the first time, SDL3 is the bulk):
#   cmake -S launcher -B out/build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build out/build/launcher
#   out/build/launcher/crash_mom_launcher
#
# How it finds the game: see folders.h.
# =============================================================================

cmake_minimum_required(VERSION 3.25)
project(crash_mom_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Windows: the C/C++ runtime built INTO the exe (static), so the launcher
# runs on a PC without Microsoft's runtime DLLs (testers may not have Visual
# Studio). Set before any target: SDL3 and ImGui get it too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SDK_THIRDPARTY ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rexglue-sdk/thirdparty)
if(NOT EXISTS ${SDK_THIRDPARTY}/sdl3/CMakeLists.txt)
    message(FATAL_ERROR
        "SDL3's source isn't there (${SDK_THIRDPARTY}/sdl3).\n"
        "Fetch the submodules first: git submodule update --init --recursive")
endif()

# --- SDL3, static, only what a small windowed tool needs ----------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# Not needed: no sound, no camera, no 3D API of our own (the SDL renderer
# picks Vulkan or OpenGL by itself).
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDK_THIRDPARTY}/sdl3 ${CMAKE_CURRENT_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)

# --- ImGui + its SDL3 backends -------------------------------------------------
set(IMGUI_DIR ${SDK_THIRDPARTY}/imgui)
add_library(launcher_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp          # SDL3 events -> ImGui
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp  # ImGui -> SDL's 2D renderer
)
target_include_directories(launcher_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(launcher_imgui PUBLIC SDL3::SDL3-static)

# --- the launcher ------------------------------------------------------------
add_executable(crash_mom_launcher WIN32   # WIN32: a window, no console (Windows)
    main.cpp          # the window and the Play page
    platform_posix.cpp  # Linux: processes, places, facts (platform.h)
    platform_win.cpp    # Windows: the same
    sha256.cpp        # SHA-256 of a download (updates), no outside tool needed
    folders.cpp       # where the game, its disc files and the player's files are
    saves.cpp         # the player's saves: names, progress, last played
    game_process.cpp  # starting the game, watching it, telling a crash from a quit
    settings.cpp      # the game's settings: the list (asked from the game) + user/settings.toml
    settings_page.cpp # the Settings tab
    desktop_entry.cpp # the launcher in the desktop's applications menu
    task_runner.cpp   # runs a setup step's commands one after another, with progress
    setup.cpp         # the Setup tab: build tools listed, disc, SDK, game builds
    update.cpp        # new versions: the feed, download, swap (release layout)
)
# toml++ (header only, in the SDK submodule): reads and writes settings.toml.
# SYSTEM: its headers' own warnings (old literal-operator spelling) aren't ours.
target_include_directories(crash_mom_launcher SYSTEM PRIVATE ${SDK_THIRDPARTY}/tomlplusplus/include)
target_link_libraries(crash_mom_launcher PRIVATE launcher_imgui)
if(WIN32)
    # ole32 / shell32 / uuid: Start menu shortcuts (desktop_entry.cpp).
    target_link_libraries(crash_mom_launcher PRIVATE ole32 shell32 uuid)
endif()
