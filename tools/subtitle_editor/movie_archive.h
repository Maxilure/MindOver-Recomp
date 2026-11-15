// =============================================================================
// tools/subtitle_editor/movie_archive.h -- the story cartoons' sound
// =============================================================================
//
// The story cartoons are Bink 1 movies (.bik) inside the player's own
// game/movie1.rcf and game/movie2.rcf (findings/63 s.3.1). Each has 14 audio
// tracks (findings/49 s.5): the music and effects in surround (tracks 0, 2
// and 3: front, LFE, back) and one DIALOGUE track per language: 1 English,
// 4 French, 5 Danish, 6 Dutch, 7 German, 8 Italian, 9 Finnish, 10 Norwegian,
// 11 Spanish, 12 Swedish, 13 Russian (English speech on the discs seen).
// The game plays the voice language's dialogue track with the surround
// ones; the subtitles follow the movie's own clock (current frame / frame
// rate), so a cartoon's times are seconds from its first frame, as here.
//
// BINK 1 (as FFmpeg's libavformat/bink.c reads it; little-endian)
//   +0 "BIK" + revision letter, +4 file size - 8, +8 frames, +12 largest
//   frame, +16 (frames again), +20 width, +24 height, +28 / +32 frame rate
//   (2997 / 100), +36 video flags, +40 audio track count N; revision 'k'
//   has 4 more bytes here; then N x u32 largest decoded size, N x (u16 rate,
//   u16 flags: 0x2000 stereo, 0x1000 DCT else RDFT), N x u32 track IDs, then
//   one u32 per frame = its offset in the file (bit 0 = key frame).
//   A frame: per track u32 size + that many bytes of audio (a packet starts
//   with its decoded size in bytes), then the video.
// The audio is decoded by FFmpeg's Bink audio decoder (binkaudio.c, built
// with the editor: the SDK's FFmpeg leaves it out; its helpers are there).
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "voice_archive.h"

// FFmpeg's types (the decoders live in movie_archive.cpp).
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace subtitle_editor {

class MovieArchive {
 public:
  // Lists the .bik movies of <game>/movie*.rcf (names and lengths). Returns
  // how many were found.
  size_t Open(const std::filesystem::path& game_dir);
  // Each movie as a Clip: `movie` = its key (file name without ".bik", as
  // the text files write it), `name` = archive + file, family kCartoon,
  // seconds() = frames / frame rate (rate = the frame rate's numerator,
  // end = frames x its denominator).
  const std::vector<Clip>& clips() const { return clips_; }

  // Decodes the movie's sound into its parts: the dialogue track for
  // `voice` (english, french...) and the music and effects tracks (front,
  // bass, back), as stereo float from the first frame.
  bool Decode(const Clip& clip, const std::string& voice, Stems* out, std::string* error) const;

  // The dialogue track of a voice language (findings/49 s.5), -1 if none.
  static int DialogueTrack(const std::string& voice);

  const std::filesystem::path& ArchiveOf(const Clip& clip) const { return archives_[clip.archive]; }

 private:
  std::vector<std::filesystem::path> archives_;
  std::vector<Clip> clips_;
};

// THE PICTURE (watching a cartoon with its subtitles, main.cpp's preview):
// one movie's video frames, decoded by FFmpeg's Bink video decoder (built
// with the editor, ffmpeg/), as RGBA. Frames are decoded in order; a jump
// (a click elsewhere, playing from the cursor) starts again at the key frame
// before it (index bit 0 = key frame) and decodes up to the frame asked for.
class MovieVideo {
 public:
  MovieVideo();
  ~MovieVideo();
  MovieVideo(const MovieVideo&) = delete;
  MovieVideo& operator=(const MovieVideo&) = delete;

  bool Open(const std::filesystem::path& archive, const Clip& clip, std::string* error);
  void Close();
  bool open() const { return ctx_ != nullptr; }
  int width() const { return width_; }
  int height() const { return height_; }
  uint32_t frames() const { return uint32_t(index_.size()); }
  double fps() const { return fps_; }

  // The picture of frame `n` (0-based) as RGBA rows (width x height x 4),
  // or nullptr if it can't be decoded. Unchanged until the next call.
  const uint8_t* Frame(uint32_t n);
  uint32_t current() const { return current_; }

 private:
  bool DecodeNext();  // the frame after current_ (or the first)

  std::ifstream in_;
  uint32_t base_ = 0;  // the .bik's first byte in the archive
  uint32_t tracks_ = 0;
  std::vector<uint32_t> index_;    // per frame: offset in the .bik
  std::vector<uint8_t> keyframe_;  // per frame: 1 = a key frame
  uint32_t file_size_ = 0;
  double fps_ = 30;
  int width_ = 0, height_ = 0;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  uint32_t current_ = UINT32_MAX;  // the frame in rgba_, UINT32_MAX = none
  uint32_t next_ = 0;              // the frame the decoder reads next
  std::vector<uint8_t> rgba_;
};

}  // namespace subtitle_editor
