// =============================================================================
// tools/subtitle_editor/movie_archive.cpp -- see movie_archive.h
// =============================================================================
#include "movie_archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"
// FFmpeg 4.4's Bink audio decoders (binkaudio.c, compiled with the editor:
// not in the SDK's codec list, so used by pointer, not looked up by name).
extern AVCodec ff_binkaudio_rdft_decoder;
extern AVCodec ff_binkaudio_dct_decoder;
// FFmpeg 4.4's Bink video decoder (bink.c, also built with the editor).
extern AVCodec ff_bink_decoder;
#include "libavutil/frame.h"
}

namespace subtitle_editor {
namespace {

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A Bink file's header (movie_archive.h).
struct BinkHeader {
  uint32_t tag = 0, frames = 0, fps_num = 0, fps_den = 0, tracks = 0;
  std::vector<uint16_t> rate, flags;
  std::vector<uint32_t> ids;
  std::vector<uint32_t> index;  // per frame: offset in the .bik (bit 0 cleared)
  std::vector<uint8_t> keyframe;  // per frame: bit 0 of its index entry
  uint32_t file_size = 0;
  uint32_t video_flags = 0;  // +36: the video decoder's "extradata"
  uint32_t width = 0, height = 0;
};

bool ReadHeader(std::ifstream& in, uint32_t base, BinkHeader* h, bool with_index) {
  uint8_t b[48] = {};
  in.seekg(base);
  if (!in.read(reinterpret_cast<char*>(b), 44) || std::memcmp(b, "BIK", 3) != 0) return false;
  h->tag = Le32(b);
  h->file_size = Le32(b + 4) + 8;
  h->frames = Le32(b + 8);
  h->fps_num = Le32(b + 28);
  h->fps_den = Le32(b + 32);
  h->tracks = Le32(b + 40);
  h->width = Le32(b + 20);
  h->height = Le32(b + 24);
  h->video_flags = Le32(b + 36);
  if (h->frames == 0 || h->frames > 1000000 || h->tracks > 256 || !h->fps_num || !h->fps_den) return false;
  if (b[3] == 'k') in.seekg(4, std::ios::cur);  // revision k: one more field
  std::vector<uint8_t> t(size_t(h->tracks) * 12);
  in.read(reinterpret_cast<char*>(t.data()), std::streamsize(t.size()));
  for (uint32_t i = 0; i < h->tracks; ++i) {
    h->rate.push_back(Le16(&t[h->tracks * 4 + i * 4]));
    h->flags.push_back(Le16(&t[h->tracks * 4 + i * 4 + 2]));
    h->ids.push_back(Le32(&t[h->tracks * 8 + i * 4]));
  }
  if (!with_index) return bool(in);
  std::vector<uint8_t> idx(size_t(h->frames) * 4);
  in.read(reinterpret_cast<char*>(idx.data()), std::streamsize(idx.size()));
  if (!in) return false;
  h->index.resize(h->frames);
  h->keyframe.resize(h->frames);
  for (uint32_t f = 0; f < h->frames; ++f) {
    const uint32_t v = Le32(&idx[f * 4]);
    h->index[f] = v & ~1u;
    h->keyframe[f] = uint8_t(v & 1u);
  }
  return true;
}

// One audio track being decoded to stereo float.
struct TrackDecoder {
  AVCodecContext* ctx = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  std::vector<float> stereo;
  int channels = 1;
  ~TrackDecoder() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
  }
  bool Open(const BinkHeader& h, uint32_t track) {
    const bool dct = h.flags[track] & 0x1000;
    channels = (h.flags[track] & 0x2000) ? 2 : 1;
    ctx = avcodec_alloc_context3(dct ? &ff_binkaudio_dct_decoder : &ff_binkaudio_rdft_decoder);
    if (!ctx) return false;
    ctx->sample_rate = h.rate[track];
    ctx->channels = channels;
    ctx->channel_layout = channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
    // The decoder reads the file's tag (its revision) from the extradata.
    ctx->extradata = static_cast<uint8_t*>(av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE));
    ctx->extradata_size = 4;
    std::memcpy(ctx->extradata, &h.tag, 4);  // little-endian like the file (x86 / ARM hosts)
    if (avcodec_open2(ctx, ctx->codec, nullptr) < 0) return false;
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    return packet && frame;
  }
  void Feed(const uint8_t* data, size_t size) {
    if (av_new_packet(packet, int(size)) < 0) return;
    std::memcpy(packet->data, data, size);
    if (avcodec_send_packet(ctx, packet) < 0) {
      av_packet_unref(packet);
      return;
    }
    av_packet_unref(packet);
    while (avcodec_receive_frame(ctx, frame) == 0) {
      const int n = frame->nb_samples;
      const bool planar = av_sample_fmt_is_planar(AVSampleFormat(frame->format));
      for (int i = 0; i < n; ++i) {
        float l, r;
        if (planar) {
          l = reinterpret_cast<const float*>(frame->extended_data[0])[i];
          r = channels > 1 ? reinterpret_cast<const float*>(frame->extended_data[1])[i] : l;
        } else {
          const float* f = reinterpret_cast<const float*>(frame->extended_data[0]);
          l = f[i * channels];
          r = channels > 1 ? f[i * channels + 1] : l;
        }
        stereo.push_back(l);
        stereo.push_back(r);
      }
      av_frame_unref(frame);
    }
  }
};

}  // namespace

