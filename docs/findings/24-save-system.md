# 24. Saves: how the game stores, lists and loads its three slots

2026-10-02. Research for a save library: more than three saves, names,
deleting ([enhancements](../05-enhancements.md#more-saves-with-names)),
then the library itself (section 6).

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
(Built that way first; then replaced by one scrolling list with the same
mapping trick, section 6.)

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

## 6. The save library (built)

`src/saves/` (`--save_library`, on by default; `=false` = the game's own
three slots only). What it does is described for players in
[05-enhancements.md](../05-enhancements.md#more-saves-with-names); this is
how. (A first version showed the saves in pages of three, turned by moving
past the bottom or top slot; it was replaced by the list below. Its lesson
about the overwrite prompt, section 6.2, still holds.)

### 6.1 One list

* **The list**: every save (`CrashMOM GameSlot N` with any N), the most
  recently PLAYED first: the later of its file time (written by every save;
  a rename keeps it) and the last time it was loaded, kept in
  `<profile>/565507FA/save_library_played.txt`. When saving, "Create New
  Save" comes first; it writes the smallest free N.
* **A window of three**: the screen's three panels show list items top,
  top+1, top+2. `sub_82258DE8` (slot -> file name) is overridden: slot k
  becomes the file of item top+k ("Create New Save" = the free number, a
  hidden panel = number 0, "GameSlot 0", which never exists). The list is
  built when the screen's scan starts (SelectCard, `sub_82259758`).
* **Sliding**: the slot menu is a Scrooby Menu: its items in a vector at
  +112, the selected index (s8) at +152, bit 0x80 of +153 = the cursor
  wraps. Its MoveNext / MovePrevious (`sub_8237A668` / `sub_8237A550`) are
  wrapped: down on the bottom visible panel moves the window one item down
  instead of wrapping (the cursor stays, the next save appears under it),
  up on the top panel moves it back; the list stops at both ends. The new
  panels come straight from the files (the same 104 bytes the scan copies;
  a format word other than 0x4ECE = corrupt), then the screen's Enter runs
  again to redraw them.
* **Hidden panels**: with fewer than three items, the unused panels' texts
  and pictures are hidden (an element shows while bit 0x80 of +110 is set)
  and their menu items made unselectable. In wrap mode MoveNext /
  MovePrevious step to the next item whose +137 has bit 0x80 SET; with none
  they still return 1 (the move sound plays, the cursor stays). A first
  version had that bit backwards, cleared it on every shown panel, and the
  cursor never moved (found in the disassembly: `lbz 137` / `rlwinm 0,0,24`
  / `bne` = found).

### 6.2 Which file a load or save uses

The manager's request state (+8) at the moment the file name is built tells
the operations apart; the screen's "selected" flag tells a pick:

| Request (manager +8) | File |
|---|---|
| 8, 9: the scan of slots 0-2 | the window's files |
| 10: the scan's re-read of the game in progress's slot (+139) | the game's file |
| 11: LoadGame | the picked file |
| 12-14: SaveGame (14 = removing the old file first, 13 = writing) | the picked file, or the game's file |

* **A pick** is the front end's "selected" flag (bit 3 of
  `*(0x8259B190)+52 -> +8593`) turning on during the screen's Update; the
  item under the cursor at that moment is remembered. LoadGame
  (`sub_82259858`) and SaveGame (`sub_822598D8`) use it when its slot
  matches the call's slot, and that file becomes the game's file (the game
  in progress lives there).
* **Everything else saves to the game's file**: SaveGame calling itself to
  write after removing the old file, and AutoSaveGame (`sub_82259B70`,
  wrapped to mark the call), which saves straight into the last slot
  (+139). Despite the name, the game never saves on its own: no level
  script calls the save manager (only the front end's tree does), and the
  save totem's prompt (SavePrompt, node 833, decision 0x82121728) only ever
  returns 837 "choose a save slot" or 838 "done", never 836
  "ExitAutoSaveScreen" (the direct save, a path of other platforms).
  AutoSaveGameScreen (326) is simply the in-game "saving" screen after a
  slot was picked.
* **The overwrite prompt closes and reopens the screen.** Traced (log lines
  in the wrapped Enter / Exit / EndAccess): pick an occupied slot -> Exit ->
  Enter -> "Are you sure you wish to overwrite this save file?" -> No ->
  Exit -> Enter (the list again) -> ... So a pick must survive the screen's
  Exit and Enter; a first version forgot it at Enter, and "Yes" then wrote
  to the game in progress's file instead of the picked one.
* **The save session's end** is CSaveGameManager::EndAccess
  (`sub_822596D8`): the front end calls it when the Load / Save screens are
  left (and a few times before a session starts), never between a pick and
  its load or save. There an unused pick is forgotten, so a later save of
  "the last slot" can't land on a file that was only looked at.
* **New Game saves by itself.** The difficulty screen's decision
  (`sub_8211B8E8`) marks a new game when it returns Mild / Tricky / Bonkers
  (109-111); ReadingCard's decision (`sub_8211BE58`, node 153) would then
  open the list (159) or report no saves (155): it returns slot 1's
  "available slot" exit instead (185, whose script saves slot 0), with slot
  0 = "Create New Save" = a new file. The list never shows; the new save
  carries the typed name.

### 6.3 The screen's look

* **The cursor on opening**: the menu object stays loaded between visits
  and keeps its index, so it opened wherever the last visit left it (Load
  Game on panel 3, then the in-game Save list on panel 3 = some other
  save). On every new visit (a new scan) it is set (Scrooby Menu
  SetSelection `sub_8237A4C0`): on the game in progress's save when saving
  ("Create New Save" one up), on the most recently played save when
  loading.
* **Centred when short**: with one or two items the panels move to the
  middle of the screen (one = where panel 2 is, two = half a panel down).
  A Scrooby element's position is at +92 / +96 (floats, a fraction of the
  screen width; `sub_82370E68` reads them x 640); `sub_82370EE8(element,
  dx, dy)` moves an element by whole units of that 640-wide screen (a
  translation multiplied into its transform at +16). The panels are 121
  units apart (panel 1's background at y 327, panel 2's at 206: y points
  up); all seven elements of each panel (background, picture, five texts)
  move together, and are moved back when the screen closes (the page
  outlives the screen).
* **No fade on a slide**: the screen's Enter ends by restarting the
  screen's opening: the top 6 bits of +124 = the screen's state (2 opening,
  3 taking input, 4 closing, 5 picked: the Update's switch at 0x820CCA58),
  +112 = the opening's fade 0 -> 1, which the opening state (`sub_820CCCF0`)
  hands to the page's alpha (`sub_82370888`), and it clears the front end's
  "screen ready" bit (+8592 bit 0x40, set again once the fade ends). Every
  slide faded the whole page in again. A slide now puts all three back
  after its Enter (screenshots every 100 ms: the panels change in one
  frame, the brightness stays).
* **"Create New Save" previews the new save**: the empty slot's panel
  shows the game in progress as the new save would hold it. While the
  screen's Enter fills the panels, that panel's table entry gets a copy of
  the manager's own header (+16, the 104 bytes every save starts with),
  brought up to date the way SaveGame does right before writing: the date
  (`sub_8235AA10`), the current level (+84, which picks the location
  picture), the play time (+88, the live timer: a float at
  `*(game+96)+20`), the % (+92, `sub_822E10C8` of the same object), the
  difficulty (+96, `sub_82270608()+240`). Right after, the entry gets its
  old bytes back, so the game's own checks still see an empty slot (no
  overwrite question). The title "Create New Save" replaces the name; with
  no game in progress the panel shows only that title, where the game
  writes "Empty" (the Date element: Enter 0x820CC910 puts GameSlot_Empty in
  element 16 + slot).

### 6.4 The screen's prompts and buttons

![The Load Game screen with the new prompts, and the game's own name screen opened by X (demo names)](../images/save-library-rename.jpg)

* **Prompts**: they are text in the game's font (Titans_Small), whose
  private characters U+00A5-00BE draw the 360 buttons (findings/23
  section 1.2; U+00B3 = X, U+00B4 = Y), on the Scrooby page `FE_Buttons`:
  four slots, each a `...Button` (the glyph) and a `...Text` element. On
  this screen the game fills LowerLeft (Back), LowerRight (Select) and
  UpperRight ("Storage Device", the Xbox's storage device chooser); UpperLeft
  is empty. Now UpperRight reads "Rename X" and UpperLeft "Delete Y". Every
  front-end state starts a prompts action (vtable 0x82024A08, Enter
  0x820DFBD8) that sets all eight texts (empty first, then its own: the
  tree's FE_Button_Text_* parameters) after the screen's own Enter, so ours
  are set at the screen's first update (`sub_82373360` sets a text
  element's string; elements found with the front end's own page and
  element lookups, `sub_821241E8` and `sub_82371AD0`). They show while the
  cursor is on a save, not on "Create New Save" (cleared there, the game's
  "Storage Device" included). Leaving the screen empties both upper slots
  again.
* **Y** = delete: the list's decision functions end with "button 14 (Y)
  pressed -> Storage Device" (exits 235 / 236 on the menu's list, 444 in
  game); those exits are blocked, and Y asks the game's own question
  instead (6.5). **X** = rename with the game's name screen (6.5). On the
  keyboard: the keys bound to X / Y, plus **F2** (our rename box) and
  **Delete**.
* **Not over the overwrite prompt**: the prompt keeps the list running
  underneath (dimmed), but then the front end asks the prompt's exits, not
  the list's. The list's decision functions stamp the update they ran in;
  X / Y / slides only act when that stamp is current.

### 6.5 Renaming with the game's own name screen

The front end's screens and their order are the fight tree
`fighttrees/Frontend.bfig` (default.rcf): a Lua chunk, then the binary tree
("fig0"). Its node records (state 0x83A93633, exit 0xBC27359D, and others)
hold a name, the number of their PARENT node, and for exits the TARGET
state; nodes are numbered in file order. Each state's compiled decision
function (a table of 2,161 function pointers at 0x82503030, indexed by node
number; 0x821174B0 = none) returns the number of the exit to take, -1 to
stay. The engine follows the target of whatever exit comes back, child of
the current state or not. The nodes used:

| Node | What | Target |
|---|---|---|
| 93 | NameEntryScreen (decision 0x8211B7D8: 94 Done, 95 Back) | |
| 107 | DifficultyScreen (109-111 Mild / Tricky / Bonkers, 112 Back) | |
| 112 | DifficultyScreen's ExitBack (one transition animation) | 93 |
| 153, 159 | ReadingCard, its ExitHasValidSaveFiles (no actions) | 160 |
| 160 | GameSlotScreen, menu (decision 0x8211BF88) | |
| 384, 385 | in game: ReadingCard, its ExitOperationDone (no actions) | 386 |
| 386 | GameSlotScreen, in game (decision 0x8211DAC0) | |
| 170, 171 | slot 1's ExitSlotNotEmpty, OverWriteMessage (decision 0x8211C2D8: 175 No, 176 Yes) | 171 |
| 175, 176 | its ExitCancel, ExitOverWrite | 160, 289 |
| 394, 395 | in game: the same (decision 0x8211DD28: 399 No, 400 Yes) | 395 |
| 399, 400 | its ExitCancel, ExitOverWrite | 386, 326 |

X on a save: the list's decision returns 112 (the only way into the name
screen that doesn't start a new game: the difficulty screen's Easy / Normal
/ Hard exits call ResetSaveFileHeader), with the save's name written into
the name screen's text buffer (0x825A06F8, 32 UTF-16 characters = 64 bytes
up to the next global). The player edits it with the game's on-screen
keyboard. The name screen's decision is wrapped: Done or Back return 159
(or 385 when it came from the in-game list) instead of 94 / 95, Done after
renaming the save (6.6). The buffer's earlier content is put back
afterwards: it holds New Game's typed name until the difficulty screen
copies it, and Back from the slot list leads there.

**16 characters.** The screen's "type the selected key" (`sub_820D6618`)
refuses a character once the name has 9: `0x820D663C cmpwi r11,9` (r11 =
the length of the buffer), `0x820D6640 bge` -> the error sound. A midasm
hook right before that `bge` (`CrashMomNameEntryRoom`, `jump_address_on_true
0x820D6644`) lets it through up to 16, the rename box's limit (16 fits
beside a save's picture and in the screen's name field; New Game's copy of
the name, ResetSaveFileHeader `sub_82259408`, keeps 32).

