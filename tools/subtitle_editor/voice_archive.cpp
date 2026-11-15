// =============================================================================
// tools/subtitle_editor/voice_archive.cpp -- see voice_archive.h
// =============================================================================
#include "voice_archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include "audio/xma_stream.h"

namespace subtitle_editor {
namespace {

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// "....\8150b53fbe2b48d3.rsd" -> the ID; false for any other file.
bool IdFromName(const std::string& name, uint64_t* id) {
  if (name.size() < 20) return false;
  if (Lower(name.substr(name.size() - 4)) != ".rsd") return false;
  const std::string hex = name.substr(name.size() - 20, 16);
  uint64_t v = 0;
  for (char c : hex) {
    const int d = c >= '0' && c <= '9' ? c - '0'
                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                         : -1;
    if (d < 0) return false;
    v = (v << 4) | uint64_t(d);
  }
  *id = v;
  return true;
}

// Speaker folder, file and group, by the SAME rule as the game's subtitles
// (src/subtitles/subtitles.cpp KindOf), so "in scope" here = shown in game.
void Classify(Clip& c) {
  std::vector<std::string> parts;
  size_t p = 0;
  for (size_t q; (q = c.name.find('\\', p)) != std::string::npos; p = q + 1) {
    parts.push_back(c.name.substr(p, q - p));
  }
  parts.push_back(c.name.substr(p));
  std::string file = parts.back();
  if (file.size() > 4 && Lower(file.substr(file.size() - 4)) == ".wav") file.resize(file.size() - 4);
  c.file = file;
  c.speaker = parts.size() >= 3 && parts[0] == "character" ? parts[2] : "";
  const std::string folder = Lower(c.speaker);
  const std::string low = Lower(file);
  if (folder == "nis") c.family = Family::kCutscene;
  else if (folder == "akuaku") c.family = Family::kAkuAku;
  else if (folder == "tutorial") c.family = Family::kTutorial;
  else if (folder == "credits") c.family = Family::kCredits;
  else if (low.starts_with("loudspeaker")) c.family = Family::kLoudspeaker;
  else if (low.starts_with("ingame")) c.family = Family::kStory;
  else c.family = Family::kCombat;
}

}  // namespace

const char* FamilyName(Family f) {
  switch (f) {
    case Family::kCutscene: return "cutscene";
    case Family::kAkuAku: return "Aku Aku";
    case Family::kTutorial: return "tutorial";
    case Family::kStory: return "story";
    case Family::kLoudspeaker: return "loudspeaker";
    case Family::kCredits: return "credits";
    case Family::kCombat: return "combat";
    case Family::kCartoon: return "cartoon";
  }
  return "?";
}

bool InScope(Family f) { return f != Family::kCombat && f != Family::kCredits; }

bool VoiceArchive::Open(const std::filesystem::path& file, std::atomic<int>* progress,
                        std::atomic<int>* total) {
  file_ = file;
  clips_.clear();
  std::ifstream in(file, std::ios::binary);
  uint8_t header[0x3C] = {};
  if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) ||
      std::memcmp(header, "ATG CORE CEMENT LIBRARY", 23) != 0) {
    return false;
  }
  const uint32_t dir_off = Be32(header + 0x24), names_off = Be32(header + 0x2C);
  const uint32_t names_size = Be32(header + 0x30), count = Be32(header + 0x38);
  std::vector<uint8_t> dir(size_t(count) * 12), names(names_size);
  in.seekg(dir_off);
  in.read(reinterpret_cast<char*>(dir.data()), std::streamsize(dir.size()));
  in.seekg(names_off);
  in.read(reinterpret_cast<char*>(names.data()), std::streamsize(names.size()));
  if (!in) return false;
  // (offset, size) in data order = the order of the names.
  std::vector<std::pair<uint32_t, uint32_t>> entries(count);
  for (uint32_t i = 0; i < count; ++i) entries[i] = {Be32(&dir[i * 12 + 4]), Be32(&dir[i * 12 + 8])};
  std::sort(entries.begin(), entries.end());
  if (total) *total = int(count);
  size_t p = 8;
  for (uint32_t i = 0; i < count && p + 16 <= names.size(); ++i) {
    p += 12;
    const uint32_t length = Le32(&names[p]);
    p += 4;
    if (p + length > names.size() || length == 0) break;
    const std::string name(reinterpret_cast<const char*>(&names[p]), length - 1);
    p += length + 3;
    if (progress) *progress = int(i + 1);
    Clip c;
    if (!IdFromName(name, &c.id)) continue;
    c.offset = entries[i].first;
    c.size = entries[i].second;
    // The .rsd's two headers: the designer name and the XMA header H.
    uint8_t rsd[0x830] = {};
    in.seekg(c.offset);
    in.read(reinterpret_cast<char*>(rsd), sizeof(rsd));
    if (in.gcount() < 0x830 || std::memcmp(rsd, "RSD6", 4) != 0) {
      in.clear();
      continue;
    }
    rsd[0x7FF] = 0;
    std::string designer(reinterpret_cast<const char*>(rsd + 0x18));
    constexpr std::string_view kPrefix = "in_game_art\\sound\\sounds\\";
    if (designer.starts_with(kPrefix)) designer.erase(0, kPrefix.size());
    c.name = designer;
    const uint8_t* h = rsd + 0x80C;
    c.streams = h[1];
    c.lead_in = Be32(h + 4);
    c.end = Be32(h + 8);
    c.rate = Be32(h + 0x0C);
    Classify(c);
    clips_.push_back(std::move(c));
  }
  std::sort(clips_.begin(), clips_.end(), [](const Clip& a, const Clip& b) { return a.name < b.name; });
  return true;
}

