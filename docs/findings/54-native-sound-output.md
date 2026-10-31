# 54. Sound played by the port: output and XMA decoding, no fake Xbox

The game mixes its own sound. The last step, handing each mixed block to the
console, used to go through the SDK's emulated Xbox audio device. The port
now answers those calls itself and plays the blocks through SDL
(`src/audio/sound_out.*`, on by default; `--sound_out_native=false` = the
SDK's path, kept for comparisons). With this, the game's sound output makes
no calls to the emulated Xbox (the Xbox calls meter, findings/32, shows 0).

The XMA decoder, the emulated "sound chip" that unpacks the compressed
sounds, is replaced too (s.5): the game's sound makes no calls to the
emulated Xbox at all now.

## 1. How the game's sound reaches the speakers

Radical's audio engine plays every sound through Microsoft's XAudio library,
statically linked into `default.xex` (code around `0x82470000-0x82490000`).
XAudio mixes everything into one block at a time:

| | |
|---|---|
| block | 256 samples per channel = 5.33 ms |
| channels | 6 (5.1), XAudio's order: front left, front right, centre, LFE, back left, back right |
| rate | 48 kHz |
| samples | 32-bit floats, big-endian, **planar** (all 256 of channel 0, then channel 1, ...) |

XAudio's render-driver object (vtables `0x8201BCAC` / `0x8201BC90`) talks
to the console through kernel calls, each made from exactly one instruction:

| Call | Instruction | In | Registers |
|---|---|---|---|
| `XAudioRegisterRenderDriverClient` | `0x824817C4` | `sub_82481768` | r3 = {callback, argument}, r4 = where the handle goes |
| `XAudioSubmitRenderDriverFrame` | `0x824819F0` | `sub_824819A8` | r3 = handle, r4 = the block |
| `XAudioUnregisterRenderDriverClient` | `0x82481794` | `sub_82481768` (re-register) | r3 = handle |
| same | `0x8248190C` | `sub_82481890` (destructor) | r3 = handle |
| `XAudioGetVoiceCategoryVolumeChangeMask` | `0x8247F0C0` | `sub_8247F088` (before every block) | r3 = handle, r4 = mask out |
| `XAudioGetVoiceCategoryVolume` | `0x8247F0F0` | same, per set mask bit | r3 = category, r4 = float out |

The console calls the registered callback (`0x82485A50` in this game) every
time it wants another block; XAudio mixes and submits it. The callback gets
a pointer to its argument, not the argument itself.

The kernel exports are called directly from these instructions, so there's
no game function to override at the boundary: midasm hooks (manifest
"SOUND OUT") run our code right before each call and jump over it.

## 2. What the port does

* A guest thread of ours, **Sound Out**, runs the game's callback whenever
  the speakers have room for another block: a *credit*. It must be a guest
  thread because the callback is game code (`XHostThread`, as the SDK's
  "Audio Worker").
* The submit hook copies the block into a queue, already byte-swapped.
* SDL's device callback takes blocks from the queue, converts them for the
  device and gives a credit back. With no block ready it plays a block of
  silence: a **gap** (the log says how many per minute).
* 8 credits by default (`--sound_out_buffer_blocks`, 3-32) = up to 43 ms on
  the way, the SDK's own default.
* The category-volume questions are answered "nothing changed" and 1.0,
  like the SDK (consoles had dashboard volume categories; a PC has none).

The first version reproduces the SDK's output stage on purpose, so the
sound doesn't change in this step:

* The device is opened the same way: 5.1 is asked for first, and only a
  device that then reports 1-2 channels gets a stereo fold done by us (ITU
  weights, as the SDK). On a PipeWire "Analog Stereo" output the opened
  device reports 6 channels, so the 5.1 block goes out as is and the
  **system** folds it to stereo. Asking the default device before opening
  says 2, and doing our own fold there made the front channels about 6 dB
  louder: noticed in the first test run and changed back.
* Same clamp to -1..1.

## 3. Checks

* Same save, one minute each path, the mixed blocks dumped
  (`--debug_audio_dump`): equal length (31.1 s for the same stretch) and
  equal loudness per channel.
* A minute of play: 11,253 blocks played (real time is 11,250), 2-3 gaps,
  around the level load.
* Xbox calls meter: `XAudioRegister/Submit/Unregister`, the volume calls:
  0 (the SDK path: 187 submits and 187 volume questions per second).

## 4. Next: the XMA decoder

The compressed sounds (XMA, the 360's hardware format) are unpacked by the
SDK's emulation of the console's decoder chip, which runs on its own thread
with its own timing. The game reaches it only through the XMA helper library
(`0x8247D4D8-0x8247E540`, findings/04). Both earlier audio bugs were in that
emulation (findings/04: a one-block-late decoder hung the loading screen;
findings/19: 5.1 cutscene dialogue cut after 3.4 s).

Who uses the chip (2026-10-31, a temporary counter on the context
allocator `sub_8247D4F0` during a minute of play): **only Radical's own
sound objects**. Every context came from `sub_8234BE88` (call at
`0x8234C090`). XAudio's own XMA voice code (`sub_824894E8`, `sub_82489B90`,
`sub_8248AC28`) never ran: XAudio only mixes PCM. Radical's sound object
unpacks a sound into its own 8192-sample rings through the context
(findings/04 step 3: `0x8234C220` feeds input, `0x8234C398` pulls output,
`0x8234C580` fills N samples, `0x8234C740` seeks / skips the lead-in,
`0x8234C8A8` reads N bytes; destructor `sub_8234BC58`).

So the native cut is that object: its methods rewritten to decode the
sound's XMA data with a software decoder straight into the same rings, with
no context, no lock / kick registers and no chip emulation at all.

## 5. The XMA decoder: Radical's stream, decoded in software

`src/audio/xma_native.cpp` (glue to the game's object) +
`src/audio/xma_stream.{h,cpp}` (the decoder core: blocks in, PCM out, no
guest memory). On by default; `--xma_native=false` = the game's own code
and the SDK's chip emulation (read when each sound object is created).

The object, `audio::hal::hw::XmaDecoder` (vtable `0x8200A9E8`, a `Stream`
whose Read hands out 16-bit PCM, a filter over an `RsdFileStream`), decoded
with Ghidra:

| Method | Function | Does |
|---|---|---|
| constructor | `sub_8234BE88` | reads the 12-byte size header + XMA header H + seek table from the inner stream; allocates 2 input buffers (physical), the rings, **creates the chip context** at `0x8234C090` |
| destructor | `sub_8234BC58` | frees rings, input buffers, releases the context (`sub_8247D6F0`) |
| Read (slot 6) | `sub_8234C8A8` | seek if pending, frames = bytes / (2 x channels), clamped to the sound's end; fill |
| fill | `sub_8234C580` | loop: pull, feed, copy each stream's ring into the caller's buffer (stream k at channel `2k`), else busy-wait 50 us |
| feed | `sub_8234C220` | when the context has a free input buffer: read the next block (inner Read) and hand it to the context |
| pull | `sub_8234C398` | copy what the chip decoded into the rings |
| seek | `sub_8234C740` | seek table (`sub_8234BDC8`): block + frames into it; reset the context, move the inner stream, read and discard those frames |

Object fields: `+0x04` inner stream (slot 5 Seek, slot 6 Read), `+0x08` seek
table (u32 per block = sample count at its end), `+0x14` H, `+0x1C` data
size, `+0x24` data offset, `+0x28` input buffers, `+0x30` next buffer,
`+0x38` bytes fed, `+0x3C` rings (0x18 per stream: buffer of 8192 frames of
big-endian int16, frames written, write position, frames read, read
position), `+0x40` position, `+0x44` total channels, `+0x48` seek pending.
H: `+1` streams, `+4` lead-in frames (384), `+8` end frame, `+0x0C` rate,
`+0x18` block size (64 KB), `+0x24` seek table entries, `+0x28 + 4k`
channels of stream k. The `.rsd` file has the same 12-byte sizes + H + table
at `0x800`.

What changed: the constructor no longer creates a context (midasm hook
`CrashMomXmaNoContext`, jumping over the call); feed does nothing (pull reads
blocks itself through the inner stream exactly when a stream needs one);
pull decodes with FFmpeg's single-frame XMA decoder (the one the SDK's chip
emulation uses) straight into the rings; seek restarts our decoder at the
block and drops the frames before the target. Read and fill stay the game's.

The format, as the core walks it: 64 KB blocks of 2048-byte packets; each
block starts every stream afresh, stream k's first packet is packet k, a
packet's skip count says how many packets of other streams follow; a
stream's payloads form a bit stream of frames (15-bit length, the frame,
its last bit = "another frame starts in this packet", else the next frame
starts at the next packet's first-frame offset); 512 samples per frame,
the decoder's output 192 samples ahead of the stream's numbering
(realigned with a carry; 192 samples of silence at the very end, as the
SDK). The decoder needs the file's exact rate (e.g. 47,994 Hz for a 48 kHz
cutscene track): rounding it to the chip's four rates changed 12 of 97
sounds.

