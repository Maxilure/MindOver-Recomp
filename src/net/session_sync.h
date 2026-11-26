// =============================================================================
// net/session_sync.h -- online session rules: non-pausing menus, shared pickups,
//                       host-to-client save sync, area/cutscene transitions,
//                       and ridden + pocketed titan ownership sync
// =============================================================================
//
// WHAT THIS MODULE HANDLES (all strictly gated behind net::Active() so offline
// single-player and local 2-4 player co-op remain 100% original):
//
//   1. NON-PAUSING MENUS & INTERACTABLES:
//      - Intercepts CUberManager::SetPaused (sub_8227C8E8) and SetPausedWithSound
//        (sub_8227C950) while in a level so opening Pause, Save Totems, Outfit
//        Chests, Map, or Upgrade screens never sets CTimeManager's pause bit
//        (+24 bit 0x80), never sets uber +168 bits 0x10/0x20 during simulation,
//        and never pauses world audio.
//      - Wraps CConfirmBox::Update (sub_8225DE68) to present bit 0x20 only for
//        the duration of the UI update (without it, 0x8225DEC8 auto-closes the
//        Save Totem's "Do you want to save the game?" prompt).
//
//   2. SHARED PICKUPS -> HOST AUTHORITATIVE PROGRESSION:
//      - Tracks frame-to-frame positive deltas on EStatInt[0..182] (Mojo, Crash
//        upgrade mojo, Titan mojo/levels, Voodoo Doll mission counts, idols),
//        unlock tables (Outfits, Concept Art, Enemy Info, Tutorials, Coco),
//        power-ups (Free Jack, Mojo Multiplier), and world Collectable pickups
//        (sub_821991B0).
//      - Clients send reliable kMsgPickupEvent packets to the Host; the Host
//        applies them authoritatively and broadcasts kMsgStatsSync so every
//        connected player shares identical progression.
//
//   3. SAVES (HOST NORMAL, CLIENTS CAN SAVE HOST'S CAMPAIGN LOCALLY):
//      - Host saves, renames, and deletes its own save files normally, and
//        packs its live 20,174-byte save block (sub_8225A7D0) whenever a client
//        opens a Save screen or triggers SaveGame.
//      - Clients receive the Host's authoritative save block via reliable
//        kMsgHostSaveChunk packets and write that campaign state into their own
//        local user/saves/ folder when saving (never touching Host's disk).
//
//   4. SYNCED AREA TRANSITIONS & CUTSCENES (+ VOID-DEATH IMMUNITY):
//      - Hooks door/portal level transitions (sub_82293D70, sub_822975A8, and
//        CUberManager::ChangeLevel sub_8227B640) and CMasterCinematicAction
//        (sub_820B3430) so moving to a new area or starting a cutscene brings
//        all players along together and protects remote puppets from void deaths
//        while zones load.
//
//   5. TITAN OWNERSHIP + POCKETING SYNC:
//      - Tracks each player's ridden titan template and pocketed titan template
//        (including RB swaps when holding two titans: one ridden + one pocketed),
//        reconciles remote titan mounts/dismounts natively without fake button
//        presses, and populates carried titan names in sub_8229CB78 across loads.
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace net {

// Protocol message IDs handled by session_sync (dispatched from state_sync.cpp).
constexpr uint8_t kMsgAreaTransition = 20;  // Any -> Host -> All (reliable): level + entry
constexpr uint8_t kMsgCutsceneSync = 21;    // Any -> Host -> All (reliable): cinematic start
constexpr uint8_t kMsgTitanOwnership = 22;  // Owner -> Host -> All (reliable): ridden + pocket
constexpr uint8_t kMsgPickupEvent = 23;     // Any -> Host -> All (reliable): mojo/doll/powerup
constexpr uint8_t kMsgStatsSync = 24;       // Host -> Clients (reliable): full EStatInt + unlocks
constexpr uint8_t kMsgRequestHostSave = 25; // Client -> Host (reliable): request live save blob
constexpr uint8_t kMsgHostSaveChunk = 26;   // Host -> Client (reliable): 1 KB chunk of save blob
constexpr uint8_t kMsgWorldInteract = 27;   // Any -> Host -> All (reliable): shared NPC/world interact
constexpr uint8_t kMsgOutfitSync = 28;      // Owner -> Host -> All (reliable): [slot, outfit_id]

namespace session_sync {

// Reset all session sync state on host/join/leave.
void Reset();

// Called once per game frame on the main guest thread (from cheats.cpp GetScale).
void TickFrame(PPCContext& ctx, uint8_t* base);

// Handle incoming protocol messages 20..28 (called from state_sync.cpp).
void HandlePacket(const uint8_t* data, size_t size, bool is_host, uint32_t sender_peer);

// Interaction tracking & shared NPC/cutscene replication (called from menu_input.cpp
// sub_8214A9A8 around CInteractorBehaviour::Interact).
void BeginPlayerInteract(int player);
void EndPlayerInteract(PPCContext& ctx, uint8_t* base, int player, uint32_t actor,
                       uint32_t target, uint32_t interactor_flag);
bool ShouldSuppressMenuOpen();

// True when a local UI menu / confirm prompt / outfit picker is open on THIS PC
// during online play, so gameplay controllers & outgoing snapshots mute character
// actions while letting the local player navigate their menu normally.
bool IsLocalMenuOpen(uint8_t* base);
bool IsLocalMenuOpenCached();
bool ShouldBlockLocalGameplayInput(uint8_t* base);

// Another PC left a 3D cutscene: answers our cutscene step's own Skip questions
// (START at lr 0x82242E98, A at lr 0x82242E1C) with yes, from the front end's
// IsButtonPressed wrapper (players/menu_input.cpp). True = answer "pressed".
bool AnswerRemoteSceneSkip(uint32_t lr, uint32_t button);

// Current level ID on this PC (CUberManager + 20, signed byte; -1 if not in play).
int CurrentLevelId(uint8_t* base);

// True while loading, during an area transition, or within the post-load /
// post-cutscene grace window (~2.5 s) where cross-area coordinates or kill
// planes must never kill or snap remote puppets.
bool InTransitionOrCutsceneGrace();

// True if `actor` belongs to a remote online player (so local void/pit damage
// on this PC is suppressed unless confirmed by the owner).
bool IsRemotePlayerActor(uint32_t actor);

// Returns true if a remote player's titan is currently being force-dismounted
// because its owner pocketed or unjacked it on their PC (checked in
// lost_controller.cpp sub_8221F1E8).
bool ShouldForceDismountTitan(uint32_t titan_actor);

// Client save support: request a fresh snapshot of Host's save block when the
// Save screen opens, and retrieve the latest 20,174-byte game save block (plus
// outfit picks & cheat flag) when writing a save file on a Client.
void RequestHostSaveSnapshot();
bool GetHostSaveForClient(std::vector<uint8_t>& out_block_20174,
                          std::array<int8_t, 4>& out_outfits,
                          bool& out_cheated);

// Short diagnostic line for the `online` debug console command.
std::string DebugStatus();

}  // namespace session_sync
}  // namespace net
