// =============================================================================
// players/outfit_previews.h -- one animated 3D model per player at the chest
// =============================================================================
//
// THE GAME: the chest's outfit screen (CSkinScreenAction, vtable 0x82024D9C,
// front-end state 847) shows ONE 3D preview: the shown outfit's character,
// playing random idle animations, built by the screen itself once that
// outfit's package (an inventory section) is loaded.
//
// OURS: up to four previews, one per player, each wearing that player's
// pick, standing wherever the co-op picker (players/outfit_picker.cpp) puts
// it. Each preview runs the game's OWN preview steps (build, animate, idle,
// destroy) on a small "shadow screen" of ours (see outfit_previews.cpp for
// how that was found), and holds its own reference on its outfit's package,
// so several outfits can be loaded at once. While previews are on, the
// game's own single preview is moved out of sight.
//
// THE API (all on the game thread, e.g. from outfit_picker::AfterUpdate;
// the calls only record what's wanted: the work happens in Tick, right after
// the picker, the same frame):
//   Begin(n)          n previews (1-4); previews past n go away
//   Show(i, outfit)   preview i wears `outfit` (outfits.h numbers 0-17);
//                     loads the package first (Loading(i) meanwhile: the
//                     previous model stays until the new one is ready)
//   Place(i, x, s)    across the screen: 0 = left edge, 0.5 = middle, 1 = right
//                     (of the 16:9 picture); s = size (1 = the game's own,
//                     0.1-2); world_y = the feet's height in world units
//                     (0.1 = the game's; about 270 px of a 1080 picture per unit)
//   SetLocked(i, b)   a locked pick: no model shown (a black silhouette is
//                     the plan, not built yet)
//   End()             all previews go (also done by the screen's Exit)
// Debug: --debug_outfit_previews="0,11,9,4" (or console `flag
// debug_outfit_previews 5,6,7,8@0.8`; @ = size) = while the chest screen is up, one preview
// per listed outfit, spread evenly; "" = off (the picker drives them).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DECLARE(std::string, debug_outfit_previews);

namespace outfit_previews {

constexpr int kMaxPreviews = 4;

void Begin(int count);
void Show(int i, int outfit);
void Place(int i, float x, float scale = 1.0f, float world_y = 0.1f);
void SetLocked(int i, bool locked);
void End();
// A Crash <-> Coco pick is about to respawn the players (a small level
// reload that frees every actor in the zones, our preview models included):
// the models go now and are built again once outfits::PreviewHeld() is false.
void LetGoForRespawn(PPCContext& ctx, uint8_t* base);

// Previews are on (Begin called, End not yet).
bool Active();
// Preview i's model isn't showing its outfit yet (package loading / about
// to be built).
bool Loading(int i);

// --- hooks (outfits.cpp) ---
// After the screen's Update (outfits.cpp's wrapper of sub_820E0858): builds,
// animates and places the previews; `dt` = the Update's f1 (seconds).
void Tick(PPCContext& ctx, uint8_t* base, uint32_t screen, double dt);
// The outfit a character is drawn in if it is one of our previews (or the
// one being built right now), -1 = not ours.
int OutfitForActor(uint32_t actor);
// The outfit of one of our shadow screens, -1 = not ours (the idle picker,
// outfits.cpp's sub_820E0C98, picks Crash's or Coco's idles by it).
int OutfitOfScreen(uint32_t screen);
// The outfit being built right now, -1 = none (the controller's animation
// set, outfits.cpp's OutfitsPreviewCharacter).
int BuildingOutfit();

}  // namespace outfit_previews