int MovieArchive::DialogueTrack(const std::string& voice) {
  static const std::pair<const char*, int> kTracks[] = {
      {"english", 1}, {"french", 4},   {"danish", 5},    {"dutch", 6},    {"german", 7},  {"italian", 8},
      {"finnish", 9}, {"norwegian", 10}, {"spanish", 11}, {"swedish", 12}, {"russian", 13}};
  for (const auto& [name, track] : kTracks) {
    if (voice == name) return track;
  }
  return -1;
}

size_t MovieArchive::Open(const std::filesystem::path& game_dir) {
  archives_.clear();
  clips_.clear();
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(game_dir, ec)) {
    const std::string stem = Lower(e.path().stem().string());
    if (Lower(e.path().extension().string()) == ".rcf" && stem.starts_with("movie")) archives_.push_back(e.path());
  }
  std::sort(archives_.begin(), archives_.end());
  for (size_t a = 0; a < archives_.size(); ++a) {
    std::ifstream in(archives_[a], std::ios::binary);
    uint8_t header[0x3C] = {};
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) ||
        std::memcmp(header, "ATG CORE CEMENT LIBRARY", 23) != 0) {
      continue;
    }
    // The RCF list (voice_archive.h): offsets in data order = the names' order.
    const uint32_t dir_off = Be32(header + 0x24), names_off = Be32(header + 0x2C);
    const uint32_t names_size = Be32(header + 0x30), count = Be32(header + 0x38);
    std::vector<uint8_t> dir(size_t(count) * 12), names(names_size);
    in.seekg(dir_off);
    in.read(reinterpret_cast<char*>(dir.data()), std::streamsize(dir.size()));
    in.seekg(names_off);
    in.read(reinterpret_cast<char*>(names.data()), std::streamsize(names.size()));
    std::vector<std::pair<uint32_t, uint32_t>> entries(count);
    for (uint32_t i = 0; i < count; ++i) entries[i] = {Be32(&dir[i * 12 + 4]), Be32(&dir[i * 12 + 8])};
    std::sort(entries.begin(), entries.end());
    size_t p = 8;
    for (uint32_t i = 0; i < count && p + 16 <= names.size(); ++i) {
      p += 12;
      const uint32_t length = Le32(&names[p]);
      p += 4;
      if (length == 0 || p + length > names.size()) break;
      std::string name(reinterpret_cast<const char*>(&names[p]), length - 1);
      p += length + 3;
      std::string file = name.substr(name.find_last_of("\\/") + 1);
      if (Lower(file).size() < 5 || !Lower(file).ends_with(".bik")) continue;
      file.resize(file.size() - 4);
      BinkHeader h;
      if (!ReadHeader(in, entries[i].first, &h, false)) {
        in.clear();
        continue;
      }
      Clip c;
      c.movie = file;
      c.file = file;
      c.name = archives_[a].filename().string() + ": " + file + ".bik";
      c.speaker = "cartoon";
      c.family = Family::kCartoon;
      c.offset = entries[i].first;
      c.size = entries[i].second;
      c.archive = uint16_t(a);
      c.streams = h.tracks;
      c.rate = h.fps_num;               // seconds() = end / rate = frames x den / num
      c.end = h.frames * h.fps_den;
      clips_.push_back(std::move(c));
    }
  }
  std::sort(clips_.begin(), clips_.end(), [](const Clip& x, const Clip& y) { return Lower(x.movie) < Lower(y.movie); });
  return clips_.size();
}