const Clip* VoiceArchive::Find(uint64_t id) const {
  for (const Clip& c : clips_) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

bool VoiceArchive::Decode(const Clip& clip, Stems* out, std::string* error) const {
  std::ifstream in(file_, std::ios::binary);
  std::vector<uint8_t> d(clip.size);
  in.seekg(clip.offset);
  if (!in.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size())) || d.size() < 0x830) {
    *error = "can't read the sound from the archive";
    return false;
  }
  const uint32_t hsize = Be32(&d[0x800]), tsize = Be32(&d[0x804]), dsize = Be32(&d[0x808]);
  const uint8_t* h = &d[0x80C];
  const uint32_t streams = std::min<uint32_t>(h[1], xma::Stream::kMaxStreams);
  const uint32_t rate = Be32(h + 0x0C), block = Be32(h + 0x18);
  int channels[xma::Stream::kMaxStreams] = {};
  for (uint32_t k = 0; k < streams; ++k) channels[k] = h[0x28 + 4 * k];
  size_t pos = 0x80C + size_t(hsize) + tsize;
  const size_t end = std::min(d.size(), pos + dsize);
  if (streams == 0 || block == 0 || pos >= end) {
    *error = "not an XMA sound the editor understands";
    return false;
  }
  xma::Stream x;
  if (!x.Init(streams, channels, rate, block, [&](std::vector<uint8_t>& b) {
        if (pos >= end) return false;
        const size_t n = std::min<size_t>(block, end - pos);
        b.assign(d.begin() + std::ptrdiff_t(pos), d.begin() + std::ptrdiff_t(pos + n));
        pos += n;
        return true;
      })) {
    *error = "decoder set-up failed";
    return false;
  }
  x.Restart(0, 0);
  // Every stream to the end, as float stereo.
  std::vector<std::vector<float>> per(streams);
  for (uint32_t k = 0; k < streams; ++k) {
    std::vector<int16_t> buf;
    while (true) {
      const uint32_t n = x.Ready(k, 4096);
      if (n == 0) break;
      buf.resize(size_t(n) * channels[k]);
      x.Take(k, buf.data(), n);
      for (uint32_t f = 0; f < n; ++f) {
        const float l = buf[size_t(f) * channels[k]] / 32768.0f;
        const float r = channels[k] > 1 ? buf[size_t(f) * channels[k] + 1] / 32768.0f : l;
        per[k].push_back(l);
        per[k].push_back(r);
      }
    }
  }
  // The parts: 1 stream = the line itself; 3 streams = the 5.1 cutscene
  // tracks (FL/FR, FC/LFE, BL/BR; findings/19): front, centre (the dialogue),
  // bass (the LFE: off at first, as the editor always left it out), back,
  // at the game's fold-down levels; anything else: one part per stream.
  const auto mono = [](const std::vector<float>& st, int side) {
    std::vector<float> out(st.size());
    for (size_t f = 0; f + 1 < st.size(); f += 2) out[f] = out[f + 1] = st[f + size_t(side)];
    return out;
  };
  out->rate = rate;
  out->parts.clear();
  if (streams == 1) {
    out->parts.push_back({"Voice", 1.0f, true, per[0]});
  } else if (streams == 3) {
    out->parts.push_back({"Front", 1.0f, true, per[0]});
    out->parts.push_back({"Centre (dialogue)", 0.707f, true, mono(per[1], 0)});
    out->parts.push_back({"Bass (LFE)", 0.707f, false, mono(per[1], 1)});
    out->parts.push_back({"Back", 0.707f, true, per[2]});
  } else {
    for (uint32_t k = 0; k < streams; ++k) {
      out->parts.push_back({"Stream " + std::to_string(k + 1), 0.707f, true, per[k]});
    }
  }
  // Time 0 = the first sample the game plays (its position counts after the
  // lead-in, findings/63 s.14), so cut the lead-in off; and stop at the end
  // sample (the last block is padded).
  for (Stem& part : out->parts) {
    const size_t frames = part.stereo.size() / 2;
    const size_t first = std::min<size_t>(clip.lead_in, frames);
    const size_t last = std::min<size_t>(std::max<size_t>(clip.end, first), frames);
    part.stereo = std::vector<float>(part.stereo.begin() + std::ptrdiff_t(first * 2),
                                     part.stereo.begin() + std::ptrdiff_t(last * 2));
  }
  if (x.errors()) *error = std::to_string(x.errors()) + " frame(s) the decoder refused";
  return true;
}

std::vector<std::filesystem::path> VoiceArchives(const std::filesystem::path& game_dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(game_dir, ec)) {
    const std::string stem = Lower(e.path().stem().string());
    if (Lower(e.path().extension().string()) != ".rcf") continue;
    if (stem == "default" || stem.starts_with("movie")) continue;
    out.push_back(e.path());
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    const bool ea = Lower(a.stem().string()) == "english", eb = Lower(b.stem().string()) == "english";
    if (ea != eb) return ea;
    return a.filename() < b.filename();
  });
  return out;
}

}  // namespace subtitle_editor
