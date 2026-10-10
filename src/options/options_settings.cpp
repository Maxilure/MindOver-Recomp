// =============================================================================
// options/options_settings.cpp -- see options_settings.h
// =============================================================================
#include "options_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include <fmt/format.h>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "../frame_rate.h"
#include "../game_folder.h"
#include "../guest_memory.h"
#include "../ui/scrooby.h"

REXCVAR_DECLARE(int32_t, fps_cap);
REXCVAR_DECLARE(bool, native_only);
REXCVAR_DECLARE(bool, coop_camera);
REXCVAR_DECLARE(bool, coop_camera_near_focus);

// The game's own options (the original Options screen's code, CMenuAction
// sub_820D2A00 / sub_820D2F80, traced 2026-10-10). Volumes are 0.0-1.0 in
// steps of 0.2 (one star); the getters / setters ignore r3.
extern "C" REX_FUNC(__imp__sub_82266240);  // Dialogue volume: get (f1)
extern "C" REX_FUNC(__imp__sub_82266268);  // Dialogue volume: set (f1)
extern "C" REX_FUNC(__imp__sub_822661F0);  // Music volume: get
extern "C" REX_FUNC(__imp__sub_82266218);  // Music volume: set
extern "C" REX_FUNC(__imp__sub_82266290);  // SFX volume: get
extern "C" REX_FUNC(__imp__sub_822662B8);  // SFX volume: set
extern "C" REX_FUNC(__imp__sub_822615D0);  // front end: Music row selected (its preview plays)
extern "C" REX_FUNC(__imp__sub_822615E8);  // front end: Music row left (preview stops)
extern "C" REX_FUNC(__imp__sub_82266130);  // front end: a player's slot (Invert Axis byte index)
extern "C" REX_FUNC(__imp__sub_82261800);  // front end: play a sound (bank, name, f1-f3)