bool MovieArchive::Decode(const Clip& clip, const std::string& voice, Stems* out,
                          std::string* error) const {
  if (clip.archive >= archives_.size()) return false;
  std::ifstream in(archives_[clip.archive], std::ios::binary);
  BinkHeader h;
  if (!ReadHeader(in, clip.offset, &h, true)) {
    *error = "can't read the movie's header";
    return false;
  }
  // Which tracks: the voice's dialogue (by ID, else by number) and the
  // music and effects: front (0), LFE (2), back (3).
  const auto track_of = [&](uint32_t want) -> int {
    for (uint32_t i = 0; i < h.tracks; ++i) {
      if (h.ids[i] == want) return int(i);
    }
    return want < h.tracks ? int(want) : -1;
  };
  const int dialogue = track_of(uint32_t(std::max(0, DialogueTrack(voice))));
  if (DialogueTrack(voice) < 0 || dialogue < 0) {
    *error = "this movie has no " + voice + " dialogue track";
    return false;
  }
  std::vector<int> wanted = {dialogue};
  for (uint32_t t : {0u, 2u, 3u}) {
    if (const int k = track_of(t); k >= 0 && k != dialogue) wanted.push_back(k);
  }
  std::vector<std::unique_ptr<TrackDecoder>> dec(h.tracks);
  for (int t : wanted) {
    dec[size_t(t)] = std::make_unique<TrackDecoder>();
    if (!dec[size_t(t)]->Open(h, uint32_t(t))) {
      *error = "the Bink audio decoder didn't open";
      return false;
    }
  }
  // Every frame: the tracks' packets one after the other (movie_archive.h).
  std::vector<uint8_t> buf;
  for (uint32_t f = 0; f < h.frames; ++f) {
    in.seekg(std::streamoff(clip.offset) + h.index[f]);
    for (uint32_t t = 0; t < h.tracks; ++t) {
      uint8_t sz[4];
      if (!in.read(reinterpret_cast<char*>(sz), 4)) break;
      const uint32_t size = Le32(sz);
      if (size > (1u << 24)) break;
      if (dec[t] && size >= 4) {
        buf.resize(size);
        in.read(reinterpret_cast<char*>(buf.data()), size);
        dec[t]->Feed(buf.data(), size);
      } else {
        in.seekg(size, std::ios::cur);
      }
    }
  }
  // The parts. Only the dialogue is on at first (the subtitles follow it);
  // the music and effects at the game's balance (back pair x 0.707).
  out->rate = h.rate[size_t(dialogue)];
  out->parts.clear();
  out->parts.push_back({"Dialogue (" + voice + ")", 1.0f, true, std::move(dec[size_t(dialogue)]->stereo)});
  const char* names[] = {"Front (music + effects)", "", "Bass (LFE)", "Back (music + effects)"};
  for (size_t w = 1; w < wanted.size(); ++w) {
    const int t = wanted[w];
    if (h.rate[size_t(t)] != out->rate) continue;  // never seen; left out rather than resampled
    const int number = h.ids[size_t(t)] < 4 ? int(h.ids[size_t(t)]) : t;
    out->parts.push_back({number < 4 ? names[number] : "Track " + std::to_string(t), number == 3 ? 0.707f : 1.0f,
                          false, std::move(dec[size_t(t)]->stereo)});
  }
  return true;
}

// -----------------------------------------------------------------------------
// MovieVideo
// -----------------------------------------------------------------------------

MovieVideo::MovieVideo() = default;
MovieVideo::~MovieVideo() { Close(); }

void MovieVideo::Close() {
  av_frame_free(&frame_);
  av_packet_free(&packet_);
  avcodec_free_context(&ctx_);
  in_.close();
  index_.clear();
  keyframe_.clear();
  current_ = UINT32_MAX;
  next_ = 0;
}

