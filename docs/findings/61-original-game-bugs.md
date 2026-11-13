# 61. Bugs of the original game

Some bugs reported by players are in the Xbox 360 release itself: mistakes
in Radical's own data or code, not effects of the recompilation. The port
fixes them where the fix is small and leaves the game's look and feel
otherwise untouched. Each fix has its own setting (default on), so the
original behaviour can still be compared. Code: `src/game_bugs/`.

| Bug | Setting | Section |
|-----|---------|---------|
| Spike's jacking sound plays twice | `fix_spike_jack_sound` | 1 |

## 1. Spike's jacking sound plays twice

**The report:** jacking Spike, or Spike in the Hero levels, plays the
jacking sound twice; no other titan does that.

### Where a jack's sounds come from

A character's moves are a fight tree, `fighttrees/<type>.bfig` in
`default.rcf` (data/fight_tree.h has the file's node format). Spike is a
"Strong" titan, like the Battler, Phantom, Ratcicle, Shurtle and Sludge,
so its moves are in `fighttrees/Strong.bfig`. Each of these titans has a
node `Strong<Name>` for the moment Crash jumps on and jacks it. Its record
type, `0x602941B8`, is the hash (h × 31 + c, as everywhere in the engine)
of "MasterContentGrapple". The hashes of the other record and item names
were found by hashing every string in the executable:

| Hash | Name | What it is |
|------|------|------------|
| `0x04F3A980` | enterTracks | list run when the state starts |
| `0x27DE7629` | duringTracks | list run while it plays |
| `0x0A870F46` | exitTracks | list run when it ends |
| `0x73EE692F` | Grapple | item |
| `0x2B4155FA` | CharacterEffect | item: start and end frame, effect name, bone |
| `0x04C5D8EF` | Sound | item: a sound at a frame |

A list is a u32 kind, a u32 byte size (counted from the next word), an f32
item count, then the items one after another. A **Sound** item is:

- a u32 type;
- an f32 frame and an f32 end (-1);
- three strings, each a u32 length followed by the letters padded to
  4 bytes (for the jack sound: `ratcicle_jack_sfx`, `jacking_sfx`,
  `jacking_sfx`);
- four words: 0, a 0/1 flag, 20 (the default that the constructor
  sub_820FAED0 also writes) and 0.

At run time each item is a `CSoundTrack` (vtable `0x820286FC`). When the
state plays, a track makes a `CSoundAction` (vtable `0x82024DDC`), and that
action's slot 4, sub_820E24B0, starts the sound. The tracks of one list
sit 48 bytes apart in memory. +8 holds the frame × 16 as a u16, and
+16/+20/+24 hold name hashes (+24 = the item's second string).

### What is in Spike's list

These are the sound and effect items of each Strong titan's normal jack
(frames). The slow-motion "first time" jack is a child state with one
jack sound of its own (at 60 for Spike).

| Titan | Sounds | Effects start |
|-------|--------|---------------|
| Battler | whoosh 22, jack sound 38 | 54 |
| Phantom | whooshes 3 and 8, jack sound 15 | 36 |
| Ratcicle | whoosh 10, teleport 24, jack sound 43 | 43 |
| Shurtle | jack sound 30 | 44 |
| Sludge | whoosh 40, punch 42, jack sound 50 | 50 |
| **Spike** | whooshes 3 and 8, jack sound **10 and 30** | 54 |

Spike's list looks like a copy of the Phantom's: the same two whooshes at
frames 3 and 8, and a jack sound right after them. The effects were then
moved to frame 54 for Spike's longer animation, as in the Battler's, and a
second jack sound was added at 30 to match. The first one was never
removed. Spike in the Hero levels uses the same node, so it has the same
double.

### Seen in a running game

A new setting, `--debug_sound_track_trace`, logs every sound track start
with the frame and name hashes. To test it: spawn a knocked-out Spike with
the cheat menu, hit it once, then jack it. With the original data, four
tracks of the jack list start, 3.00, 8.00, 10.00 and 30.00, and the last
two are both `0x6C1651E9` (`jacking_sfx`). The starts are 0.67 s apart,
which is 20 frames at 30 fps.

### The fix

A data patch of `Strong.bfig` (data/data_patcher.h: the player's own file,
changed at run time) removes the frame-10 item from that list. The list's
byte size goes down by the item's 84 bytes and its count by one. Fight
tree records aren't size-prefixed, and nothing points at a record's
offset, so the rest of the file just moves up. The patch only acts on the
exact shape it expects: two jack sounds at frames 10 and 30, side by side,
in the list whose animation is `PTRN_spike_jack`. Otherwise it leaves the
file alone and logs a warning.

After the fix, the trace shows the jack list's sounds at 3, 8 and 30 for
both Spike and Spike in the Hero levels. Riding and attacking work as
before, with no errors. `fix_spike_jack_sound = false` brings back the
original's double sound.
