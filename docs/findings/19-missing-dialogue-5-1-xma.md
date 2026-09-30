# 19. The missing line: cutscene dialogue cut off after 3.4 seconds

2026-09-27. Found in playtesting: in the cutscene where a box of NVs
arrives at Crash's house (internal name `nvmail`), a line of dialogue was
missing; in the original game Coco says *"Oh my god, somebody sent us
NVs!"*. The log didn't mention audio at all, so the first job was to make
the game's sound visible. It turned out to be an XMA decoder bug in the
SDK, now fixed (SDK patch 0007). The same day we also measured what our
native renderer costs the game's main thread (section 6), the cause of a
second playtest finding: a lower frame rate in the fight against the
Ratnicians right after that scene.

## 1. How the game stores its sounds

* Every sound is an **`.rsd`** file: Radical's container, `"RSD6"`, a codec
  (`"XMA "` for all of them here), channels / bits / sample rate as
  little-endian numbers, then `"****"` and the **sound designer's original
  file name**, e.g.
  `in_game_art\sound\sounds\character\english\labrat1\battlecry_labrat1_09.wav`.
  The XMA data follows (2 KB packets).
* They live in the `.rcf` archives (RCF 2.1, big-endian, magic
  `ATG CORE CEMENT LIBRARY`): 8,320 voice clips in `english.rcf` (Coco,
  Crunch, Cortex, Aku Aku, the tutorial voice, and ~200 lines for each
  enemy variant: Bratgirls `bratgirl1..6`, Ratnicians `labrat1..6`, Znus
  `znu`, Slap-Es `slappe`, `monkey1..6` (name TBD)...), 1,162 other sounds
  in `default.rcf`.
* Inside an archive a sound is named after a **64-bit ID** in hex,
  `sound\rsd\english\8\8150b53fbe2b48d3.rsd`: the readable name is only in
  the file's header.
* The archive's name list is stored in **data order**, its directory in
  hash order: sorting the directory's offsets pairs them up. Checked on
  `default.rcf`, where every name's extension agrees with the data at its
  offset (1,162 `.rsd` = `RSD6`, 1,102 `.p3d` = P3D models, 239
  `.lua`/`.blua` = Lua bytecode).
* Side find for the roadmap's "script decryption": those 239 scripts in
  `default.rcf` are **not encrypted**. They are Lua 5.1 bytecode (`\x1bLuaQ`,
  with a slightly customized header) and keep their source file names. Only
  the one loose `script/ahoy.blua` on the disc looks encrypted.

## 2. A log of every sound the game asks for (`--debug_audio_trace`)

`src/audio_trace.cpp`. The game turns a sound ID into its `.rsd` path in
one function, **`sub_82336430`** (found through its `".rsd"` string at
`0x820093E8`): r5 points to the 8-byte ID, printed as two `%08x` words.
Its callers, from RTTI (`tools/rtti_vtables.py`) and the call graph:

| Caller | What |
|---|---|
| `0x82336598` (wrapper) | `CSoundResourceManager::ClipLoadThread` (`0x822DA8C0`) and `0x82348BC8` |
| `0x82336B20` (wrapper) | `audio::SeqEventClip` slot 1 (`0x823385E8`), `audio::SeqEventStream` slot 1 (`0x82348AF8`), `0x82338228`; then opens the file (`0x823411C8`) |

We wrap `sub_82336430` (like `frame_rate.cpp` wraps `sub_82310728`: the
generated `sub_X` is a weak alias) and log the ID's name, read from its
RSD header, the archives' file lists being indexed at start:

```
Audio: load character\english\nis\6ch\l1_r1_sc4_nvmail_ch*.wav (80736861cec2b2cb)
Audio: load character\english\labrat1\battlecry_labrat1_12.wav (8150b53fbe2c491b)
```

"load" (through `0x82336598`) turned out to cover both level loads and
sounds opened **as they play**: during the fight after the cutscene the
Ratnicians' lines appear one by one, the moment they're spoken. So the log
says which lines the game asked for, and when.

## 3. The scene's dialogue: one 5.1 file, all in the centre

Walking into the box logged the cutscene's
sound: `nis\6ch\l1_r1_sc4_nvmail_ch*.wav`, a **6-channel** sound, 11.04 s.
Its XMA header:

* **3 streams** of 2 channels each, with channel masks: stream 0 = front
  left/right, stream 1 = **centre + LFE**, stream 2 = rear left/right. That
  is how the Xbox 360 stores 5.1 XMA: one XMA context per stream.
* **64 KB blocks** (4 of them). The first ends at sample 161,792 =
  **3.37 s**.

Decoded straight from the disc with ffmpeg (the RSD wrapped as an XMA2
WAV), it's a pure dialogue track: everything is in the **centre channel**
(the front channels hold only a faint bleed ~60 dB lower, the rest is
silent). Speech from 0 to 6.3 s, a pause, speech again from 7.5 to 9.7 s.
Music and effects come from other sounds.

## 4. What the game actually sent to the speakers (`--debug_audio_dump`)