bool MovieVideo::Open(const std::filesystem::path& archive, const Clip& clip, std::string* error) {
  Close();
  in_.open(archive, std::ios::binary);
  BinkHeader h;
  if (!ReadHeader(in_, clip.offset, &h, true)) {
    *error = "can't read the movie's header";
    return false;
  }
  base_ = clip.offset;
  tracks_ = h.tracks;
  index_ = h.index;
  keyframe_ = h.keyframe;
  if (!keyframe_.empty()) keyframe_[0] = 1;  // the first frame always starts a picture
  file_size_ = h.file_size;
  fps_ = double(h.fps_num) / double(h.fps_den);
  width_ = int(h.width);
  height_ = int(h.height);
  ctx_ = avcodec_alloc_context3(&ff_bink_decoder);
  if (!ctx_) return false;
  // What the decoder reads from the container (FFmpeg's bink demuxer): the
  // file's tag (its version letter in the top byte) and the 4 flag bytes.
  ctx_->codec_tag = h.tag;
  ctx_->width = width_;
  ctx_->height = height_;
  ctx_->extradata = static_cast<uint8_t*>(av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE));
  ctx_->extradata_size = 4;
  std::memcpy(ctx_->extradata, &h.video_flags, 4);  // little-endian like the file
  if (avcodec_open2(ctx_, ctx_->codec, nullptr) < 0) {
    *error = "the Bink video decoder didn't open";
    Close();
    return false;
  }
  packet_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  rgba_.assign(size_t(width_) * height_ * 4, 0);
  return packet_ && frame_;
}

bool MovieVideo::DecodeNext() {
  if (next_ >= index_.size()) return false;
  const uint32_t f = next_++;
  const uint32_t start = index_[f];
  const uint32_t end = f + 1 < index_.size() ? index_[f + 1] : file_size_;
  in_.clear();
  in_.seekg(std::streamoff(base_) + start);
  // The audio packets first (each with its size), then the picture.
  uint32_t used = 0;
  for (uint32_t t = 0; t < tracks_; ++t) {
    uint8_t sz[4];
    if (!in_.read(reinterpret_cast<char*>(sz), 4)) return false;
    const uint32_t size = Le32(sz);
    used += 4 + size;
    in_.seekg(size, std::ios::cur);
  }
  if (end <= start + used) return false;
  const uint32_t size = end - start - used;
  if (av_new_packet(packet_, int(size)) < 0) return false;
  in_.read(reinterpret_cast<char*>(packet_->data), size);
  if (keyframe_[f]) packet_->flags |= AV_PKT_FLAG_KEY;
  const int sent = avcodec_send_packet(ctx_, packet_);
  av_packet_unref(packet_);
  if (sent < 0) return false;
  if (avcodec_receive_frame(ctx_, frame_) < 0) return false;
  // YUV 4:2:0 -> RGBA (BT.601, video range: the port's native renderer gets
  // its matrix from the game at run time; this is close enough to judge
  // subtitles against).
  const uint8_t* py = frame_->data[0];
  const uint8_t* pu = frame_->data[1];
  const uint8_t* pv = frame_->data[2];
  for (int y = 0; y < height_; ++y) {
    uint8_t* out = &rgba_[size_t(y) * width_ * 4];
    const uint8_t* ry = py + size_t(y) * frame_->linesize[0];
    const uint8_t* ru = pu + size_t(y / 2) * frame_->linesize[1];
    const uint8_t* rv = pv + size_t(y / 2) * frame_->linesize[2];
    for (int x = 0; x < width_; ++x) {
      const int c = (int(ry[x]) - 16) * 298;
      const int d = int(ru[x / 2]) - 128;
      const int e = int(rv[x / 2]) - 128;
      out[x * 4 + 0] = uint8_t(std::clamp((c + 409 * e + 128) >> 8, 0, 255));
      out[x * 4 + 1] = uint8_t(std::clamp((c - 100 * d - 208 * e + 128) >> 8, 0, 255));
      out[x * 4 + 2] = uint8_t(std::clamp((c + 516 * d + 128) >> 8, 0, 255));
      out[x * 4 + 3] = 255;
    }
  }
  av_frame_unref(frame_);
  current_ = f;
  return true;
}

const uint8_t* MovieVideo::Frame(uint32_t n) {
  if (!ctx_ || index_.empty()) return nullptr;
  n = std::min<uint32_t>(n, uint32_t(index_.size() - 1));
  if (n == current_) return rgba_.data();
  // Behind, or far ahead: start again at the key frame before n.
  if (current_ == UINT32_MAX || n < current_ || n > current_ + 90) {
    uint32_t k = n;
    while (k > 0 && !keyframe_[k]) --k;
    if (current_ == UINT32_MAX || n < current_ || k > current_ + 1) {
      avcodec_flush_buffers(ctx_);
      next_ = k;
    }
  }
  while (next_ <= n) {
    if (!DecodeNext()) return current_ == UINT32_MAX ? nullptr : rgba_.data();
  }
  return rgba_.data();
}

}  // namespace subtitle_editor
