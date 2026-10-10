// =============================================================================
// saves/pc_save_drive.h -- the game's saves as plain PC files (no Xbox storage)
// =============================================================================
//
// WHAT IT REPLACES. Radical's engine talks to storage through platform
// "drives" (core::Drive): XenonDrive for the disc, XenonSaveDrive for saves
// ("SAVEDRIVE:", findings/32). Requests from the save manager (open, read,
// write, delete...) queue up on Radical's drive thread, which calls the
// drive's methods one at a time. On the Xbox 360, XenonSaveDrive turned each
// of them into Xbox services: a storage device chosen in a system pop-up,
// a "content package" per save, mounted as a drive of its own (XamContent*),
// then ordinary file calls inside it. Under the port, ReXGlue emulated all of
// that with folders per profile and title (user/saves/<profile>/565507FA/...).
//
// This file is the drive a PC build of the engine would have had: the same
// methods (we override the recompiled ones by address), the same results,
// but the save is a plain file, user/saves/<game's name>.sav
// (saves/save_files.h), and there is no storage device, profile or content
// package anywhere. Everything above the drive (Radical's request system,
// the game's save manager, our save list) runs unchanged.
//
// THE METHODS (XenonSaveDrive's vtable 0x8200BBCC; r3 = the drive):
//   slot  9  CheckMedia()                    0x82369040  media info: free space
//   slot 11  Open(name, mode, write, meta, size, &slot, &size)  0x823690D8
//   slot 12  Close(slot)                     0x82368B98
//   slot 13  Commit(slot)                    0x82369558  (the save's size header)
//   slot 14  Read(slot, -, -, pos, buf, count, &read)     0x82369678
//   slot 15  Write(slot, -, -, pos, buf, count, &written, &size)  0x82369790
//   slot 19  Delete(name)                    0x823699D0
//   slot 20  FindFirst(pattern, entry, &find)            0x82368BE8
//   slot 21  FindNext(&find, entry)          0x82369890
//   slot 22  FindClose(&find)                0x82368890
// plus the constructor's notification listener (sub_823007D0: none on a PC).
// Kept as they are (no Xbox in them): slot 4/8 constants, slot 5 the size
// calculation, slot 7 SetDevice (stores the device number the selector gave).
// The device selector pop-up itself (XShowDeviceSelectorUI, sub_823002D8,
// called once at boot by the save handler) is answered here too: "device 1,
// done", no pop-up, no Xbox service.
//
// THE FILE holds exactly what the Xbox's content file held (save_files.h):
// a u32 total-size header written by this drive, then the game's data. Open
// checks the header (a mismatch = the game's "corrupt save"), Commit writes
// it for a new file, Read/Write positions are counted after it. Writes go
// to memory and reach the disk at Commit and Close, through a temporary file
// renamed over the old one: a crash mid-save leaves the previous save whole.
// When the game deletes a save (it does before every overwrite), the old file
// is moved into user/saves/Backups/ instead (save_files::MoveToBackups).
//
// ERRORS go where Radical's code reads them: the drive's +208 ("last error",
// in Radical's numbers, not Win32's: 0 none, 1 not found, 6 already exists,
// 7 no space, 8 failure, 12 corrupt; table at 0x82368958) and +124 (media
// info), with the method returning 0.
//
// No switch back to the emulated drive: the save list (save_files.h) only
// knows the flat layout now, so the two would disagree about which saves
// exist. The overrides are the whole interface (nothing to call from C++).
// =============================================================================
#pragma once
