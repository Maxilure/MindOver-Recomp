// =============================================================================
// input/key_events.h -- the game's input EVENTS worked out a second time, on
// a value set of our own (findings/62: the native keyboard, step 2)
// =============================================================================
//
// THE GAME (findings/62 s.1-2): every question about a button ("did the
// player jump?", "was Back pressed in this menu?") reads the "happened" bit
// of an EVENT in one of the player's INPUT MAPS (MENU, GENERAL, PLAYER,
// ENEMY...). Once a frame the input controller works those bits out, map by
// map (sub_82275E20, r3 = map, r5 = controller, f1 = frame time), from its
// VALUE CHANNELS: one ring of samples per controller value (controller +0x2C
// + 12 x value: {samples, ring size, newest}; a sample = 16 bytes: +0 value,
// +4 time in ms, +8 bit 0x80 = down). The rules (sub_82275768) only read
// the channels, the controller's last update time (+0x194), +0x20 and the
// parent link (+0x24).
//
// WHAT THIS DOES: for a socket, a SHADOW of each map (a copy of its event
// table and the events' inputs, in guest memory: the rules keep their
// sequence progress in them) and a SHADOW CONTROLLER (the four fields above
// + rings of its own). After the game has worked out a map, the shadow map
// goes through the SAME game code (__imp__sub_82275E20) with the shadow
// controller. So any value set can make events by the game's own rules,
// timing windows and chains included, without touching the real ones.
//
// STEP 2: the check. With --debug_key_events_check the shadow
// controller gets a copy of the REAL channels every frame, and every shadow
// event (all bytes the rules keep, and the inputs' state) is compared with
// the game's own: zero differences = the shadow route makes exactly the
// game's events. Log lines "Key events check:".
//
// STEP 3: THE KEYBOARD AS EVENTS (findings/62 s.7). The keyboard's socket
// gets its OWN shadow of each map and two shadow controllers fed with the
// keys (the driver's EventImage: menu keys for the MENU and CHEAT maps,
// play keys for the others, either for MOVIE), through the game's sample
// writer sub_82273328; the shadow's happened bits are added to the real
// map's. The game no longer gets the keyboard's buttons at all, so a key
// bound only to a menu job (Backspace = the menus' Y) can't be a heavy
// attack, held or not.
// =============================================================================
#pragma once

namespace key_events {

// The keyboard reaches the game as events (setting keyboard_events, on by
// default): the XInputGetState wrapper (saves/save_library.cpp) then takes
// the keyboard's merged buttons and triggers back out of what the game gets
// (the port's own readers still see them; the sticks stay). Any thread.
bool Active();

}  // namespace key_events