To compare, a recording of the game's own output: `--debug_audio_dump=<wav>`
writes everything the game mixes, as a 6-channel 48 kHz WAV, taken where
the XDK's audio thread hands each mixed frame (256 samples x 6 channels,
big-endian floats) to the kernel's `XAudioSubmitRenderDriverFrame`. That
export is called directly, so a **midasm hook** at `0x824819F0`
(`CrashMomAudioFrame`, r4 = the frame) copies it. With the trace on, every
log line also says where it is in the recording (`[dump 144.06 s]`).

The centre channel of the recording (the scene starting at 144.0 s):

| Seconds into the dialogue | Disc | Game output, before |
|---|---|---|
| 0.5 - 3.0 | speech (-8 to -20 dB) | speech, same shape |
| 3.4 - 9.7 | speech, a pause at 6.5-7.3 | **near-silence (-70 dB), then nothing** |

The sound stops **exactly where the first 64 KB block ends**. And the
-70 dB left in the centre channel matched the disc file's *front* channels.

## 5. The bug: each stream must continue at its own packet

The packet headers explain it. The three streams' packets are interleaved
in each block. Every packet's **skip count** says how many packets to jump
to reach that stream's next one, and every block starts with one packet
per stream, in stream order:

| Stream | Packets | Crossing into block 2 |
|---|---|---|
| 0 (front L/R) | 0, 5, 11, 17, 23, 27 | 27 + 5 = 32 = block 2, packet **0** |
| 1 (centre + LFE) | 1, 3, 4, 6, ... 28, 29, 30 | 30 + 3 = 33 = block 2, packet **1** |
| 2 (rear L/R) | 2, 7, 13, 20, 26, 31 | 31 + 3 = 34 = block 2, packet **2** |

The SDK's XMA decoder (the same code as Xenia's) restarted *every* stream at
the next buffer's packet 0. Right for stream 0, and for every mono or
stereo sound (their skip count is always 0). But streams 1 and 2 then
followed stream 0's packets: the centre channel played the front channels'
near-silence, and Coco's line was gone.

**SDK patch `0007-xma-multistream-buffer-carry`**: a skip that reaches past
the end of a buffer carries into the next one (packet index minus the
buffer's packet count), both when the decoder moves on
(`XmaContext::Decode`) and when a frame's tail is read from the next buffer
(`GetNextPacket`). A carry of 0 behaves as before.

After the fix, the same scene recorded again: the centre channel follows
the disc decode for all 11 seconds, pause included (loudness in 20 ms steps
correlates **1.000** before the block boundary, 1.000 after it, 0.999 over
the last line), about 2 dB quieter (the game's mix volume). No errors in
the log.

Every 5.1 sound in the game had this problem once longer than one block,
and all 35 of them belong to the in-engine cutscenes (`nis\6ch\`): 6
dialogue tracks in `english.rcf` and 29 effects / music tracks in
`default.rcf`. Everything else is mono (9,256) or stereo (191).

## 6. Where the frame time goes

The second playtest finding: in dual mode the game ran at 59.4 fps
on Wumpa Island, then **~50 fps (1% low 35-41)** in the fight against the
Ratnicians after this cutscene. A frame rate can't say who is slow, so the main thread is
now cut in three every frame (`pddi::FrameTiming`, three clock reads at
`xnDisplay::SwapBuffers`), logged with `--debug_log_fps`:

```
frame_rate: last 5.0 s, main thread per frame: game 12.0 ms, frame-end work 4.2 ms (worst 17.0), SwapBuffers (mostly waiting for the screen) 0.7 ms
NativeRenderer: last 300 frames, on the game's main thread: 4.19 ms per frame (waiting for the GPU 0.00, textures 3.50, mesh buffers 0.20, the rest 0.49), worst frame 17.0 ms
```

(A later playtest, native picture, Wumpa Island.) At 60 fps a
frame has 16.7 ms. The game needs 10.5-12 ms, and **our renderer 4-5 ms**,
almost all of it in the **texture cache**: every texture a frame uses is
re-hashed every frame, to notice the ones the game rewrites (texture_cache.h,
"WHEN IT RE-UPLOADS"). That leaves under 1 ms of slack. A busier scene
misses the refresh, and a missed frame takes two refreshes (33 ms). The
emulated picture alone doesn't pay this (nothing is recorded or drawn
natively then), and in dual mode both windows show the same slowed game.
Next step: make those checks cheaper (only textures that can change, or
check them off the main thread).

## 7. Using the tools

```bash
tools/play.sh --dual                               # the trace is on in play.sh logs
out/build/linux-amd64-relwithdebinfo/crash_mom ... --debug_audio_trace --debug_audio_dump=<tmp>/dump.wav
```

* `--debug_audio_trace`: `Audio: <kind> <name> (<ID>)` lines. `tools/play.sh`
  passes it, so a playtest log lists the lines the game asked for.
* `--debug_audio_dump=<file.wav>`: ~0.6 MB per second; the WAV header is
  refreshed about once a second, so the file stays readable when the game
  is closed or killed. Keep it in a temporary folder (game audio: never in the
  repo).
* Decoding a sound from the disc to compare: find it by name in the RSD
  headers, wrap the XMA data (after the `0x800` header plus the XMA2 block
  table) in a RIFF with an `XMA2WAVEFORMATEX` (`0x166`, streams, channel
  mask `0x3F` for 5.1), then `ffmpeg -i x.wav -c:a pcm_s16le out.wav`.