**Where the screen exists.** If its page (`GameStart_NameEntry`) isn't
loaded, X opens our rename box instead (the jump would make the screen's
Enter look up elements of a missing page). The page is in the front end's
menu package, which the game keeps loaded in game only in level L0 (Crash's
house), so outside it X opens the box.

**Delete asks the game's own question.** Y on a save: the list's decision
returns 170 (394 in game), slot 1's "slot not empty" exit into the overwrite
question (OverWriteMessage: page `FE_TRG_Message_Frontend` / `_InGame`,
text ID `ConfirmOverwrite_`, the `ConfirmationMenu` No / Yes, No first). Its
`Message` text is replaced in the question's first frames ("Are you sure
you wish to delete this save file? It will be moved to the Deleted saves
folder."). Its decision is wrapped: No returns 175 (399), Yes moves the
save away (6.5) and returns 175 (399) too, back to the list, which is then
built again. The overwrite itself is never taken.

### 6.6 The rename box and deleting

F2 opens a small box (ImGui, like the Controls menu) to type a name for the
save under the cursor:

* **Rename**: up to 16 characters (letters, digits, space, `- _ . ! ? ' &`);
  16 still fits beside the slot's picture. Written (also by 6.5) into the
  save file (offset 24), the header's display name ("Name: ... Slot: ..."),
  the slot table entry on screen, and, when it's the game in progress's own
  save, the manager's current header (+36): the game writes that name with
  every save, so it would otherwise put the old one back.
