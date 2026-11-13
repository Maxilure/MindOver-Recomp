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
| `sub_82266398` (apply the profile) | default difficulty, every player's Invert Axis, vibration, from the profile settings | the same defaults (Normal, off, on), but Invert Axis (and the volumes) = the player's settings (findings/31 s.6) |
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
player's choice (Options -> Controls) now stays: since findings/31 s.6 it
comes from `user/settings.toml` (`invert_axis_player1`-`4`), applied there
and after every save load (saves carried their own copy too).

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

## 6. Console settings (TV mode, video flags, dashboard version)

The last "Console settings" imports in the meter were questions about the
console itself, asked once at start-up. A scan of every `bl` to the eight
import thunks (`XGetVideoMode` 0x824BB94C, `XGetAVPack` 0x824BBA4C,
`XexCheckExecutablePrivilege` 0x824BBB0C, `ExGetXConfigSetting` 0x824BBADC,
`XamGetSystemVersion` 0x824BB99C, `XGetGameRegion` 0x824BB9EC, `XGetLanguage`
0x824BBA3C, `XamGetExecutionId` 0x824BBA7C) found 26 call sites; Ghidra on
each caller showed what the answers are used for:

| Question | Asked by | Used for |
|---|---|---|
| `XGetVideoMode` (48-byte `X_VIDEO_MODE`) | PDDI display set-up sub_82433B00 | widescreen -> xnDisplay +0x678 bit 0x40; hi-def -> 1280x720 back buffers (else 640x480) |
| | the game's start-up sub_8227A620 | video standard 3 (PAL) -> byte 13 of the movie object sub_822C8C18 |
| | D3D sub_82316C48 / sub_82318A50 | the display scaler (re-set when the mode changes; 4:3 or 16:9 filter ratios) |
| `XamGetSystemVersion` | Microsoft library code sub_82300140 / sub_82300218 / sub_82322A90 | old or new dashboard features (by build number) |
| `ExGetXConfigSetting(3, 10)` | D3D device creation sub_82311B70 | user video flags -> device +0x414C |
| privilege 10, `XGetAVPack`, `ExGetXConfigSetting(2,2)` + `(3,10)`, `XGetLanguage` | sub_823009B8, called from the program's entry point 0x82300BA0 | a start-up check for a standard-definition cable on a PAL console: shows a system message box (text by language) and returns 1, and the entry point then ends the program |
| `ExGetXConfigSetting(3, 14)` (country), `XGetGameRegion` | sub_82300398 / sub_82474258 (via sub_8235B860) | the language letter: only called from sub_82262980, replaced by `language.cpp` |
| `XamGetExecutionId` | sub_82300FF0 | only called from sub_82322960, replaced by `who_plays.cpp` |
| 8 x `ExGetXConfigSetting` | sub_824769B0 | a Microsoft helper never reached in play |

The emulated Xbox answered the TV mode from the SDK's `video_mode_width` /
`video_mode_height` / `video_mode_refresh_rate` flags, so what the game heard
depended on whatever a settings file held (any size and refresh rate a saved
settings file once wrote). The port now answers every reached site itself
(`src/system/console_settings.cpp`; midasm hooks "CONSOLE SETTINGS" in the
manifest, each one filling the answer and jumping over the call):

* TV mode: always 1280 x 720, progressive, widescreen, hi-def, 60 Hz, NTSC:
  the picture the game was made for. The port draws that picture at any
  size and shape itself (render resolution, widescreen, the presenter).
* User video flags: 0x00040000 and dashboard version: 0, the values every
  session had so far.
* The start-up check returns 0 ("nothing to tell the player") without
  asking anything: a PC has no video cable. The SDK's answer (cable 6, VGA)
  gave the same result.

`--console_settings_native=false` lets the SDK answer again (comparison
switch).

Tests (installed-style copy, `--load_save=last`): the four answers logged
once each ("Console settings: ... answered by the port"); the meter shows 0
calls for all eight imports in play (with the switch off: the old callers
and counts come back: video mode 2, dashboard version 3, privilege, cable,
config 1 each); gameplay pictures in the native and the emulated renderer
look as before; 0 errors.

## 7. System messages (XMsg*): the music player and rich presence (study)

The meter's three `XMsg*` imports looked like a big job (about 65,000
calls in a five-minute session). They are not: every one of them comes
from two small Microsoft helpers, and neither is about achievements any
more (the achievement writer sub_82292938, the old user of message
0x000B0008, is already replaced: `src/achievements/`).

