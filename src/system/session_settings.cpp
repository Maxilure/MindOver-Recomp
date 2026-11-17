// =============================================================================
// system/session_settings.cpp -- the settings that change the world
// (session_settings.h)
// =============================================================================
#include "system/session_settings.h"

#include <fmt/format.h>
#include <rex/cvar.h>

namespace session_settings {
namespace {

struct WorldSetting {
  const char* name;
  const char* why;  // what in the world it changes
};

// Every setting that changes what the game decides, or when (2026-11-17).
// Grouped; the order is part of the fingerprint (append new ones at the end
// of a group, or accept that old fingerprints change: they're only compared
// between PCs running the same build).
constexpr WorldSetting kWorldSettings[] = {
    // --- the session's clock and its helpers -------------------------------
    {"fixed_step", "every frame's length (the session's frame rate)"},
    {"loading_follows_game_frames", "frames wait for idle helper threads (loads land on the same frame)"},
    {"helper_threads_between_frames", "helper threads work only between frames"},
    {"helper_threads_on_game_time", "helper threads' clocks and timed waits follow game frames"},
    {"sound_follows_game_frames", "sound mixed per frame: cutscenes are timed by their sound"},
    {"sound_out_native", "who runs the game's mixer (the port's own output or the SDK's)"},
    {"voice_language", "which voice lines play: their lengths time cutscenes and tutorial lines"},
    // --- players -------------------------------------------------------------
    {"local_players", "how many players the co-op tables and spawn events are set up for"},
    {"lost_controller", "what happens to a joined player without a device"},
    {"coop_any_player_triggers", "who may start story triggers and interactions"},
    {"coop_mask_from_anywhere", "when a player may turn into a mask"},
    {"invert_axis_player1", "player 1's stick up/down, applied by the game"},
    {"invert_axis_player2", "player 2's stick up/down, applied by the game"},
    {"invert_axis_player3", "player 3's stick up/down, applied by the game"},
    {"invert_axis_player4", "player 4's stick up/down, applied by the game"},
    // --- gameplay rules --------------------------------------------------------
    {"friendly_fire", "whether players' hits on each other land"},
    {"friendly_hits_break_combo", "whether a partner's hit breaks a combo"},
    {"simple_spinning", "LB / the Spin key spin (else only stick circles)"},
    {"keep_spinning", "holding Spin keeps the spin going"},
    {"reticle_hide_idle_ms", "an idle mask's reticle locks out (a behaviour state)"},
    {"background_input", "an unfocused window pauses the game (off) or plays on"},
    // --- physics and timing fixes ---------------------------------------------
    {"ground_grace_ms", "how long a lost ground contact still counts as ground"},
    {"platform_move_fix", "moving platforms' minimum steps scaled by the frame time"},
    {"fix_speech_cut_off", "tutorial lines wait for the voice before them"},
    {"fix_spike_jack_sound", "data patch of a fight tree's track list"},
    {"zone_list_growth", "zones' character lists grown (more characters updated / faded in)"},
    {"outfit_store_fix", "which character packages the front end keeps loaded"},
    // --- menus that run the game's own flow ----------------------------------
    {"save_screen_waits", "the save screens' waits (front-end states take longer)"},
    {"save_success_screens", "the 'saved' message states in the front end's flow"},
    {"outfit_picker", "the chest's flow (the port's picker or the game's screen)"},
    // --- the co-op camera (until each PC has its own camera: the stick is
    //     camera-relative, so the camera decides where a player walks) -------
    {"coop_camera", "the shared camera: stick directions are camera-relative"},
    {"coop_camera_group_focus", "the shared camera's centre"},
    {"coop_camera_max_distance", "the shared camera's distance"},
    {"coop_camera_reach", "the shared camera's reach"},
    {"coop_camera_room", "the shared camera's room around each player"},
    {"coop_camera_walls", "the shared camera's wall checks"},
    {"coop_camera_pinned_rooms", "the shared camera in pinned rooms"},
    {"coop_camera_game_clock", "the shared camera's springs on game time"},
    {"net_own_camera", "each PC's camera follows its own player (experiment: the world may still read the camera)"},
};

}  // namespace

uint64_t Fingerprint() {
  uint64_t h = 0xCBF29CE484222325ull;
  for (const WorldSetting& s : kWorldSettings) {
    const std::string line = std::string(s.name) + "=" + rex::cvar::GetFlagByName(s.name) + "\n";
    for (unsigned char c : line) {
      h ^= c;
      h *= 0x100000001B3ull;
    }
  }
  return h;
}

std::string Report() {
  std::string out;
  for (const WorldSetting& s : kWorldSettings) {
    const std::string value = rex::cvar::GetFlagByName(s.name);
    out += fmt::format("{:32} = {:12} ({}){}\n", s.name, value.empty() ? "?" : value, s.why,
                       value.empty() ? "  <- NO SUCH SETTING" : "");
  }
  out += fmt::format("{} world settings, fingerprint {:016X}\n", sizeof(kWorldSettings) / sizeof(kWorldSettings[0]),
                     Fingerprint());
  return out;
}

}  // namespace session_settings
