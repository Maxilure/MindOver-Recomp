# 32. Saves and profiles without the Xbox: a PC save drive, native sign-in, the Xbox calls meter

2026-10-10. The first pieces of the "fake Xbox" replaced by the port's own
code: storage (saves) and profiles (sign-in). Plus a meter that shows which
Xbox services the game still calls, and the README's progress bars built
from it.

Short version: the game's code already runs as recompiled C++, but it
still asks a pretend Xbox 360 (the ReXGlue runtime) for 155 system
services. Saves went through **Radical's platform drive for saves,
`XenonSaveDrive`**, which turned every file operation into Xbox "content"
calls; the port now overrides that drive's methods with a **PC save drive**
that reads and writes plain files, `user/saves/CrashMOM GameSlot N.sav`,
byte for byte what the Xbox stored. Old saves are **copied** once into the
new place (the old folders stay untouched). Profiles and sign-in are removed
from the game's code: "who is playing" is simply which controllers play (the
port's player assignment), with no profile name, profile settings, sign-in
pop-up or system notifications; the SDK patch that let players 2-4 share
player 1's profile (0011) is gone, and Invert Axis is no longer reset by
"the profile". After
both, a play session calls none of the 17 profile, storage and
notification services any more.

## 1. Measuring it: the Xbox calls meter

`src/debug/xbox_calls.*`. Every import is a C function `__imp__<Name>` in
the SDK's runtime library, called directly by the generated code. On
Linux, the linker's `--wrap=__imp__<Name>` sends each of those calls to
`__wrap___imp__<Name>`: CMake reads the 155 names from the generated
function table at configure time and writes one small wrapper per name
(count + caller, then the real function). Cost: one relaxed atomic add per
call.

Output (on by default, `--xbox_calls_meter=false` turns it off):

* the log: `Xbox calls: first XamContentCreateEx from sub_82474720+0x68`,
  the first time each service is called, with the game function that
  called it (where to cut in to replace it);
* `<log name>-xbox-calls.txt` beside the session log, rewritten every
  30 s: each service with its count and up to 8 calling places, then the
  ones never called;
* the debug console's `xbox` command ([findings/29](29-debug-console.md)).

A boot into a save before this work: 103 of the 155 imports called. The
callers confirmed how few game places talk to the Xbox for saves and
profiles: Radical's save drive (`sub_82368D10`, `sub_823690D8`,
`sub_82369558`, ...), the save handler's device selector (`sub_82258E60`),
the sign-in step (`sub_8227CEA8`, `sub_8227B1D8`) and the profile
settings read (`sub_82322960`).