`XMsgInProcessCall(app, message, buffer, ...)` / `XMsgStartIORequest(Ex)`
send a numbered message to a system "app". The numbers, read from the
`lis`/`ori` pairs before each call and matched against the SDK's app
handlers (`src/kernel/xam/apps/xmp_app.cpp`, `xgi_app.cpp`):

| Import | Wrapper | App | Message | Meaning |
|---|---|---|---|---|
| `XMsgInProcessCall` | sub_822FFF70 (via sub_82300130) | 0xFA XMP | 0x00070009 | XMPGetStatus: is the player's own music playing? |
| `XMsgInProcessCall` | sub_82300080 | 0xFA XMP | 0x0007001B | XMPGetPlaybackController |
| `XMsgStartIORequestEx` | sub_822FFFD8 | 0xFA XMP | 0x0007001A | XMPSetPlaybackController (take / give back the music) |
| `XMsgStartIORequest` | sub_823006E8 (via sub_82300770) | 0xFB XGI | 0x000B0006 | XUserSetContext: rich presence |

### 7.1 The music player ("custom soundtracks")

The Xbox 360 lets a player run their own music from the dashboard over
any game; a game must then mute its own music, and must take the music
back while it plays a movie or a cutscene track. Radical's music code does
exactly that:

* sub_822DE3C8 = "player music on?" (XMPGetStatus, answer = state != 0).
* sub_822DE270 (start, from sub_822D60D0) and sub_822DE338 (EVERY FRAME,
  from the music player's update sub_822D6798, a vtable slot) keep bit 0x80
  of a flags byte; when the answer changes, the game's music is muted
  (sub_822D8338) or brought back (sub_822D83A8) through the object at
  0x825A75AC. This per-frame poll is the 65,000 calls.
* sub_822DE2D8 / sub_822DE320 = a counted "take the music" / "give it back"
  (count at +4; only the first take and the last give-back reach
  sub_82300140 / sub_82300218 = XMPOverrideBackgroundMusic /
  XMPRestoreBackgroundMusic, which ask and set the playback controller).
  Callers: CMoviePlayer slot 4 sub_822C4190 (play) and its stop
  sub_822C4A00, CCinematicSoundTrackAction slots 4/6 sub_82240EB0 /
  sub_82241150, and the music player itself (sub_822DD740 / sub_822DD7C8).
  On a dashboard version 0x8A100 or newer (XamGetSystemVersion) they only
  send XMPSetPlaybackController; older ones first ask (GetPlaybackController)
  and keep their own state at 0x82520C80 / 0x82520C84 (critical section
  0x824DE8A4). Our dashboard version is 0 (section 6), so the old path runs
  here: that is the XMPGetPlaybackController caller in the meter.

A PC has no dashboard music: the native answer is "no player music,
nothing to take": sub_822DE3C8 returns 0, sub_82300140 / sub_82300218
return 0 (success) without a message. The game's own counting and muting
code stays as it is.

### 7.2 Rich presence

sub_8227B3B8 (from the per-pass subsystem update sub_8227CE08 in the main
loop) runs over the four players: for each one in `uber +364` (players
with a controller) it sets two contexts through sub_82300770:

* context 1 = what the player is doing, from the game mode at `this+8`
  (jump table at 0x8227B424): 2 / 4, 3 while front-end flag
  `fe+8594` bit 0x10, 1 or 14 by sub_82263608, or the byte at `this+376`;
* context 0x8001 (`X_CONTEXT_PRESENCE`) = 0.

On the 360 these fill the friends list's "what is Crash doing" line. The
SDK only logs them. Native answer: nothing to send (the wrapper returns
success). The values themselves could feed a PC rich presence later
(Discord), which is why they are written down here.

### 7.3 Replaced

`src/system/system_messages.cpp` overrides the four Microsoft wrappers:

* sub_82300130 (XMPGetStatus): writes 0 = idle, returns 0. sub_822DE3C8
  turns "idle" into "the game may play its own music", as before.
* sub_82300140 / sub_82300218 (take / give back the music): return 0
  without a message (their dashboard-version midasm hooks of section 6
  only run with the switch off now).
* sub_823006E8 (XUserSetContextEx, the only target of sub_82300770):
  returns 0 without a message.

Also gone: the SDK's XMPGetStatus slept 1 ms whenever it was asked from a
thread other than the main one. `--system_messages_native=false` lets the
SDK answer again.

Tests (installed-style copy, `--load_save=last`, 40 s of play): the four
answers logged once each ("System messages: ... answered by the port"), the
meter shows 0 calls for all three `XMsg*` imports; with the switch off the
same run gives 34,674 `XMsgStartIORequest` (presence, sub_823006E8+0x44),
1,353 `XMsgInProcessCall` and 2 `XMsgStartIORequestEx` (how the counts split
between the two kinds depends on the session). The level's music streams
load the same in both runs; 0 errors.

## 8. The last system pop-ups and the network start-up

Four services were still answered by the SDK after sections 3-7. Each has
exactly one caller (Xbox calls meter "first X from sub_Y+off" lines, a grep
of the generated code for each import, `tools/callgraph.py` for the callers'
callers, every site read with xexdis). `src/system/system_popups.cpp`
answers them; `--system_popups_native=false` lets the SDK answer again.

| Service | Caller | 360 behaviour | PC behaviour |
|---|---|---|---|
| XNotifyPositionUI | 0x8227AE14 in the start-up object set-up sub_8227AC58 (`li r3,1` before it), once per boot | where the dashboard shows its notification toasts | skipped (midasm hook jumps over the `bl`; the next instruction `mr r3,r27` overwrites r3) |
| NetDll_WSAStartup | sub_8225F8A0 -> sub_82322950 (a Microsoft network library's start-up, config 0x824E5D2C) -> sub_823228C8 -> WSAStartup sub_82322B38 -> dispatcher sub_82322A90, once per boot | starts the console's socket library | sub_82322B38 returns 0 (success): sub_823228C8 only checks r3 == 0 before laying out its memory block; the WSADATA is a stack buffer nobody reads. No sockets: the game has no online features |
| XamShowDirtyDiscErrorUI | sub_823002E0, called only by Radical's error thread sub_8235B400 (section 4) when the file layer reports a failed read | "the disc is unreadable" box, then back to the dashboard (sub_82300DB0, never returns) | a message box (the files in the `game` folder may be missing or damaged; the installer repairs them), log line, exit. The box opens on the UI thread; the game thread waits for the exit |
| XamShowMessageBoxUIEx | sub_82300878, whose only caller is the start-up cable check sub_823009B8 | the PAL / standard-definition cable message | already gone: sub_823009B8 is replaced as a whole (section 6) |

With WSAStartup answered, the dispatcher sub_82322A90 (and its
dashboard-version hook of section 6) is no longer reached.

Tests (copy of the saves, `--load_save=last`, Native Only, 30 fps): both
"System pop-ups: ... answered by the port" lines logged once, console `xbox`
shows 0 calls for all four services, the level loads, 0 errors. The disc
error box was never seen in a real session; `--debug_disc_error_box` shows it
at start-up (on the once-per-boot notification call): the box window opened
(title "Mind over Recomp"). Closing it with OK (exit afterwards) was not
tested.

## 9. The graphics driver, step 1: the simple answers

Direct3D (linked into the game, code 0x82305000-0x82323000) is the only
caller of the console's graphics driver, 20 `Vd*` kernel calls. Six of them
are the emulated GPU itself and stay with the SDK for now: the vertical-blank
interrupt (`VdSetGraphicsInterruptCallback`), the swap (`VdSwap`,
`VdGetSystemCommandBuffer`, `VdPersistDisplay`) and the command ring
(`VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`). The other 14
are questions or no-ops. `src/system/graphics_driver.cpp` answers them at all
21 call sites (manifest "GRAPHICS DRIVER", one midasm hook per `bl`, found by
a search of the generated code for each import; every site read with xexdis);
`--graphics_driver_native=false` lets the SDK answer again.

| Call | Callers | Answer |
|---|---|---|
| VdQueryVideoMode | D3D device set-up sub_82311640, display reset sub_8231B8D0 | the console's TV of section 6 (1280x720, 16:9, 60 Hz), shared code |
| VdQueryVideoFlags | scaler size sub_8231A2E0 | 3 (widescreen, at least 1024 wide) |
| VdGetCurrentDisplayInformation | sub_82311978, sub_8231E1F0, sub_8231D4C0 | the same TV, laid out like the SDK's answer |
| VdGetCurrentDisplayGamma | gamma-ramp set-up sub_823136B8 | type 2 (BT.709), power 2.2222223: the SDK's values, so the ramp both renderers apply is unchanged |
| VdInitializeScalerCommandBuffer | sub_8231BDF8 (buffer and word count are stack arguments 10-11, [r1+0x64] / [r1+0x6C]) | the buffer filled with GPU no-op packets, result = word count (the SDK's answer; the PC has no display scaler) |
| VdRetrainEDRAM | sub_8231C050, every frame | 0 (the second call site runs only after a non-zero first answer) |
| VdRetrainEDRAMWorker, VdSetDisplayMode, VdEnableDisableClockGating, VdCallGraphicsNotificationRoutines | D3D set-up | 0 |
| VdIsHSIOTrainingSucceeded, VdInitializeEngines | device creation sub_82311B70, engine start sub_82311A80 | 1 |
| VdShutdownEngines, VdSetSystemCommandBufferGpuIdentifierAddress | D3D set-up / shutdown | nothing |

One answer differs from before: D3D used to hear the SDK's `video_mode_*`
flags in VdQueryVideoMode, which a saved settings file could set to any size
and refresh rate (1920x1080 at 180 Hz was seen), while the game itself already
heard the fixed TV (section 6) and the virtual screen ticks at 60 Hz. D3D keeps
the refresh at device +0x5434 and its flip function sub_82310728 uses it to
tell how far into a vertical blank a flip arrives; it now hears 60 Hz, the
real tick rate.

Tests (copy of the saves, `--load_save=last`, 30 fps): Native Only, 70 s of
play: the 14 calls show 0 in the Xbox calls meter, only the 6 above remain,
0 errors. Emulated renderer, same save, with and without the switch: 30.0 fps
with the same 1% lows (29.3) in both, and a frozen frame's HUD region is
identical to the pixel (gamma unchanged; whole frame 0.08/255 from the idle
animation).

## 10. The graphics driver, step 2: the vertical blank

A TV redraws its picture 60 times a second; between two pictures comes the
VERTICAL BLANK. The console's graphics driver reports each one by calling an
interrupt callback the game registers with `VdSetGraphicsInterruptCallback`:
D3D's engine start sub_82311A80 registers 0x82308798 with the D3D device as
its argument (site 0x82311AEC), its shutdown sub_82311E58 clears it (site
0x82311F54, r3 = 0). The callback is called with a SOURCE:

| Source | Who raises it | What the game does |
|---|---|---|
| 0 | the vertical blank (SDK: its "GPU VSync" thread, MarkVblank) | vblank handler 0x82310628: the vblank count PDDI waits on (0x8258E460, via 0x82433380) and the flip queue sub_82310728 |
| 1 | the GPU command stream (PM4 INTERRUPT packet) | D3D's fences and callbacks (`cmplwi r3,1` at 0x823087A8) |

Source 0 is the original game's frame clock: at 30 fps PDDI waits for two of
them per frame (findings/07). `src/system/vblank.cpp` now delivers it: both
registration sites are answered (midasm hooks), and a "VBlank" thread of the
port's (an SDK XHostThread, so game code can run on it) calls the callback
with source 0 at 60 Hz on an absolute schedule (start + k / 60 s, sleep_until;
the SDK's thread polled every 1 ms). It runs the callback the way the SDK
did: through ExecuteInterrupt (global lock held, TLS pointer 0) on "CPU 2", so
it never overlaps a source-1 interrupt. A late tick is still delivered; more
than 4 behind restarts the schedule instead of firing a burst.
`guest_refresh_hz` (SDK patch 0010) still overrides the rate.

Source 1 still comes from the emulated GPU, which keeps running until its
command ring is replaced: the port passes the callback's address on to it.
SDK patch 0023 adds the GPU flag `guest_vblank_interrupt`; the port sets it to
false, so the SDK's vblank thread keeps counting vblanks for its own command
processor but no longer calls the game. `--vblank_native=false` brings the
SDK's ticks back.

Tests (copy of the saves, `--load_save=last`; "1% low" = the slowest 1% of
frames):

| Run | Result |
|---|---|
| Native Only, 30 fps cap, 75 s | 3,600 ticks in the minute, 0 late, 0 restarts; 30.0 fps, 1% low 29.5; 0 errors |
| Emulated, 30 fps, port's ticks / SDK's ticks | 30.0 fps both, 1% low 29.7 / 29.2; frozen frames identical (HUD 0.0, frame 0.07/255) |
| Native Only, 60 fps cap / uncapped | 60.0 fps / ~233 fps (unchanged) |
| Boot through the intro movies to the attract movie | movies play, 0 errors |

The meter shows 0 calls of `VdSetGraphicsInterruptCallback`.