* **Delete** (after the game's question, 6.5): the save's folder and its
  header move to `<profile>/565507FA/Deleted saves/CrashMOM GameSlot N (<date time>)/`,
  out of the game's sight, and the list is built again (the window stays
  where it was, as far as the shorter list allows).
* **Controller**: A confirms, B cancels. While the box is open the game
  sees a connected but untouched controller (its XInputGetState wrapper,
  `sub_824742F0`, called only by the input manager's poll, is wrapped), and
  keys and mouse are paused like for the Controls menu; both stay off until
  the buttons / keys that closed the box are let go (or the confirming A /
  Enter would arrive in the game and pick the slot).

### 6.7 Tests (on copies of profiles with 1, 3 and 5 saves)

Driven through `--debug_input_fifo`, with a script that waits for the main
menu on the screenshots (the five-save profile: copies of real saves,
renamed "Save A" to "Save E" and given file times in that order):

| Test | Result |
|---|---|
| Load Game, down x4, up x4 (5 saves) | A,B,C -> cursor to panel 3 -> B,C,D -> C,D,E; back the same way; no wrap at the ends |
| Slide, screenshots every 100 ms | the panels change in one frame; no fade (before: the whole page faded in again) |
| Y on the third save, Yes | moved to `Deleted saves/`; the list A,B,D with the cursor on D |
| X, one letter, Start (game's name screen) | "Save D2" in the file; back to the list, order kept (a rename keeps the file time) |
| X, 14 letters on a 6-letter name | stops at 16; fits the name field and the list |
| Load a save, then Load Game again | that save is first |
| Load Game, cursor to panel 3, B, Load Game | the cursor starts on panel 1 again |
| In game, pause -> Save Game | "Create New Save", then the game's save (cursor on it), ... |
| "Create New Save" | the preview: date, live play time (0:56 after two minutes from 0:54), %, difficulty, picture; picking it saves a new file with the same values, no overwrite question |
| The game's save: overwrite, No; then Yes | No keeps the cursor; Yes writes only that file (the others' times unchanged) |
| One save; delete down from 5 to 2 and 1 | panels 2-3 hidden, down / up don't move; two saves centred as a pair, one in the middle panel's place; reopening keeps it right |
| New Game | "Save successful" with the typed name, no list; the opening movie starts |

All runs: 0 errors in the log.

## 7. Tools

- Strings (`"%s GameSlot %d"`, `Slot%d_Name`, the script method names) ->
  code addresses with a `lis`/`addi` cross-reference search -> RTTI vtables
  (`tools/rtti_vtables.py '^(CSaveGame|CGameSlot)'`) -> callers
  (`tools/callgraph.py`).
- The call log: gdb Python breakpoints on `*__imp__sub_X` reading the guest
  registers from `((PPCContext*)$rdi)`, `stop()` returning False.