namespace options_settings {
namespace {

using scrooby::Call;

constexpr uint32_t kGameGlobal = 0x8259B190;  // the game (front end at +52)
// Front end +8588 + slot: a player's Invert Axis (1 = on), read by the aiming
// reticle (CReticleAimingBehaviour sub_82255160 flips the vertical aim).
constexpr uint32_t kInvertAxis = 8588;
// The volume rows' own sounds (the original screen plays them on a change):
// bank, name.
constexpr uint32_t kSoundDialogueBank = 0x82023AFC;  // "FE_SFX_Volume_D"
constexpr uint32_t kSoundDialogue = 0x82023AF0;      // "Dialogue"
constexpr uint32_t kSoundSfxBank = 0x82023B10;       // "FE_SFX_Volume_S"
constexpr uint32_t kSoundSfx = 0x82023B0C;           // "SFX"

Hooks g_hooks;
std::mutex g_changed_mutex;
std::set<std::string> g_changed;  // port flags changed since the last save

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t FrontEnd(const uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  return game ? Read32(base, game + 52) : 0;
}

// Sets a port flag by name (like the F4 settings: the same parsing and
// checks), remembered for SaveChanged.
void SetFlag(const char* name, const std::string& value) {
  if (!rex::cvar::SetFlagByName(name, value)) {
    REXLOG_WARN("Options: couldn't set {} = {}", name, value);
    return;
  }
  REXLOG_INFO("Options: {} = {}", name, value);
  std::lock_guard lock(g_changed_mutex);
  g_changed.insert(name);
}

// A row for an on / off port flag: values[0] = on, unless `off_first`
// (plain Off / On rows read better that way round).
Row OnOffRow(std::u16string label, const char* flag, std::u16string on, std::u16string off,
             std::u16string on_text, std::u16string off_text, bool off_first = false) {
  Row r;
  r.label = std::move(label);
  const int on_index = off_first ? 1 : 0;
  r.values = [on, off, off_first](Env&) {
    return off_first ? std::vector<std::u16string>{off, on} : std::vector<std::u16string>{on, off};
  };
  r.get = [flag, on_index](Env&) { return rex::cvar::Query<bool>(flag) ? on_index : 1 - on_index; };
  r.set = [flag, on_index](Env&, int i) { SetFlag(flag, i == on_index ? "true" : "false"); };
  r.describe = [on_text, off_text, on_index](Env&, int i) { return i == on_index ? on_text : off_text; };
  return r;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

// The caps offered (0 = no limit); a cap set by hand elsewhere joins the list.
constexpr int kCaps[] = {30, 60, 120, 144, 165, 180, 240, 0};

std::vector<int> CapList() {
  std::vector<int> caps(std::begin(kCaps), std::end(kCaps));
  const int now = REXCVAR_GET(fps_cap);
  if (std::find(caps.begin(), caps.end(), now) == caps.end()) {
    caps.insert(std::find_if(caps.begin(), caps.end(), [&](int c) { return c == 0 || c > now; }),
                now);
  }
  return caps;
}

std::u16string Utf16(const std::string& s) { return std::u16string(s.begin(), s.end()); }

Row FrameRateRow() {
  Row r;
  r.label = u"Frame-rate Cap";
  r.values = [](Env&) {
    std::vector<std::u16string> out;
    for (int c : CapList()) {
      out.push_back(c == 30 ? u"30 (original)" : c == 0 ? u"Unlimited" : Utf16(std::to_string(c)));
    }
    return out;
  };
  r.get = [](Env&) {
    const auto caps = CapList();
    return int(std::find(caps.begin(), caps.end(), REXCVAR_GET(fps_cap)) - caps.begin());
  };
  r.set = [](Env&, int i) {
    const auto caps = CapList();
    if (i >= 0 && i < int(caps.size())) SetFlag("fps_cap", std::to_string(caps[size_t(i)]));
  };
  r.describe = [](Env&, int) {
    return std::u16string(u"The most pictures a second the game draws. It was made for 30.");
  };
  r.warn = [](Env&, int i) {
    const auto caps = CapList();
    const int c = i >= 0 && i < int(caps.size()) ? caps[size_t(i)] : 30;
    return c == 0 || c > 60 ? std::u16string(u"Above 60, jumps and timings may feel different.")
                            : std::u16string();
  };
  return r;
}

// Four steps from "only the Xbox imitation" to "only ours". Emulated Only
// (the --emulated_only flag) keeps the native renderer idle and F9 / F8
// locked; it's read whenever it's asked, so it switches live too.
enum Renderer { kEmulatedOnly, kEmulated, kNative, kNativeOnly };

Row RendererRow() {
  Row r;
  r.label = u"Renderer";
  // Without a native renderer (not Vulkan) there is one value.
  auto none = [] { return !g_hooks.native_shown; };
  r.values = [none](Env&) {
    if (none()) return std::vector<std::u16string>{u"Emulated"};
    return std::vector<std::u16string>{u"Emulated Only", u"Emulated", u"Native", u"Native Only"};
  };
  r.get = [none](Env&) {
    if (none()) return 0;
    if (g_hooks.emulated_only && g_hooks.emulated_only()) return int(kEmulatedOnly);
    if (!g_hooks.native_shown()) return int(kEmulated);
    return int(REXCVAR_GET(native_only) ? kNativeOnly : kNative);
  };
  r.set = [none](Env&, int i) {
    if (none()) return;
    // The flags first: showing a picture also turns the emulated GPU's
    // drawing on / off by them (NativeRenderer::UpdateEmulatedDrawing).
    SetFlag("emulated_only", i == kEmulatedOnly ? "true" : "false");
    SetFlag("native_only", i == kNativeOnly ? "true" : "false");
    g_hooks.show_native(i == kNative || i == kNativeOnly);
    if (g_hooks.emulated_drawing) g_hooks.emulated_drawing();
  };
  r.describe = [none](Env&, int i) -> std::u16string {
    if (none() || i == kEmulated) {
      return u"The Xbox 360's graphics chip, imitated step by step: looks like the original.";
    }
    switch (i) {
      case kEmulatedOnly: return u"Only the Xbox 360 imitation: the port's renderer stays off (F9 too).";
      case kNative: return u"The port's own renderer, made for PC. The imitation keeps drawing beside it.";
      default: return u"The port's own renderer alone: the lightest on your PC.";
    }
  };
  r.warn = [none](Env&, int i) {
    return !none() && (i == kNative || i == kNativeOnly)
               ? std::u16string(u"Still being finished: an effect may look wrong.")
               : std::u16string();
  };
  return r;
}

// Renderer's X: DUAL MODE (= F8): a second window with the native picture
// next to the emulated one in the main window. Offered on Emulated and
// Native (both renderers draw there; Native Only turns the emulated one off,
// Emulated Only the native one).
void AddDualMode(Row& r) {
  r.x_label = u"Dual Mode";
  r.x_shown = [](Env&, int i) {
    return g_hooks.toggle_dual && g_hooks.native_shown && (i == kEmulated || i == kNative);
  };
  r.x_action = [](Env&) { g_hooks.toggle_dual(); };
}

// BRIGHTNESS (SDK patch 0015): 11 steps, a power of 2^(-step / 10) on the
// game's display gamma ramp (+5 = 0.71: shadows brighter; -5 = 1.41). The
// guide is the original's calibration grey scale (its screen said to turn the
// TV's brightness until the darkest squares can just be told apart).
constexpr int kBrightnessSteps = 5;

int BrightnessStep() {
  const std::string value = rex::cvar::GetFlagByName("gamma_ramp_power");
  const double power = value.empty() ? 1.0 : std::strtod(value.c_str(), nullptr);
  if (!(power > 0.0)) return 0;
  return std::clamp(int(std::lround(-10.0 * std::log2(power))), -kBrightnessSteps, kBrightnessSteps);
}

Row BrightnessRow() {
  Row r;
  r.label = u"Brightness";
  r.guide = true;
  r.values = [](Env&) {
    std::vector<std::u16string> out;
    for (int step = -kBrightnessSteps; step <= kBrightnessSteps; ++step) {
      out.push_back(step == 0   ? std::u16string(u"0 (original)")
                    : step > 0  ? u"+" + Utf16(std::to_string(step))
                                : Utf16(std::to_string(step)));
    }
    return out;
  };
  r.get = [](Env&) { return BrightnessStep() + kBrightnessSteps; };
  r.set = [](Env&, int i) {
    const int step = i - kBrightnessSteps;
    SetFlag("gamma_ramp_power", step == 0 ? "1.0" : fmt::format("{:.4f}", std::exp2(-step / 10.0)));
  };
  r.describe = [](Env&, int) {
    return std::u16string(u"Raise it until the darkest squares above can just be told apart.");
  };
  return r;
}

// V-SYNC (frame_rate.h): pictures wait for the monitor's refresh (present
// mode FIFO) and the game runs at most at its refresh rate. The swapchain
// picks its mode when it's made: made again right away (Hooks::refresh_present).
Row VsyncRow() {
  Row r = OnOffRow(u"V-sync", "present_vsync", u"On", u"Off",
                   u"Matches your monitor: whole pictures only, no tearing, and the game runs at "
                   u"most at its refresh rate.",
                   u"Pictures show the moment they're ready: the least delay, but the picture can "
                   u"tear.",
                   /*off_first=*/true);
  const auto set = r.set;
  r.set = [set](Env& e, int i) {
    set(e, i);
    frame_rate::ApplyVsyncPresentMode();
    if (g_hooks.refresh_present) g_hooks.refresh_present();
  };
  return r;
}

// FPS COUNTER (fps_overlay.h): the port's own, in three styles.
Row FpsCounterRow() {
  Row r;
  r.label = u"FPS Counter";
  r.values = [](Env&) {
    return std::vector<std::u16string>{u"Off", u"Simple", u"Average", u"Detailed"};
  };
  r.get = [](Env&) { return std::clamp(int(rex::cvar::Query<int32_t>("fps_overlay")), 0, 3); };
  r.set = [](Env&, int i) { SetFlag("fps_overlay", std::to_string(i)); };
  r.describe = [](Env&, int i) -> std::u16string {
    switch (i) {
      case 0: return u"No frame-rate counter.";
      case 1: return u"The frame rate, at the top of the screen.";
      case 2: return u"The frame rate, its average and its slowest 1 % (the 1% low).";
      default: return u"Also how long a frame takes, and how much of it the game works or waits.";
    }
  };
  return r;
}

Row FpsPositionRow() {
  Row r;
  r.label = u"FPS Position";
  r.values = [](Env&) {
    return std::vector<std::u16string>{u"Top Left", u"Top Centre", u"Top Right", u"Bottom Left",
                                       u"Bottom Right"};
  };
  r.get = [](Env&) {
    return std::clamp(int(rex::cvar::Query<int32_t>("fps_overlay_position")), 0, 4);
  };
  r.set = [](Env&, int i) { SetFlag("fps_overlay_position", std::to_string(i)); };
  r.describe = [](Env&, int) {
    return std::u16string(u"Where the frame-rate counter sits on the screen.");
  };
  return r;
}

Row FullscreenRow() {
  // The SDK's own flag: its change callback resizes the window right away.
  return OnOffRow(u"Fullscreen", "fullscreen", u"On", u"Off", u"Fills the whole screen.",
                  u"Plays in a window you can move and resize.", /*off_first=*/true);
}

// ---------------------------------------------------------------------------
// Audio: the game's own volumes
// ---------------------------------------------------------------------------

struct Volume {
  scrooby::GuestFunction get, set;
  uint32_t sound_bank, sound;  // 0 = no sound of its own
};

int Stars(Env& e, const Volume& v) {
  const PPCContext saved = e.ctx;
  v.get(e.ctx, e.base);
  const double level = e.ctx.f1.f64;
  e.ctx = saved;
  return std::clamp(int(level * 5.0 + 0.5), 0, 5);  // as the original: x5, rounded
}

void SetStars(Env& e, const Volume& v, int stars) {
  scrooby::CallF(v.set, e.ctx, e.base, 0, stars * 0.2);
  if (!v.sound_bank) return;
  const uint32_t fe = FrontEnd(e.base);
  if (!fe) return;
  const PPCContext saved = e.ctx;
  e.ctx.r3.u64 = fe;
  e.ctx.r4.u64 = v.sound_bank;
  e.ctx.r5.u64 = v.sound;
  e.ctx.f1.f64 = 1.0;
  e.ctx.f2.f64 = 0.0;
  e.ctx.f3.f64 = 1.0;
  __imp__sub_82261800(e.ctx, e.base);
  e.ctx = saved;
}

Row VolumeRow(std::u16string label, Volume v, std::u16string text, bool music) {
  Row r;
  r.label = std::move(label);
  r.kind = Kind::kStars;
  r.get = [v](Env& e) { return Stars(e, v); };
  r.set = [v](Env& e, int stars) { SetStars(e, v, stars); };
  r.describe = [text](Env&, int) { return text; };
  if (music) {
    r.focus = [](Env& e, bool on) {
      if (const uint32_t fe = FrontEnd(e.base)) {
        Call(on ? __imp__sub_822615D0 : __imp__sub_822615E8, e.ctx, e.base, fe);
      }
    };
  }
  return r;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

uint32_t InvertAxisByte(Env& e) {
  const uint32_t fe = FrontEnd(e.base);
  if (!fe) return 0;
  const uint32_t slot = Call(__imp__sub_82266130, e.ctx, e.base, fe, uint32_t(e.player));
  return fe + kInvertAxis + slot;
}

Row InvertAxisRow() {
  Row r;
  r.label = u"Invert Axis";
  r.values = [](Env&) { return std::vector<std::u16string>{u"Off", u"On"}; };
  r.get = [](Env& e) {
    const uint32_t a = InvertAxisByte(e);
    return a && *GuestPtr(e.base, a) ? 1 : 0;
  };
  r.set = [](Env& e, int i) {
    if (const uint32_t a = InvertAxisByte(e)) *GuestPtr(e.base, a) = uint8_t(i == 1);
  };
  r.describe = [](Env&, int i) {
    return i == 1 ? std::u16string(u"Up and down are flipped when you aim the reticle.")
                  : std::u16string(u"Aiming the reticle: up is up.");
  };
  return r;
}

Row RebindRow() {
  Row r;
  r.label = u"Rebind Keys";
  r.kind = Kind::kAction;
  r.describe = [](Env&, int) {
    return std::u16string(u"Press ± to choose the keyboard and mouse keys (F6).");
  };
  r.activate = [](Env&) {
    if (g_hooks.open_controls) g_hooks.open_controls();
  };
  return r;
}

// ---------------------------------------------------------------------------
// The tabs
// ---------------------------------------------------------------------------

std::vector<Tab> Build() {
  std::vector<Tab> tabs;
  Row renderer = RendererRow();
  AddDualMode(renderer);
  tabs.push_back(
      {u"Display",
       {FrameRateRow(), renderer, BrightnessRow(), VsyncRow(), FullscreenRow(), FpsCounterRow(),
        FpsPositionRow()}});
  tabs.push_back(
      {u"Audio",
       {VolumeRow(u"Dialogue", {__imp__sub_82266240, __imp__sub_82266268, kSoundDialogueBank, kSoundDialogue},
                  u"How loud the characters talk.", false),
        VolumeRow(u"Music", {__imp__sub_822661F0, __imp__sub_82266218, 0, 0},
                  u"How loud the music plays.", true),
        VolumeRow(u"SFX", {__imp__sub_82266290, __imp__sub_822662B8, kSoundSfxBank, kSoundSfx},
                  u"How loud hits, crates, footsteps and the rest are.", false)}});
  tabs.push_back({u"Controls", {InvertAxisRow(), RebindRow()}});
  tabs.push_back(
      {u"Camera",
       {OnOffRow(u"Co-op Camera", "coop_camera", u"Frame Everyone", u"Original",
                 u"The camera pulls back to keep every player on screen.",
                 u"The camera of the original game."),
        OnOffRow(u"Camera Follows", "coop_camera_near_focus", u"Nearest Player", u"Middle",
                 u"Front to back, the camera stays with the player nearest to it.",
                 u"The camera aims at the middle of the group.")}});
  return tabs;
}

// ---------------------------------------------------------------------------
// user/settings.toml: change only the keys we set
// ---------------------------------------------------------------------------

// The value as settings.toml writes it: strings in single quotes, the rest
// as is (the file's other writers, the launcher and F4, do the same).
std::string TomlValue(const rex::cvar::FlagEntry& flag, const std::string& value) {
  if (flag.type == rex::cvar::FlagType::String) return "'" + value + "'";
  return value;
}

}  // namespace

const std::vector<Tab>& Tabs() {
  static const std::vector<Tab> tabs = Build();
  return tabs;
}

void SetHooks(Hooks hooks) { g_hooks = std::move(hooks); }

void SaveChanged() {
  std::set<std::string> changed;
  {
    std::lock_guard lock(g_changed_mutex);
    changed.swap(g_changed);
  }
  if (changed.empty()) return;
  const std::filesystem::path path = game_folder::UserFolder() / "settings.toml";
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  for (const std::string& name : changed) {
    const rex::cvar::FlagEntry* flag = rex::cvar::GetFlagInfo(name);
    if (!flag) continue;
    const std::string value = rex::cvar::GetFlagByName(name);
    // The line of this key, if any ("name = ...", maybe spaced differently).
    auto it = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) {
      const size_t start = l.find_first_not_of(" \t");
      if (start == std::string::npos || l.compare(start, name.size(), name) != 0) return false;
      const size_t after = l.find_first_not_of(" \t", start + name.size());
      return after != std::string::npos && l[after] == '=';
    });
    if (value == flag->default_value) {
      if (it != lines.end()) lines.erase(it);  // back to its default: the file holds changes only
      continue;
    }
    const std::string line = name + " = " + TomlValue(*flag, value);
    if (it != lines.end()) {
      *it = line;
    } else {
      lines.push_back(line);
    }
  }
  // Temp file + rename: a crash mid-write never leaves half a file.
  const std::filesystem::path temp = path.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    for (const std::string& l : lines) out << l << '\n';
    if (!out) {
      REXLOG_WARN("Options: couldn't write {}", temp.string());
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    REXLOG_WARN("Options: couldn't save {}: {}", path.string(), ec.message());
    return;
  }
  REXLOG_INFO("Options: saved {} setting(s) to {}", changed.size(), path.string());
}

}  // namespace options_settings