Checks:

* Every sound object's output captured (a temporary hook on Read, one file
  per object) with our decoder and with the SDK's chip, same save: 171 of
  171 sounds bit-identical (incl. 3-minute music streams); a second save
  with a cutscene: 182 of 182. Later runs pick random sound variants
  (another grunt of the same length), so runs are compared as sets: the
  pairs that differ are different sounds (correlation ~0).
* 5.1 cutscene tracks (3 streams; no test save reaches one quickly):
  `tools/xma_test` decodes an `.rsd` with the core outside the game; against
  the system FFmpeg's full XMA2 decoder (the file's data wrapped in an
  `XMA2WAVEFORMATEX` RIFF), two cutscene tracks, all 3 streams, whole
  length: the same sound, 1-4 units apart at most on a 32,767 scale (float
  rounding between FFmpeg versions), aligned by the 384-frame lead-in.
* 3 minutes of play: no errors, no stalls; the Xbox calls meter shows 0
  `XMACreateContext` / `XMAReleaseContext` (the SDK path: ~170 a minute).

```bash
cmake --build --preset linux-amd64-relwithdebinfo --target xma_test
out/build/linux-amd64-relwithdebinfo/xma_test <file.rsd> <scratch>/out   # -> out_s<k>.raw
```

Game data (extracted `.rsd` files, decoded audio) stays out of the repo.

## 6. A latent bug in the original: writing past a ring's end

The game's pull (`sub_8234C398`) takes the room in a ring as 8192 frames
whenever the ring is **empty**, wherever its write position is. With the
write position at 7000 and 2000 frames ready on the chip, it copies frames
7000-9000: about 800 frames past the end of the ring's heap block, and the
reader later finds stale samples at 0-800. It needs an empty ring and more
ready frames than are left before the ring's end; in the A/B runs above it
never happened (the SDK's outputs match ours exactly, and ours can't do it).
On the console the chip's small output buffer made it rarer still.

It is a candidate for the audio-heap damage seen once in a playtest (a crash
freeing a sound object's buffers right after a music change: the next heap
chunk's header overwritten), not a proven cause. The port's pull counts the
free space up to the ring's end, so the overwrite can't happen any more.
