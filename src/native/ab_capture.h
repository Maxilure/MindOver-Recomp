// =============================================================================
// native/ab_capture.h -- the same game frame, drawn by both renderers
// =============================================================================
//
// A debugging aid, OFF unless you pass --debug_native_ab_ms=<ms,ms,...>.
//
// WHY: comparing the native picture with the emulated one ("A/B") is how
// every native material was checked so far (findings 09 and 10). With two
// separate runs that only works for still scenes: characters, water and
// particles move, so two screenshots "at 55 s" show different poses. The
// game runs on both renderers at once anyway (shadow mode), so each frame
// exists twice: this saves both pictures of ONE frame.
//
// HOW: our picture is captured from the window's presenter right after we
// hand it over. The emulated one comes from guest memory: with
// --readback_resolve=full the emulator copies every resolve back into the
// Xbox's RAM, including EndFrame's resolve of the finished picture into the
// frontbuffer. The emulated GPU runs behind the game's main thread, so we
// must know when that copy has landed: we write a MARKER (a byte pattern)
// into the frontbuffer just before the game asks for the resolve, and wait
// until the marker has been overwritten. The frontbuffers alternate between
// frames, so first the previous frame's frontbuffer is marked and awaited
// too (the GPU works in order: once frame N-1's copy is done, nothing older
// can overwrite frame N's). The emulated picture then gets the display gamma
// ramp applied on the CPU, as the emulator does when it presents.
//
// Usage (the native picture must be on screen):
//   crash_mom --renderer=native --readback_resolve=full \
//       --debug_capture_dir=<dir> --debug_native_ab_ms=55000,60000
//   -> <dir>/ab_<ms>_native.ppm and ab_<ms>_emulated.ppm (ms since start;
//      the first frame after each time). The game pauses a moment for each.
//   Live instead: --debug_native_ab_trigger=<file>, then `touch <file>`.
// Raw captures are game content: keep them out of the repo.
//
// PHOTOS (F10, for everyone, no flags needed): the same capture, saved as
// PNG files into --photo_dir (default "photos", under the folder the game
// was started from; tools/play.sh uses the repo's photos/, gitignored):
//   photo_<date>_<time>_native.png + _emulated.png   native picture shown
//   photo_<date>_<time>_emulated.png                 emulated picture shown
//                                                    (just the screen)
// The emulated picture of a frame needs --readback_resolve=full, which
// slows every frame down (each resolve waits for the GPU). So a photo
// switches it on for the two frames it takes and back afterwards (the SDK
// reads that flag at every resolve): playing costs nothing until F10.
// =============================================================================

#pragma once

#include <filesystem>

#include <array>
#include <cstdint>
#include <string>

#include <rex/cvar.h>

#include "frame.h"

REXCVAR_DECLARE(std::string, debug_native_ab_ms);
REXCVAR_DECLARE(std::string, debug_native_ab_trigger);
REXCVAR_DECLARE(std::string, photo_dir);

namespace rex::ui {
class Presenter;
}

namespace native {
class TextureCache;
}

namespace native::ab_capture {

// Main thread (recorder), right before D3D resolves the finished picture
// into `frontbuffer` (xnContext::EndFrame). `base` = guest memory.
void BeforeFrontbufferResolve(uint8_t* base, const GuestTexture& frontbuffer);

// Main thread (renderer), right after our picture of the same frame went to
// the presenter. `gamma_ramp`: the display gamma table (native_renderer.h).
// `frame` = the frame both pictures show: a photo also writes its DRAW LIST
// (photo_<stamp>_draws.txt, WriteDrawList below; `material_name` names the
// materials in it).
void AfterPresent(rex::ui::Presenter* presenter, const TextureCache& textures,
                  const std::array<uint32_t, 256>& gamma_ramp, const Frame& frame,
                  const char* (*material_name)(Material));

// Debug aid (2026-09-29, the Ratcicle titan's glow: one screen-covering
// particle drawn invisibly by us): one line per draw of `frame` as OUR
// renderer saw it: material, geometry, blending, depth, alpha test, fade,
// fog, and for immediate geometry the screen rectangle its vertices cover
// (plus how many are behind the camera); particle draws add their soft-edge
// numbers. So a photo pair that differs can be matched to the draw behind
// it. `material_name` turns a Material into the game's class name.
// `textures`: for particle draws, the average colour of the sprite's base
// level as our texture cache decodes it (32-bit RGBA formats), to tell a
// dark texture from a sprite that lands wrong on screen.
void WriteDrawList(const Frame& frame, const std::filesystem::path& path,
                   const char* (*material_name)(Material), const TextureCache& textures);

// F10 (any thread): take a photo of the next frame. `native_shown`: our
// picture is on screen (then both pictures of one frame are saved; else
// only the screen).
void RequestPhoto(bool native_shown);

// Main thread, at every frame end while the EMULATED picture is on screen
// (AfterPresent doesn't run then): takes a pending photo of the screen, and
// drops a two-picture photo the switch to the emulated picture interrupted.
void WhileEmulatedShown(rex::ui::Presenter* presenter);

}  // namespace native::ab_capture
