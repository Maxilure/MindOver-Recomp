// =============================================================================
// tools/subtitle_editor/voice_archive.h -- the voice lines of one language
// =============================================================================
//
// Reads ONE voice archive of the player's own disc (game/english.rcf,
// game/french.rcf, ...): the list of its sounds (every .rsd file, keyed by
// the 64-bit sound ID in its name), each sound's designer name and length,
// and on request the sound itself, decoded to PCM with the port's own XMA
// decoder (src/audio/xma_stream.*, the same core the game uses).
//
// The same sound ID names the same line in every voice archive, so the
// editor can switch the voice language and keep the line selected. Lengths
// differ between languages (a French line is not as long as the English
// one): that is why the editor shows each language's own sound.
//
// FORMATS (findings/19 s.1, findings/54 s.5; tools/subtitles.py reads the same)
//   RCF 2.1 archive, big-endian, magic "ATG CORE CEMENT LIBRARY":
//     0x24: u32 directory offset, directory size, names offset, names size,
//           (0), file count
//     directory: per file u32 name hash, data offset, size (sorted by HASH)
//     names: 8 bytes, then per file 12 bytes, a LITTLE-endian u32 length, the
//           name (length bytes incl. its NUL), 3 bytes of padding; listed in
//           DATA order, so sorting the directory by offset pairs them up.
//     A name "sound\rsd\english\8\8150b53fbe2b48d3.rsd": 16 hex digits = ID.
//   RSD6 sound file:
//     +0x00 "RSD6", "XMA ", channels, bits, rate (little-endian), "****",
//     +0x18 the designer's .wav path (NUL-terminated);
//     +0x800 three big-endian u32: header size, seek table size, data size;
//     +0x80C the XMA header H: H+1 streams, H+4 lead-in samples, H+8 end
//     sample, H+0x0C rate, H+0x18 block size, H+0x28+4k channels of stream k.
//     Then the seek table, then the XMA2 blocks.
//
// Nothing here is game data: it is read from the player's disc at run time.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace subtitle_editor {

// The groups of findings/63 s.13.1 (a guess from folder + file names).
enum class Family { kCutscene, kAkuAku, kTutorial, kStory, kLoudspeaker, kCredits, kCombat, kCartoon };
const char* FamilyName(Family f);
// The groups the game's subtitles cover (findings/63 s.11): all but combat
// barks and the voiced credits.
bool InScope(Family f);

struct Clip {
  uint64_t id = 0;
  uint32_t offset = 0;      // the .rsd file's first byte in the archive
  uint32_t size = 0;
  std::string name;         // designer path without "in_game_art\sound\sounds\"
  std::string speaker;      // the folder after character\<language>\ ("akuaku")
  std::string file;         // the file name without ".wav"
  Family family = Family::kCombat;
  uint32_t streams = 0;
  uint32_t rate = 0;        // Hz (the file's own, e.g. 47994)
  uint32_t lead_in = 0;     // samples the game skips at the start (H+4)
  uint32_t end = 0;         // the end sample (H+8)
  // A story cartoon (movie_archive.h) instead of a voice line: its key in
  // the text files ("Crash_02_51"; id = 0) and which movie archive holds it.
  std::string movie;
  uint16_t archive = 0;
  double seconds() const { return rate ? double(end > lead_in ? end - lead_in : end) / rate : 0; }
};

// A decoded sound: stereo float at the clip's rate (5.1 tracks folded down,
// mono doubled), from the first sample the game plays (lead-in dropped).
struct Pcm {
  uint32_t rate = 0;
  std::vector<float> stereo;  // L R L R ...
  size_t frames() const { return stereo.size() / 2; }
};

// A sound in its PARTS, for the editor's mixer (each turned on / off and
// made louder or quieter while listening): a voice line = one part; the
// in-engine 5.1 cutscene tracks = front, centre (the dialogue), bass (LFE),
// back; a cartoon (movie_archive.h) = its dialogue track and the music and
// effects tracks (front, bass, back: music and effects are ONE mix there).
struct Stem {
  std::string name;          // "Dialogue", "Front (music + effects)" ...
  float balance = 1.0f;      // its level in the game's own mix (x the mixer's)
  bool on = true;            // on at first (the mixer remembers the player's choice by name)
  std::vector<float> stereo; // L R L R ..., from the first sample the game plays
};
struct Stems {
  uint32_t rate = 0;
  std::vector<Stem> parts;
};

class VoiceArchive {
 public:
  // Reads the list and every sound's header (~8,000 small reads; run it on a
  // worker thread: `progress` counts the clips read so far, `total` is set
  // first). False if the file is not an RCF archive.
  bool Open(const std::filesystem::path& file, std::atomic<int>* progress = nullptr,
            std::atomic<int>* total = nullptr);
  const std::filesystem::path& file() const { return file_; }
  const std::vector<Clip>& clips() const { return clips_; }
  const Clip* Find(uint64_t id) const;

  // Decodes one clip into its parts (a few ms for a line, ~0.1 s for a
  // long cutscene track).
  bool Decode(const Clip& clip, Stems* out, std::string* error) const;

 private:
  std::filesystem::path file_;
  std::vector<Clip> clips_;  // sorted by name
};

// The voice archives in <game>/ (every .rcf but default and the movies),
// English first, then by name.
std::vector<std::filesystem::path> VoiceArchives(const std::filesystem::path& game_dir);

}  // namespace subtitle_editor
