# =============================================================================
# The subtitle editor: a small program of its own (subtitle_editor)
# =============================================================================
#
# Lists, plays and (step 2) times the subtitles of every voice line of the
# player's own disc, in every voice language (main.cpp; notes on the file
# format: src/subtitles/subtitles.h, docs/findings/63).
#
# A project of its own like the launcher, NOT part of the game's build: the
# SDK's SDL3 is built without SDL's 2D renderer, which this window draws
# with, so SDL3 is compiled here from the SDK submodule's source (as the
# launcher does). ImGui likewise. The one piece taken from the INSTALLED SDK
# is FFmpeg's XMA decoder (its static libavcodec / libavutil), used through
# the port's own decoder core src/audio/xma_stream.cpp, the same as the game.
#
# Build (from the repo root, after the SDK is installed; ~1 min the first time):
#   cmake -S tools/subtitle_editor -B out/build/subtitle_editor -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build out/build/subtitle_editor
#   out/build/subtitle_editor/subtitle_editor
# It finds the game folder by itself when started from inside it (or pass
# --game_folder=<path>).
# =============================================================================

cmake_minimum_required(VERSION 3.25)
project(subtitle_editor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# The SDK's FFmpeg libraries are built against the DLL C runtime (/MD): the
# editor links them, so it uses the same runtime (else lld-link: dllimport bsearch...).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SDK_THIRDPARTY ${REPO}/thirdparty/rexglue-sdk/thirdparty)
# The installed SDK (its static FFmpeg libraries). Default: the repo's own.
set(REXGLUE_INSTALL ${REPO}/thirdparty/rexglue-install CACHE PATH "The installed ReXGlue SDK")
if(NOT EXISTS ${SDK_THIRDPARTY}/sdl3/CMakeLists.txt)
    message(FATAL_ERROR "SDL3's source isn't there (${SDK_THIRDPARTY}/sdl3): "
                        "git submodule update --init --recursive")
endif()
find_library(SE_AVCODEC NAMES libavcodec liblibavcodec PATHS ${REXGLUE_INSTALL}/lib NO_DEFAULT_PATH)
find_library(SE_AVUTIL NAMES libavutil liblibavutil PATHS ${REXGLUE_INSTALL}/lib NO_DEFAULT_PATH)
if(NOT SE_AVCODEC OR NOT SE_AVUTIL)
    message(FATAL_ERROR "The SDK's FFmpeg libraries aren't in ${REXGLUE_INSTALL}/lib: "
                        "build and install the SDK first (docs/01-building.md), or pass "
                        "-DREXGLUE_INSTALL=<its install folder>")
endif()

# --- SDL3, static, with sound (the lines are played) ---------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDK_THIRDPARTY}/sdl3 ${CMAKE_CURRENT_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)

# --- ImGui + its SDL3 backends -------------------------------------------------
set(IMGUI_DIR ${SDK_THIRDPARTY}/imgui)
add_library(subtitle_editor_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp)  # InputText on std::string
target_include_directories(subtitle_editor_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(subtitle_editor_imgui PUBLIC SDL3::SDL3-static)

# --- FFmpeg's Bink decoders (the cartoons' sound and picture) -----------------
# Left out of the SDK's FFmpeg (its codec list has XMA only). Each file in
# ffmpeg/ compiles ONE of FFmpeg's own files unchanged after
# ffmpeg/config_overrides.h switches the Bink pieces on (and the CPU-specific
# paths off); everything else they call (RDFT, DCT, IDCT, VLC, buffers) is in
# the SDK's libavcodec. Compiled the way the SDK compiles FFmpeg.
set(FFMPEG_DIR ${SDK_THIRDPARTY}/FFmpeg)
add_library(subtitle_editor_bink STATIC
    ffmpeg/binkaudio.c   # Bink audio (RDFT / DCT): the dialogue tracks
    ffmpeg/bink.c        # Bink video: watching a cartoon with its subtitles
    ffmpeg/binkdsp.c
    ffmpeg/blockdsp.c
    ffmpeg/hpeldsp.c)
target_include_directories(subtitle_editor_bink PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg ${FFMPEG_DIR})
if(WIN32)
    target_include_directories(subtitle_editor_bink PRIVATE ${FFMPEG_DIR}/compat/atomics/win32)
else()
    target_include_directories(subtitle_editor_bink PRIVATE ${FFMPEG_DIR}/compat/atomics/gcc)
endif()
target_compile_definitions(subtitle_editor_bink PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
if(NOT MSVC)
    target_compile_options(subtitle_editor_bink PRIVATE -w)
endif()

# --- the editor ----------------------------------------------------------------
add_executable(subtitle_editor WIN32
    main.cpp            # the window: list, waveform, play
    voice_archive.cpp   # a voice archive's lines + decoding one
    subtitle_texts.cpp  # the subtitle files the game reads
    movie_archive.cpp   # the story cartoons (Bink) and their dialogue tracks
    exchange.cpp        # .srt / .wav to and from other subtitle programs
    icons.cpp           # the button symbols, drawn (no symbol font needed)
    ${REPO}/src/audio/xma_stream.cpp)  # the port's XMA decoder core
target_include_directories(subtitle_editor PRIVATE ${REPO}/src ${SDK_THIRDPARTY}/FFmpeg)
target_link_libraries(subtitle_editor PRIVATE subtitle_editor_imgui subtitle_editor_bink
    ${SE_AVCODEC} ${SE_AVUTIL})
if(WIN32)
    target_link_libraries(subtitle_editor PRIVATE bcrypt)  # FFmpeg's av_get_random_seed
else()
    target_link_libraries(subtitle_editor PRIVATE m)
endif()
