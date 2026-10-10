// =============================================================================
// players/who_plays.h -- no profiles: who plays = which controllers play
// =============================================================================
//
// THE XBOX 360 WAY (findings/32 s.4). Every player needed a gamer profile
// signed in on their controller. The game (Radical's code, built for the 360)
// kept, in its central object (*(0x8259B190)):
//   +340  a system notification listener (sign-in changes, the Xbox guide)
//   +344  a HASH of the main player's profile name (to notice a profile change)
//   +352  the main player's user (0-3), -1 = not signed in; +356 its last value
//   +360  how many users are signed in, +364 which ones (bit N = user N)
//   +372  the main player's controller
//   +377  bit 0x80 "the profile changed" (back to the title), 0x40 "guide open"
// and filled them from Xbox services (XUserGetSigninState, XUserGetName, the
// notification listener), then APPLIED THE PROFILE (sub_82266398): the
// default difficulty, every player's Invert Axis and vibration from three
// profile settings (XUserReadProfileSettings).
//
// A PC GAME HAS NO PROFILES. What those fields really answer on a PC:
//   +364 / +360  which players have a controller (the Players tab,
//                input/players.h); player 1 always
//   +352 / +356  the main player's controller number
// The front end's compiled menu logic reads +352 and +364 in ~25 places
// (e.g. the "ContentXenonWithProfile / NoProfile" routes), so the fields
// stay, holding these PC meanings. Everything that only existed for profiles
// is gone: no name, no hash, no "profile changed", no listener, no Xbox guide,
// no settings read, no sign-in pop-up (Radical's error thread looped on it
// while nobody was signed in). The game's own functions that did this are
// replaced (overrides by address in who_plays.cpp):
//   sub_8227CEA8  a controller starts playing (Press START, joining)
//   sub_8227B1D8  every frame: the players changed? (was: Xbox notifications)
//   sub_8227CE70  made the notification listener (+340): nothing to listen to
//   sub_82266398  applied the profile: now the PC defaults (Normal difficulty,
//                 vibration on) and Invert Axis set ONCE at start, then left
//                 to the player (the 360 reset it to the profile's value at
//                 every Press START, quit to menu and join)
//   sub_82322960  the front end's set-up asked how big the profile settings
//                 are: an empty result (nothing is read any more)
//   sub_8235B400  Radical's error thread: the disc error box, without the
//                 "sign in first" loop
// No Xbox service is asked anything about players; SDK patch 0011 (players
// 2-4 sharing player 1's Xbox profile) is gone.
// =============================================================================
#pragma once

#include <cstdint>

namespace who_plays {

// The players with a controller changed (input/players.cpp): the game's
// fields are updated at its next frame.
void PlayersChanged();

}  // namespace who_plays