**Progress bars.** `tools/native_progress.py` sorts the 155 imports into
Xbox pieces (graphics driver, profiles, saves, controllers, sound, ...)
and keeps the list of replaced ones, each with where and why;
`--readme` writes the README's "Going native" table, `--check
generated/default/crash_mom_init.cpp` confirms every import is sorted
exactly once. The 89 kernel basics (threads, locks, events, memory,
files) are listed but not counted: they are thin translations to the host
already.

## 2. How the game saves: Radical's drives

Radical's engine (`core`) reaches storage through **drives**: abstract
`Drive@core` objects with ~24 virtual methods, one per platform kind
(RTTI names): `XenonDrive` (the disc), `XenonSaveDrive` (saves),
`InstanceDrive` (a wrapper the rest of the engine holds). File and drive
operations are **request objects** (`FileOpenRequest`, `FileReadRequest`,
`DriveFileDestroyRequest`, `DriveFindFirstRequest`, ...; their type numbers
are 1-23) queued to a **drive thread** (`DriveThread@core`), which calls
the drive's methods one at a time. Each method is synchronous.

The save handler (`CSaveGameHandler`, [findings/24](24-save-system.md))
holds an `InstanceDrive` for `SAVEDRIVE:` and polls its requests (state
at +100 of a 112-byte request record; 4 = done).

### 2.1 XenonSaveDrive

Vtable `0x8200BBCC`, constructor `0x823686D0`, object 11,392 bytes.
Traced with temporary wrappers around every method (arguments, strings,
results) during a scan, a load and a save, then read instruction by
instruction:

| Slot | Address | Method | What the Xbox version did |
|---|---|---|---|
| 7 | `0x82368818` | SetDevice(r4 = 1, r5 -> {device, user}) | stores the device id and user (+224/+228), marks "changed" |
| 9 | `0x82369040` | CheckMedia | checks the device (notifications, device data, content list) and fills the media info |
| 11 | `0x823690D8` | Open(name, mode, write, metadata, size, -> slot, -> size) | finds the content by file name, creates or opens it as a drive of its own (`s0:`), opens the file inside |
| 12 | `0x82368B98` | Close(slot) | closes the file and the content |
| 13 | `0x82369558` | Commit(slot) | writes a new file's size header, flushes the content |
| 14 | `0x82369678` | Read(slot, pos, buffer, count, -> read) | |
| 15 | `0x82369790` | Write(slot, pos, buffer, count, -> written, -> size) | the 9th argument is on the caller's stack (+84) |
| 19 | `0x823699D0` | Delete(name) | deletes the content with that file name |
| 20 / 21 / 22 | `0x82368BE8` / `0x82369890` / `0x82368890` | FindFirst / FindNext / FindClose | wildcard walk over the content list |
| 4 / 5 / 8 | | constant / content size calculation / capability bits (71) | no Xbox service |

The object's fields other code reads: +124 media info (+124 error, +128
free bytes, +132 free blocks, +136 block size 2048, +140 the device's
name, 65 bytes) and **+208, the last error**, in Radical's numbers, not
Win32's (the conversion table `0x82368958`: 0 none, 1 not found, 3, 4 no
media, 5, 6 already exists, 7 no space, 8 failure, 11, 12 corrupt, 15).
Internally: 8 open-file records at +10172 (16 bytes: handle, content root
name, size, "created" flag), 4 find records at +10300, a 32-entry
`XCONTENT_DATA` list at +316 (count +312).

**The file's own header.** Open/Commit treat the first 4 bytes of every
save file as the drive's header: the total file size (`0x4EED`, findings/24
s.1). A new file is created `size + 4` bytes long with the header 0;
Commit writes the size; Open of an existing file compares the header with
the file's size and reports error 12 ("corrupt") when they differ. Read
and Write positions are counted after it.

Open's mode (r5): 0 = an existing save (the scan of an empty slot gets
error 1, "not found"), 1 = open or create, 3 = create (replace), 2 =
create new (not seen in use).

**The device selector.** At boot the save handler (`sub_82258E60`) calls
`XShowDeviceSelectorUI` through `sub_823002D8` (user, content type, flags,
bytes needed, -> device id at handler +128, overlapped at +132); the
handler's update polls the overlapped (`sub_82300300`: InternalLow 0 =
done) and gives the device to the drive (slot 7). A return of 997
(`ERROR_IO_PENDING`) means "accepted"; anything else makes it retry once
with other flags.

## 3. The PC save drive

`src/saves/pc_save_drive.*` overrides slots 9, 11-15, 19-22 by address
(`REX_FUNC(sub_X)`), plus the device selector wrapper. Everything above
the drive (requests, drive thread, save handler, save manager, the save
list) runs unchanged.

* **Files:** `user/saves/<the game's name>.sav`, i.e. `CrashMOM GameSlot
  N.sav`: the name the game asks for plus `.sav`, so nothing translates
  names. Same bytes as before, header included (old and new saves are
  interchangeable by copying).
* **Open** reads the whole file into memory and checks the header like
  the original; a new file is `size + 4` zero bytes. **Read/Write** work
  on memory. **Commit and Close** write it to disk through a temporary file
  renamed over the old one: a crash mid-save leaves the previous save
  whole (the original wrote into the file directly).
* **Delete** (the save manager deletes a slot's save before every
  overwrite) moves the file into `user/saves/Backups/` (`<name> (<date
  time>).sav`, the newest 10 kept per save); an Open in "create" mode over
  an existing file does the same first. Nothing the game deletes is lost.
* **CheckMedia** reports the PC drive's free space; the saves folder is
  always "present".
* **FindFirst/FindNext** walk the `.sav` files with a case-insensitive
  `*`/`?` match.
* **Errors** go to +208 / +124 in Radical's numbers, so the game's own
  messages ("corrupt", "couldn't save") still appear where they would.
* **Device selector:** device 1, done at once (InternalLow 0), return 997.

The save list's own operations (`src/saves/save_files.*`: list, rename,
the list's Y = move to `Deleted saves/`, the played order in
`save_library_played.txt`) moved to the same flat folder; both threads
(game, drive thread) take one lock around file work.

### 3.1 The one-time copy

At startup, before the game runs, `save_files::CopyFromXboxLayout()`
copies every save from the old layout

```
user/saves/<profile id>/565507FA/00000001/CrashMOM GameSlot N/CrashMOM GameSlot N
```

to `user/saves/CrashMOM GameSlot N.sav`, keeping each file's date (the
list is ordered by it) and merging the old played order. The old folders
are never changed or deleted: they stay as a backup, and an older build
still finds them. A file already present with the same bytes is skipped;
a number already taken by different bytes gets the next free number. When
every copy worked, `user/saves/copied-from-xbox-layout.txt` lists what was
copied, and the copy never runs again (a save deleted afterwards stays
deleted). The launcher shows the old layout's saves until that file
exists.

A trap found on the way: `std::filesystem`'s file clock on libstdc++
counts from 2174, so the "played" ticks are negative; merging with
`max(0, ticks)` made every save look played at time 0 (the copy now never
compares with a default 0).

## 4. Profiles and sign-in

What the 360 version asked, and from where:

| Question | Game-side wrapper | Asked by |
|---|---|---|
| Which of users 0-3 are signed in (`XUserGetSigninState`) | `sub_82300780` | the sign-in step `sub_8227CEA8` (main controller chosen), the notification step `sub_8227B1D8`, Radical's error thread `sub_8235B400` |
| The profile's name (`XUserGetName`) | `sub_82300778` | the same two steps: the game keeps a hash of the name (+344) to notice a profile change |
| Profile settings (`XUserReadProfileSettings`) | `sub_82322960` | the front end (`sub_8225F8A0` asks the size, `sub_82266568` reads): difficulty `0x10040015`, invert Y `0x10040002`, vibration `0x10040003` |
| System notifications (`XamNotifyCreateListener`, `XNotifyGetNext`) | `sub_823007D0` | the game (`sub_8227CE70`, polled every frame by `sub_8227B1D8`: 9 = system UI shown/hidden, 10 = sign-in changed) and the save drive (storage changes) |

The notification step on "sign-in changed": the signed-in mask (+364) and
count (+360) for users 0-3; the main controller (+372) -> the input
manager's controller -> its device -> its user index (virtual slot 7) ->
+352 (-1 if that user isn't signed in); the name's hash compared with
+344, and when it differs (and +356 isn't -2) the profile changed: +377
bit 0x80, the save handler and the front end are told.

A PC game has no profiles, so the port doesn't imitate them: it **removes
the idea** from the game's code (`src/players/who_plays.*`). The fields stay,
because the front end's compiled menu logic reads +352 and +364 in about 25
places (e.g. the tree's `ContentXenonWithProfile` / `ContentXenonNoProfile`
branches), but they hold their PC meaning: **+364/+360 = which players have
a controller** (the Players tab; player 1 always), **+352/+356 = the main
player's controller number**. The game functions that dealt with profiles
are replaced by address:

| Function | 360 | PC |
|---|---|---|
| `sub_8227CEA8` (Press START, joining, the quick load) | signed-in users, the controller's user, the name's hash | players with a controller, the controller's player number; the same calls to the save handler and the front end |
| `sub_8227B1D8` (every frame) | next system notification (sign-in changed, guide) | runs only when a controller is added, removed or moved |
| `sub_8227CE70` | creates the notification listener | nothing to listen to |
| `sub_82266398` (apply the profile) | default difficulty, every player's Invert Axis, vibration, from the profile settings | the same defaults (Normal, off, on), but Invert Axis is set once and then left to the player |
| `sub_82322960` (the front end's set-up asks the settings' size) | `XUserReadProfileSettings` | an empty 8-byte result, never read |
| `sub_8235B400` (Radical's error thread) | sign-in pop-up loop until somebody is signed in, then the disc error box | the disc error box |
| `sub_823007D0` (listener wrapper; only the save drive still calls it) | `XamNotifyCreateListener` | handle 0 |

No name, no hash, no "profile changed", no settings read, no sign-in
pop-up. The SDK patch `0011-local-players-share-profile` (players 2-4
sharing player 1's Xbox profile) was removed: nothing asks the SDK about
players now.

**Invert Axis.** On the 360 the profile owned it: every Press START, quit to
the menu and join re-applied the profile and reset each player's Invert Axis
(front end +8588..8591) to the profile's value. With no profile, the
player's choice (Options -> Controls) now stays.

**A trap on the way.** The original "apply the profile" sets the default
difficulty (game object +240) whenever there is no main player yet (+352 =
-1), whatever its arguments say; the first PC version only set it when asked.
At boot the front end's set-up calls it with no main player, so +240 stayed
0 until the first Press START, and the quick load (which skips Press START's
timing) started a level that asked the content loader for
`levels/L0/(null)`: `strrchr(name, '.') + 1` = address 1, a crash. Found by
switching each replaced function back to the original (only the pair
"apply the profile" + its settings read made the crash go away), then
comparing the two versions' memory after sign-in and the original's
branches.

## 5. Tests (an installed-style copy with a copy of the saves)

* First start: 29 old saves copied, every copy byte-identical with the
  same date; the second start copies nothing.
* `--load_save=last` -> the most recently played save, gameplay.
* In game, Save Game over the current save: "Save successful.", the old
  file in `Backups/` (old date), the new one 20,205 bytes with header
  `0x4EED`, the same 27-byte tail, new date and play time, the name kept.
* Create New Save (through the game's name screen): a new
  `CrashMOM GameSlot 30.sav`; loads in a fresh run.
* The list's Y (delete): moved to `Deleted saves/`; X (rename): name
  changed, date kept.
* Boot through the title (Press START = the sign-in step) -> main menu ->
  Load Game -> load.
* Two and three players with fake controllers (`--debug_fake_pads`):
  join, no profile message, "playing now: 1 2 3" in the log; with the
  SDK rebuilt without patch 0011.
* Invert Axis on for player 1, then Quit Game (back to the title) and Press
  START: still on (the 360 version reset it twice).
* Saving over a save after the new sign-in: "Save successful.".
* The meter after all this: `XamContent*` (9), `XamEnumerate`,
  `XamShowDeviceSelectorUI`, `XamUser*` (3), `XamNotifyCreateListener`,
  `XNotifyGetNext`: 0 calls. 0 errors in the logs.
* The launcher: its save list and the report bundle read the flat files
  (`--make_report --report_save=10` bundles `saves/CrashMOM GameSlot
  10.sav`).

Not tested yet: Windows (the code is portable; `std::filesystem::rename`
replaces an existing file there too), a disk-full save, a save whose
header doesn't match (read-only path: reported as corrupt, the file left
alone).
