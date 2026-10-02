# 24. Saves: how the game stores, lists and loads its three slots

2026-10-02. Research for a save library: more than three saves, names,
deleting ([enhancements](../05-enhancements.md#more-saves-with-names)).
Nothing is changed in the game yet.

Short version: the game's save code (`CSaveGameManager`) thinks in exactly
**three slots, numbered 0-2**. That number is built into the Load / Save
screen's layout, its code and the compiled front-end logic. But a slot only
becomes a **file** in one tiny function, which formats
`"CrashMOM GameSlot %d"` with `slot + 1`. Every time the Load Game screen
opens, the game re-reads the three files' first 104 bytes into a small
table, and the screen draws from that table. So the three slots can show
*any* three save files: change which files sit behind slots 0-2, ask for a
re-read, and the game's own screen shows them. The save's name ("the name
typed at New Game") is plain UTF-16 text near the start of the file, with
no checksum anywhere in the file.

## 1. On disk

The runtime keeps each save as Xbox "content" (type 1, saved game) under
the user data root:

```
<profile id>/565507FA/00000001/CrashMOM GameSlot 1/CrashMOM GameSlot 1   the save (20,205 bytes)
<profile id>/565507FA/Headers/00000001/CrashMOM GameSlot 1.header       XCONTENT_DATA (328 bytes)
```

The `.header` is the runtime's record of the content: device id, content
type, a 128-character UTF-16 **display name** (what the Xbox dashboard
would list) and the file name. The game builds the display name itself
when it saves: `"Name: <name> Slot: <n>  Last Save: <m>.<d>.<yy>  Time:
<hh>:<mm>"`.

The save file:

| Offset (file) | Size | What |
|---|---|---|
| 0 | 4 | total size, 0x4EED (20,205) |
| 4 | 20,174 | the game's save block (the manager's buffer, below) |
| 20,178 | 27 | 17 zero bytes + a 10-byte marker, **identical in every file** (not a checksum) |

The save block starts with the 104 bytes the Load Game screen shows
(offsets from the start of the block, file offset = block offset + 4):

| Block offset | Type | What | Example |
|---|---|---|---|
| +0, +4 | u32 | 0 | |
| +8 | u32 | format word, must equal the game's own value 0x4ECE (else the slot is listed as corrupt) | 0x4ECE |
| +12 | u16 year, u8 month, day, hour, minute, second | last save time | 2008-10-07 14:30:00 |
| +20 | 32 x UTF-16 (big-endian) | **name** typed at New Game | "CRASH" |
| +84 | s32 | probably the area picture (2 / 1 / -1 seen) | |
| +88 | float | play time in seconds | 3600.0 = 1:00 |
| +92 | s32 | completion percent | 25 |
| +96 | s32 | difficulty | 1 (shown as Tricky) |

No hash, CRC or signature of the data was found: SHA-1, MD5 and CRC-32 of
every plausible range appear nowhere in the file, and the tail is the same
in every save.

## 2. The code

### 2.1 CSaveGameManager

`CSaveGameManager` (vtable 0x82039C90) is `Managers.SaveGameManager` for the
scripts; the game object at `*(0x8259B190)` holds it at +80. Fields seen:

| Offset | What |
|---|---|
| +4 | save mode, script enum ESaveMode_NONE 0, BOOTUP 1, SAVE 2, AUTOSAVE 3, LOAD 4 |
| +8 | request state (7 begin access, 8-10 scanning the slots, 11 load, 12-14 save, 6 format) |
| +28 | the current game's save time; +100..+116 its percent, play time, difficulty (for SaveGame) |
| +120 | the **slot table**: 3 x 104 bytes (BeginAccess allocates 312 bytes) |
| +124 | the storage handler (one 112-byte request per device at +12, request state at +100) |
| +128 | the save buffer (20,174 bytes) |
| +136 / +137 | device / slot of the current request |
| +138 / +139 | device / slot the game was last loaded from or saved to |

Per-frame update = vtable slot 10, `sub_822594D0` (a switch on +8).

Its script interface is registered at 0x8225A848: 28 methods, among them

| Method | Address | Notes |
|---|---|---|
| BeginAccess / EndAccess | 0x8225ACD0 (-> 0x82259648) / 0x822596D8 | allocate / free the table and buffer |
| SelectCard | 0x82259758 | starts the **scan** of slots 0, 1, 2 |
| LoadGame | 0x82259858 | full read of one slot (state 11) |
| SaveGame | 0x822598D8 | write one slot; an occupied slot is deleted first, then written |
| IsSlotEmpty | 0x8225ADA8 | from the table: empty name or format mismatch |
| ShowDeviceSelector, IsCardPresent, IsOperationDone, HasEnoughSpace, ... | | storage-device plumbing |

### 2.2 Where a slot becomes a file

`sub_82258DE8(handler, slot, out)`:
`snprintf(out, 32, "%s GameSlot %d", "CrashMOM", slot + 1)`. Its only
callers are the storage handler's three requests:

| Request | Function | Used by |
|---|---|---|
| open + read (r8 = 0: header only, 1: whole save) | `sub_822581B0` | scan, LoadGame |
| create + write (also builds the display name) | `sub_82258298` | SaveGame |
| delete | `sub_822584C0` | SaveGame, before overwriting an occupied slot |

The handler reaches the system through its device object's virtual
functions (+148 open, +44 delete, ...), which end in the runtime's
`XamContent*` services.

### 2.3 The scan

SelectCard (state 8, `sub_8225A2C8`) reads slot 0, 1, 2 one after the
other, header only, and copies the first 104 bytes of each block into the
table. A format word other than 0x4ECE clears the name and sets +8 to -1
(shown as `GameSlot_Corrupt`); a missing file leaves the entry empty
(`GameSlot_Empty`).

### 2.4 The screen

`FEGameSlotScreen` is a fight-tree action, `CGameSlotScreenAction` (vtable
0x820234B4). Its Enter (`sub_820CC188`) looks up the page elements
`Slot1_Art .. Slot3_Difficulty` (a loop of 3: Art, BG, Name, Date, Time,
Percent, Difficulty), then fills them from the slot table: name +20, date
+12, play time +88, percent +92, difficulty +96 (the strings
`GameSlot_Difficulty_Easy/Normal/Hard`). Slot -1 means "the game in
progress" and reads the manager's own fields instead. Enter releases the
element references it held before taking new ones.

The decisions are made by the **compiled** front-end fight tree (the 360
build runs C++ compiled from `fighttrees/frontend.lua`, like Crash's moves
in [findings/22](22-ground-contact-high-fps.md)): `GetMenuIndex`
(`sub_821239C8(this, "FE_GameSlot", "GameSlotMenu")`, the menu element's
signed byte at +152) is compared with 0, 1 and 2 in three separate
branches, each asking IsSlotEmpty and the save mode, then calling LoadGame
/ SaveGame or opening a confirmation prompt. The menus themselves are
fight-tree tracks too (`CMenuTrack`, `CMenuItemTrack`,
`CMenuCursorTrack`).

## 3. When the game reads the list (measured)

A gdb Python logger on the functions above (breakpoints that log and
continue), with the menus driven through `--debug_input_fifo` and the
script watching the screenshots for the main menu (fixed timings drift:
under gdb the boot takes 30-45 s instead of ~17 s):

```
34.2 BeginAccess / EndAccess pairs        "Load Game" picked
37.3 SelectCard(device 0)
37.3 read slot 0 header -> FileName(0)    ~50 ms per slot
37.3 read slot 1 header -> FileName(1)
37.4 read slot 2 header -> FileName(2)
39.3 SlotScreenEnter                       after the "Accessing ... save game files" message
54.5 EndAccess                             Back to the main menu
67.7 SelectCard, 3 reads, Enter again      Load Game opened a second time
```

Nothing reads saves at boot before the menu. Every visit to the screen
scans again, so a change of files on disk shows up the next time it opens.

## 4. What this means for more than three saves

"Three" is built into the page layout (elements Slot1-Slot3), the 312-byte
table, the screen's loops and the front-end branches. A longer list inside
the game's own screen would mean rewriting all of them.

Mapping slots to files is much smaller: only `sub_82258DE8` turns a slot
into a file name. Showing the files in **pages of three** (slot k on page p
= `CrashMOM GameSlot <3p + k + 1>`) keeps the game's three-slot world
untouched, makes the number of saves unlimited, and leaves today's files
where they are (page 1). Moving to another page = change p, re-scan
(SelectCard), refill the panels (the screen's Enter). Saving during play
writes to the slot the game was loaded from (+138/+139), on the same page.

The other wishes:

- **Rename**: the name is the UTF-16 text at block +20 (32 characters), with
  no checksum to update; the `.header` display name repeats it. The New
  Game screen's name entry is the game's own typing screen (section 5).
- **Delete**: the handler already has a delete request (`sub_822584C0`); the
  screen has no button for it.

## 5. The name entry screen (New Game)

The name typed at New Game comes from the game's own typing screen, not the
Xbox's system keyboard:

* **Page** `GameStart_NameEntry`: a `NameField`, a `Keyboard` panel of
  **4 rows x 11 keys** (elements `Key_<row>_<column>`), Backspace, Enter,
  Space, a caps key (`a/A`) and a key that cycles the character sets
  (Latin and two more), sounds `FE_SFX_NameScreen` (letter_scroll,
  letter_select, erase, done).
* **Code**: `CNameEntryScreenAction` (vtable 0x8202426C), Enter
  `sub_820D5488` (binds the elements, builds the key grid), Update
  `sub_820D58B8` (a switch on +216: 2 opening, 3 typing = `sub_820D5C08`,
  4/5 closing), Exit `sub_820D5958`. Cursor row +308, column +312, character
  set +316, caps bit in +320. Input comes as the game's menu events
  (`sub_822652B0`): four directions, select, Start = done, back = cancel.
* **The typed text** lives in ONE global UTF-16 buffer at **0x825A06F8**
  (`sub_820D5B70` returns it, `sub_820D5B80` clears it; the main menu's
  `CMainMenuScreenAction` clears it on New Game). Done needs at least one
  character.
* **The result** is a flag for the front-end tree: the byte at
  `*(0x8259B190)+52 -> +8593` gets bit 3 for done, bit 2 for cancel.
* **Where the name goes**: when a difficulty is picked, the script calls
  `SaveGameManager:ResetSaveFileHeader` (`sub_8225ACA0` ->
  `sub_82259408`): it clears the manager's current header (+16..+120),
  copies the buffer into its name (+36 = header +20, "DEFAULT" if there is
  none), resets the progress of every saved system (a new game) and forgets
  the last slot. The first save writes that header into the chosen slot.

Which screen follows which is **data**: `fighttrees/Frontend.bfig` in
`default.rcf` holds the front-end tree's states and their exits by name
(`NameEntryScreen`: ExitDone, ExitBack; `DifficultyScreen`: ExitEasy,
ExitNormal, ExitHard, ExitBack; `GameSlotScreen`: ExitSlot1-3,
ExitAvailableSlot, ExitLoad, ExitSlotNotEmpty, ExitSlotEmpty, ExitBack,
ExitBackToDifficulty, ExitBackToMainMenu, ExitStorageDevice, ...), and the
compiled code only decides which exit is taken. The New Game path is Main
menu -> NameEntryScreen -> DifficultyScreen (ResetSaveFileHeader) -> storage
checks -> GameSlotScreen (pick the slot for the new game) -> SaveGameScreen
-> the first level.

**For renaming:** the screen itself is reusable (fill the buffer with the
save's name before it opens, read it when it reports done). What's missing
is a path to it: from GameSlotScreen to NameEntryScreen and back, without
DifficultyScreen and its ResetSaveFileHeader. That means either an extra
exit in the tree data or steering the compiled exit decisions, which needs
the `.bfig` format decoded first (states, exits, their targets).

## 6. Tools

- Strings (`"%s GameSlot %d"`, `Slot%d_Name`, the script method names) ->
  code addresses with a `lis`/`addi` cross-reference search -> RTTI vtables
  (`tools/rtti_vtables.py '^(CSaveGame|CGameSlot)'`) -> callers
  (`tools/callgraph.py`).
- The call log: gdb Python breakpoints on `*__imp__sub_X` reading the guest
  registers from `((PPCContext*)$rdi)`, `stop()` returning False.
