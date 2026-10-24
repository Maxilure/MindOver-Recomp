// =============================================================================
// options/options_settings.cpp -- see options_settings.h
// =============================================================================
#include "options_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

#include "../app_restart.h"
#include "../frame_rate.h"
#include "../game_folder.h"
#include "../guest_memory.h"
#include "../input/bindings.h"
#include "../input/keyboard_mouse.h"
#include "../input/prompt_icons.h"
#include "../ui/scrooby.h"
#include "../ui/window_mode.h"
#include "../language/language.h"
#include "global_options.h"
#include "options_menu.h"

REXCVAR_DECLARE(int32_t, fps_cap);
REXCVAR_DECLARE(bool, native_only);
REXCVAR_DECLARE(bool, coop_camera);
REXCVAR_DECLARE(bool, coop_camera_near_focus);

// The game's own options (the original Options screen's code, CMenuAction
// sub_820D2A00 / sub_820D2F80, traced 2026-10-10). The volumes and Invert
// Axis are now kept in user/settings.toml (global_options.cpp sets them).
extern "C" REX_FUNC(__imp__sub_822615D0);  // front end: Music row selected (its preview plays)
extern "C" REX_FUNC(__imp__sub_822615E8);  // front end: Music row left (preview stops)
extern "C" REX_FUNC(__imp__sub_82266130);  // front end: a player's slot (Invert Axis byte index)
extern "C" REX_FUNC(__imp__sub_82261800);  // front end: play a sound (bank, name, f1-f3)

namespace native {
int MaxSurfaceSamples();  // native/render_targets.h (kept out of here: Vulkan headers)
}

namespace options_settings {
namespace {

using scrooby::Call;

constexpr uint32_t kGameGlobal = 0x8259B190;  // the game (front end at +52)
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
// A HEADING inside a tab (2026-10-24, the Options sort): a row that can't be
// selected, drawn in the titles' cyan by options_menu.cpp. It groups the rows
// under it ("Quality", "Effects", "Co-op"...).
Row Heading(std::u16string label) {
  Row heading;
  heading.label = std::move(label);
  heading.kind = Kind::kHeader;
  return heading;
}

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

// PICTURES IN THE CARDS (2026-10-15, playtest: "every description with
// button icons"). A card's text can show keys and controller buttons as
// pictures: characters of the font's CARD slots (prompt_icons.h SlotSize::
// kCard: drawn as large as a prompt picture although the card's text is
// small print). Without the slots (font patch missing) the names are written.
//   Key(action): the action's first key in the Keyboard Icons style ("" when
//                the keyboard is off or the action has no key);
//   KeyOf(input): one given key (F9, Esc...);
//   Pad(name):   a controller button / stick by its 360 name ("LB", "X",
//                "Left stick"), in the Controller Icons style.
std::u16string KeyOf(kbm::Input key) {
  if (key == kbm::kNoInput) return u"";
  if (const char16_t g = prompt_icons::KeyGlyph(key, prompt_icons::SlotSize::kCard)) return std::u16string(1, g);
  return Utf16(kbm::InputLabel(key));
}
std::u16string Key(kbm::Action action) {
  const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver) return u"";
  for (const kbm::Input key : driver->bindings().keys[size_t(action)]) {
    if (key != kbm::kNoInput) return KeyOf(key);
  }
  return u"";
}
std::u16string Pad(std::string_view name) {
  if (const char16_t g = prompt_icons::ButtonGlyph(name, prompt_icons::SlotSize::kCard)) {
    return std::u16string(1, g);
  }
  return Utf16(std::string(name));
}
// "[pad] or [key]" for a move both have; the pad alone without a key.
std::u16string PadOrKey(std::string_view pad, kbm::Action action) {
  const std::u16string key = Key(action);
  return key.empty() ? Pad(pad) : Pad(pad) + u" or " + key;
}

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
    return std::u16string(u"The most frames a second the game draws. The original runs at 30.");
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
    // F9 switches the picture between the two renderers (not with Emulated
    // Only: the port's renderer is off then).
    const std::u16string f9 = KeyOf(kbm::ParseInput("F9"));
    if (none()) return u"The Xbox 360's graphics chip, imitated step by step: looks like the original.";
    switch (i) {
      case kEmulatedOnly:
        return u"Only the imitated Xbox 360 graphics chip: the port's own renderer stays off, and " + f9 +
               u" doesn't switch.";
      case kEmulated:
        return u"The Xbox 360's graphics chip, imitated step by step: looks like the original. " + f9 +
               u" switches to the port's renderer.";
      case kNative:
        return u"The port's own renderer, made for PC. The imitation keeps drawing behind it: " + f9 +
               u" switches back at once.";
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
      case 1: return u"The frame rate, in a corner of the screen (FPS Position).";
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

// WINDOW MODE (ui/window_mode.h, findings/50): Windowed or Fullscreen; the
// fullscreen is always the borderless kind (a window covering the monitor at
// its own mode: quick to switch away, behaves the same on every system).
Row WindowModeRow() {
  Row r;
  r.label = u"Window Mode";
  r.values = [](Env&) { return std::vector<std::u16string>{u"Windowed", u"Fullscreen"}; };
  r.get = [](Env&) { return window_mode::Current() == window_mode::Mode::kWindowed ? 0 : 1; };
  r.set = [](Env&, int i) {
    window_mode::Set(i == 0 ? window_mode::Mode::kWindowed : window_mode::Mode::kFullscreen,
                     [](const char* name, const std::string& value) { SetFlag(name, value); });
  };
  r.describe = [](Env&, int i) -> std::u16string {
    return i == 0 ? u"Plays in a window you can move and resize."
                  : u"Fills the whole monitor (the one picked under Monitor).";
  };
  return r;
}

// A monitor's name as the game font can write it: plain letters only, short
// enough for the value box.
std::u16string MonitorName(const std::string& name) {
  std::u16string out;
  for (const char c : name) {
    if (c >= 0x20 && c < 0x7F) out += char16_t(c);
  }
  while (!out.empty() && out.back() == u' ') out.pop_back();
  if (out.size() > 16) out = out.substr(0, 15) + u".";
  return out.empty() ? std::u16string(u"Monitor") : out;
}

// MONITOR (ui/window_mode.h): the SDK's monitor flag, 0 = Automatic, 1..N =
// the connected monitors (SDL's order), read again each time it's shown.
Row MonitorRow() {
  Row r;
  r.label = u"Monitor";
  r.values = [](Env&) {
    window_mode::RefreshDisplays();  // monitors plugged in since: next time
    const auto displays = window_mode::Displays();
    std::vector<std::u16string> values{u"Automatic"};
    for (size_t i = 0; i < displays.size(); ++i) {
      values.push_back(Utf16(std::to_string(i + 1)) + u": " + MonitorName(displays[i].short_name));
    }
    // A monitor picked earlier that isn't connected now keeps its place.
    const int now = int(rex::cvar::Query<int32_t>("monitor"));
    while (now >= int(values.size())) {
      values.push_back(Utf16(std::to_string(values.size())) + u": Not Connected");
    }
    return values;
  };
  r.get = [](Env&) { return std::max(0, int(rex::cvar::Query<int32_t>("monitor"))); };
  r.set = [](Env&, int i) {
    SetFlag("monitor", std::to_string(std::max(0, i)));
    window_mode::AfterChange();
  };
  r.describe = [](Env&, int i) -> std::u16string {
    if (i == 0) return u"Fullscreen fills the monitor the game is on.";
    if (i > int(window_mode::Displays().size())) {
      return u"This monitor isn't connected: fullscreen fills the one the game is on.";
    }
    return u"Fullscreen fills this monitor.";
  };
  return r;
}

// ---------------------------------------------------------------------------
// Graphics: the picture's effects (effects/post_effects.h)
// ---------------------------------------------------------------------------

// A row for an int port flag whose values are 0..N-1, one label each.
Row LevelRow(std::u16string label, const char* flag, std::vector<std::u16string> values,
             std::vector<std::u16string> texts) {
  Row r;
  r.label = std::move(label);
  const int top = int(values.size()) - 1;
  r.values = [values](Env&) { return values; };
  r.get = [flag, top](Env&) { return std::clamp(int(rex::cvar::Query<int32_t>(flag)), 0, top); };
  r.set = [flag](Env&, int i) { SetFlag(flag, std::to_string(i)); };
  r.describe = [texts](Env&, int i) {
    return i >= 0 && i < int(texts.size()) ? texts[size_t(i)] : std::u16string();
  };
  return r;
}

// Rows only the native renderer draws: GREYED OUT while the main window
// shows the emulated picture (Row::unavailable), with the way to turn it on.
void NeedsNative(Row& r) {
  r.unavailable = [](Env&) {
    const bool native = g_hooks.native_shown && g_hooks.native_shown();
    return native ? std::u16string()
                  : std::u16string(u"Needs the port's renderer: set Renderer to Native on the Graphics tab.");
  };
}

// MOTION BLUR (findings/34): ours (the 360 game has none), native only.
Row MotionBlurRow() {
  Row r = LevelRow(u"Motion Blur", "motion_blur", {u"Off (original)", u"Low", u"Medium", u"High"},
                   {u"No motion blur, like the original game.",
                    u"A light smear of the scenery when the camera moves. The players stay sharp.",
                    u"The scenery smears more when the camera moves. The players stay sharp.",
                    u"A strong smear, as long as one frame of the original game's movement."});
  NeedsNative(r);
  return r;
}

// RENDER RESOLUTION (findings/45): how many pixels the native renderer draws
// the game with (its surfaces x 1 / 1.5 / 2 / 3), native only.
Row RenderResolutionRow() {
  Row r = LevelRow(u"Resolution", "render_resolution",
                   {u"720p (original)", u"1080p", u"1440p", u"4K"},
                   {u"Drawn at 1280 x 720, like the Xbox 360.",
                    u"Drawn at 1920 x 1080: sharper on a Full HD screen.",
                    u"Drawn at 2560 x 1440: sharper still, heavier on your graphics card.",
                    u"Drawn at 3840 x 2160: the sharpest picture, the heaviest on your graphics card."});
  NeedsNative(r);
  return r;
}

// ANTI-ALIASING (findings/35): the MSAA sample count of the game's
// multisampled 3D surfaces, native only. Values up to what this GPU can do
// (they're in order, so the list is just cut).
Row AntiAliasingRow() {
  Row r = LevelRow(u"Anti-aliasing", "anti_aliasing", {u"Off", u"2x (original)", u"4x", u"8x"},
                   {u"No smoothing: jagged, shimmering edges, the lightest on your graphics card.",
                    u"Edges smoothed with 2 samples per pixel, like the Xbox 360.",
                    u"Smoother edges, 4 samples per pixel.",
                    u"The smoothest edges, 8 samples per pixel: the heaviest on your graphics card."});
  const auto all = r.values;
  r.values = [all](Env& e) {
    auto values = all(e);
    const int max = native::MaxSurfaceSamples();
    const size_t keep = max >= 8 ? 4 : max >= 4 ? 3 : max >= 2 ? 2 : 1;
    values.resize(std::min(values.size(), keep));
    return values;
  };
  NeedsNative(r);
  return r;
}

// SMOOTH LEAVES AND FENCES (findings/35 s.3): the cut-out shapes' edges (alpha
// tested, so the multisampling can't smooth them) get coverage from their
// alpha, native only; needs anti-aliasing on.
Row SmoothCutoutsRow() {
  Row r = LevelRow(u"Smooth Leaves", "smooth_cutouts", {u"Off (original)", u"On"},
                   {u"Leaves, fences and other cut-out shapes keep their hard, shimmering edges, like the "
                    u"original game.",
                    u"Smooths the edges of leaves, fences and other cut-out shapes. Needs Anti-aliasing "
                    u"on."});
  NeedsNative(r);
  return r;
}

// TEXTURE SHARPNESS (findings/36): a more detailed mip level for the
// mipmapped textures (the native renderer's samplers), native only.
Row TextureSharpnessRow() {
  Row r = LevelRow(u"Texture Sharpness", "texture_sharpness", {u"Original", u"Sharper", u"Sharpest"},
                   {u"Textures soften in the distance like the Xbox 360's.",
                    u"Textures stay crisper in the distance and on slanted ground.",
                    u"The crispest distance, with a little more shimmer when moving."});
  NeedsNative(r);
  return r;
}

// SHADOW SOFTNESS (findings/41): how wide the characters' soft shadows fade
// at their edge (the shadow material's blur), native only.
Row ShadowSoftnessRow() {
  Row r = LevelRow(u"Shadow Softness", "shadow_softness", {u"Sharper", u"Original", u"Softer", u"Softest"},
                   {u"Shadows under the characters get a crisper edge.",
                    u"Shadows fade at their edge like the Xbox 360's.",
                    u"Shadows fade over a wider edge.",
                    u"The softest shadows, fading the widest."});
  NeedsNative(r);
  return r;
}

// SHARPENING (findings/41): a contrast-adaptive sharpening filter over the
// whole finished picture (our present pass), native only.
Row SharpeningRow() {
  Row r = LevelRow(u"Sharpening", "sharpening", {u"Off (original)", u"Low", u"Medium", u"High"},
                   {u"No sharpening, like the original game.",
                    u"Fine detail a little crisper. Hard edges stay clean.",
                    u"Crisper detail across the picture.",
                    u"The crispest picture: may look grainy in places."});
  NeedsNative(r);
  return r;
}

// DEPTH OF FIELD (findings/15): the game's own gentle blur of the distance,
// scaled on the way into its renderer, so both pictures follow.
Row DepthOfFieldRow() {
  return LevelRow(u"Depth of Field", "depth_of_field", {u"Off", u"Original", u"Strong"},
                  {u"The distance stays sharp.",
                   u"The distance is gently blurred, like the original game.",
                   u"The distance is blurred twice as much."});
}

// ---------------------------------------------------------------------------
// Audio: the game's own volumes, kept in user/settings.toml (global_options.h:
// the same for every save, no longer read back from save files)
// ---------------------------------------------------------------------------

struct Volume {
  global_options::Volume which;
  uint32_t sound_bank, sound;  // 0 = no sound of its own
};

void SetStars(Env& e, const Volume& v, int stars) {
  SetFlag(global_options::VolumeFlag(v.which), std::to_string(stars));
  global_options::ApplyVolumes(e.ctx, e.base);
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
  r.get = [v](Env&) { return std::clamp(global_options::Stars(v.which), 0, 5); };
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

// The menu owner's controller slot (the front end's Invert Axis byte index),
// -1 = none.
int InvertAxisSlot(Env& e) {
  const uint32_t fe = FrontEnd(e.base);
  if (!fe) return -1;
  const int slot = int(Call(__imp__sub_82266130, e.ctx, e.base, fe, uint32_t(e.player)));
  return slot >= 0 && slot < 4 ? slot : -1;
}

Row InvertAxisRow() {
  Row r;
  r.label = u"Invert Axis";
  r.values = [](Env&) { return std::vector<std::u16string>{u"Off", u"On"}; };
  r.get = [](Env& e) {
    const int slot = InvertAxisSlot(e);
    return slot >= 0 && global_options::InvertAxis(slot) ? 1 : 0;
  };
  r.set = [](Env& e, int i) {
    const int slot = InvertAxisSlot(e);
    if (slot < 0) return;
    SetFlag(global_options::InvertAxisFlag(slot), i == 1 ? "true" : "false");
    global_options::ApplyInvertAxis(e.base, FrontEnd(e.base));
  };
  r.describe = [](Env&, int i) {
    return i == 1 ? std::u16string(u"Up and down are flipped when you aim the reticle.")
                  : std::u16string(u"Aiming the reticle: up is up.");
  };
  return r;
}

// ---------------------------------------------------------------------------
// Keyboard and mouse keys (the Controls tab; 2026-10-15)
// ---------------------------------------------------------------------------
// The keys themselves live in user/controls.toml (input/bindings.h), the
// same file the F6 window edits; the rows change them through the keyboard
// driver's copy (SetBindings: in effect at once) and write the file at every
// change, as F6 does. Only the moves you make IN PLAY are listed: the menus'
// keys (Enter, Esc, Backspace, Q / E, the arrows) stay as they are, so a
// player can never lock themselves out of the menus, and the controller's
// leftovers with no use in the game (D-pad, right stick, LB, stick clicks)
// stay in F6 only (playtest, 2026-10-15).

// The moves listed, in order, with what each does (one short sentence each).
struct KeyRowInfo {
  kbm::Action action;
  const char16_t* text;
  // The controller's way, around its picture: "[stick] up" (the four moves),
  // "lean [stick] part of the way" (Walk). Null = the picture alone.
  const char16_t* before = nullptr;
  const char16_t* after = nullptr;
};
constexpr KeyRowInfo kKeyRows[] = {
    {kbm::kMoveUp, u"Move up.", nullptr, u"up"},
    {kbm::kMoveDown, u"Move down.", nullptr, u"down"},
    {kbm::kMoveLeft, u"Move left.", nullptr, u"left"},
    {kbm::kMoveRight, u"Move right.", nullptr, u"right"},
    {kbm::kWalk, u"Hold while moving to walk instead of run.", u"lean", u"part of the way"},
    {kbm::kSpin, u"The spin attack."},  // its controller line: SpinCard below
    {kbm::kJump, u"Jump. Press again in the air to double jump."},
    // Right after Jump: the key players need most after moving and jumping, and
    // the one the floating B over save totems and stunned titans shows.
    {kbm::kJack, u"Use a save totem, or jump on a stunned titan. Again to get off."},
    {kbm::kLightAttack, u"A quick attack."},
    {kbm::kHeavyAttack, u"A strong attack."},
    {kbm::kBlock, u"Hold to block attacks."},
    {kbm::kTitanSpecial, u"Your titan's special attack."},
    {kbm::kPocket, u"Store your titan, and bring it back later."},
    {kbm::kMap, u"Open the map."},
};

// "Light attack" -> "Light Attack", "Walk (hold)" -> "Walk (Hold)": the
// Options rows' Title Case.
std::u16string TitleCase(std::string_view s) {
  std::u16string out;
  bool start = true;
  for (const char c : s) {
    out.push_back(start && c >= 'a' && c <= 'z' ? char16_t(c - 'a' + 'A') : char16_t(c));
    start = c == ' ' || c == '(' || c == '-' || c == '/';
  }
  return out;
}

std::u16string NoKeyboard(Env&) {
  return kbm::KeyboardMouseDriver::Get() ? std::u16string()
                                         : std::u16string(u"The keyboard and mouse are turned off.");
}

// The pictures in the game's button prompts (input/prompt_icons.h): one
// style for the keys, one for controllers. The prompts still follow the
// device used last (keys after a key press, buttons after a controller's).
Row KeyboardIconsRow() {
  Row r;
  r.label = u"Keyboard Icons";
  r.values = [](Env&) { return std::vector<std::u16string>{u"Light", u"Dark"}; };
  r.get = [](Env&) { return std::clamp(int(REXCVAR_GET(keyboard_icons)), 0, 1); };
  r.set = [](Env&, int i) { SetFlag("keyboard_icons", std::to_string(i)); };
  r.describe = [](Env&, int i) {
    // An example in the style now on (the slots follow it): Jump's key.
    const std::u16string example = Key(kbm::kJump);
    return std::u16string(i == 1 ? u"Dark keys in the button prompts" : u"Light keys in the button prompts") +
           (example.empty() ? u"." : u", like " + example);  // (no "." after a picture: it stood apart)
  };
  return r;
}

Row ControllerIconsRow() {
  Row r;
  r.label = u"Controller Icons";
  r.values = [](Env&) {
    return std::vector<std::u16string>{u"Automatic", u"Xbox 360", u"Xbox Series", u"PlayStation 5",
                                       u"PlayStation 4", u"Switch"};
  };
  r.get = [](Env&) { return std::clamp(int(REXCVAR_GET(controller_icons)), 0, 5); };
  r.set = [](Env&, int i) { SetFlag("controller_icons", std::to_string(i)); };
  r.describe = [](Env&, int i) {
    // An example in the style now on: the A button (the bottom one).
    const std::u16string example = u" Now: " + Pad("A") + u" " + Pad("B") + u" " + Pad("X") + u" " + Pad("Y");
    if (i == 0) {
      return u"The buttons of the controller you play with: PlayStation, Switch or Xbox. Any other "
             u"controller: the game's own Xbox 360 buttons." + example;
    }
    return std::u16string(i == 1 ? u"The game's own Xbox 360 buttons, whatever controller you use."
                                 : u"These buttons in the prompts, whatever controller you use.") + example;
  };
  return r;
}

Row DefaultKeysRow() {
  Row r;
  r.label = u"Default Keys";
  r.kind = Kind::kAction;
  r.describe = [](Env&, int) {
    return std::u16string(u"Every keyboard and mouse key back as it came.");
  };
  r.unavailable = NoKeyboard;
  r.confirm = u"Revert Keybinds to Defaults?";
  r.activate = [](Env&) {
    kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
    if (!driver) return;
    // Who plays as which player stays: only the keys and the Spin / Walk
    // tunables go back.
    kbm::Bindings b = kbm::Bindings::Defaults();
    b.players = driver->bindings().players;
    driver->SetBindings(b);
    kbm::SaveBindings(b, kbm::ControlsFilePath());
    REXLOG_INFO("Options: every key back to its default");
  };
  return r;
}

// MOUSE AIMING (findings/47): how a keyboard-and-mouse player aims as a mask,
// and the Wii-style cursor in play (input/mouse_reticle.h, ui/wii_cursor.h).
Row MouseAimingRow() {
  Row r = LevelRow(u"Mouse Aiming", "mouse_reticle", {u"Off", u"Reticle", u"Crosshair"},
                   {u"As a mask, the aiming reticle moves with the movement keys, like the original game.",
                    u"As a mask, the aiming reticle follows the mouse.",
                    u"The aiming reticle follows the mouse, and in play the pointer is a glowing bubble that "
                    u"grows over targets and pulls in mojo, like the Wii version's pointer."});
  r.unavailable = NoKeyboard;
  return r;
}

std::vector<Row> ControlsRows() {
  std::vector<Row> rows{InvertAxisRow(), MouseAimingRow(), Heading(u"Button Pictures"), KeyboardIconsRow(),
                        ControllerIconsRow()};
  // The keys start under a heading of their own.
  rows.push_back(Heading(u"Keyboard and Mouse Keys"));
  for (const KeyRowInfo& k : kKeyRows) {
    Row r;
    r.label = ActionName(k.action);
    r.kind = Kind::kKey;
    r.key_action = k.action;
    const std::u16string text = k.text;
    const std::u16string before = k.before ? std::u16string(k.before) + u" " : u"";
    const std::u16string after = k.after ? u" " + std::u16string(k.after) : u"";
    r.describe = [text, before, after, action = k.action](Env&, int) -> std::u16string {
      // The controller's button as a picture in the Controller Icons style
      // (Pad: a card-sized picture), its way around it ("[stick] up").
      if (action == kbm::kSpin) {
        // Simple Spinning on: LB too (the stick still works).
        return text + u" Controller: " +
               (rex::cvar::Query<bool>("simple_spinning") ? Pad("LB") + u", or circle " + Pad("Left stick")
                                                          : u"circle " + Pad("Left stick"));
      }
      // A sentence ending in a picture gets no full stop (it stood apart,
      // after the picture's own room: playtest photo).
      return text + u" Controller: " + before + Pad(kbm::Info(action).pad) + (after.empty() ? u"" : after + u".");
    };
    r.unavailable = NoKeyboard;
    // The Spin key works only with Simple Spinning (Options -> Gameplay):
    // while it's off the row is GREYED (it can't be rebound: the key does
    // nothing) and the card says how to spin instead (playtest, 2026-10-15).
    if (k.action == kbm::kSpin) {
      r.unavailable = [](Env& env) {
        const std::u16string keyboard = NoKeyboard(env);
        if (!keyboard.empty()) return keyboard;
        return rex::cvar::Query<bool>("simple_spinning")
                   ? std::u16string()
                   : u"To spin, circle a controller's " + Pad("Left stick") +
                         u", or turn on Simple Spinning (Gameplay tab) to spin with this key.";
      };
    }
    rows.push_back(std::move(r));
  }
  rows.push_back(DefaultKeysRow());
  return rows;
}

// ---------------------------------------------------------------------------
// The tabs
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Gameplay: GAME MODIFICATIONS (gameplay/game_mods.h, 2026-10-15). Changes to
// the original game's rules; every row's first value is the original.
// ---------------------------------------------------------------------------

std::vector<Row> GameplayRows() {
  Row friendly_fire = OnOffRow(u"Friendly Fire", "friendly_fire", u"On", u"Off",
                               u"Players' attacks hurt each other, as in the original game.",
                               u"A partner's hits pass harmlessly: no damage, combos untouched.");
  Row partner_hits = OnOffRow(u"Partner Hits", "friendly_hits_break_combo", u"End Combos", u"Keep Combos",
                              u"Getting hit by a partner ends your hit combo, as in the original game.",
                              u"A partner's hit doesn't end your combo.");
  // Nothing to keep when partners can't hit each other at all.
  partner_hits.unavailable = [](Env&) {
    return rex::cvar::Query<bool>("friendly_fire") ? std::u16string()
                                                   : std::u16string(u"Friendly Fire is off: partners can't hit you.");
  };
  Row simple_spinning = OnOffRow(u"Simple Spinning", "simple_spinning", u"On", u"Off", u"", u"",
                                 /*off_first=*/true);
  // The texts show the buttons (the Spin key as it's bound now).
  simple_spinning.describe = [](Env&, int i) -> std::u16string {
    const std::u16string key = Key(kbm::kSpin);
    if (i == 1) {
      return u"One button spins: " + Pad("LB") + u" on a controller" +
             (key.empty() ? std::u16string(u".") : u", " + key + u" on the keyboard.") +
             u" Circling " + Pad("Left stick") + u" still works.";
    }
    return u"Circle " + Pad("Left stick") + u" to spin, as in the original game." +
           (key.empty() ? std::u16string() : u" " + key + u" does nothing.");
  };
  Row keep_spinning = OnOffRow(u"Keep Spinning", "keep_spinning", u"Hold to Spin", u"Original", u"", u"",
                               /*off_first=*/true);
  keep_spinning.describe = [](Env&, int i) -> std::u16string {
    if (i == 1) {
      return u"Hold " + PadOrKey("LB", kbm::kSpin) + u" (or keep circling " + Pad("Left stick") +
             u") and Crash keeps spinning, until he gets dizzy.";
    }
    return u"Spin, then press " + PadOrKey("X", kbm::kLightAttack) +
           u" again and again to keep spinning, as in the original game.";
  };
  // Holding the spin needs a spin you can hold: the button of Simple Spinning.
  keep_spinning.unavailable = [](Env&) {
    return rex::cvar::Query<bool>("simple_spinning") ? std::u16string()
                                                     : std::u16string(u"Turn on Simple Spinning first.");
  };
  // Co-op first (the camera rows were a tab of their own until 2026-10-24:
  // both only matter with partners), then spinning.
  return {Heading(u"Co-op"),
          friendly_fire,
          partner_hits,
          OnOffRow(u"Co-op Camera", "coop_camera", u"Frame Everyone", u"Original",
                   u"The camera pulls back to keep every player on screen.",
                   u"The camera of the original game."),
          OnOffRow(u"Camera Follows", "coop_camera_near_focus", u"Nearest Player", u"Middle",
                   u"Front to back, the camera stays with the player nearest to it.",
                   u"The camera aims at the middle of the group."),
          Heading(u"Spinning"),
          simple_spinning,
          keep_spinning};
}

// ---------------------------------------------------------------------------
// LANGUAGE (findings/49): the game's text and its voices, chosen apart. The
// game loads only the chosen language's strings and voices at start-up, so a
// change shows at the next start: the rows say so, and X = "Restart Now"
// (in game after a question: progress since the last save would be lost).
// ---------------------------------------------------------------------------

// The voices offered: only those whose archive is in the game folder.
std::vector<int> VoicesOnDisc() {
  std::vector<int> out;
  const auto& voices = language::VoiceLanguages();
  for (size_t i = 0; i < voices.size(); ++i) {
    if (language::Available(voices[i])) out.push_back(int(i));
  }
  return out;
}

void AddRestart(Row& r) {
  r.warn = [](Env&, int) {
    return language::RestartNeeded()
               ? std::u16string(u"Takes effect when the game starts again: Restart Now, or next time you play.")
               : std::u16string();
  };
  r.x_label = u"Restart Now";
  r.x_shown = [](Env&, int) { return language::RestartNeeded(); };
  r.x_confirm = [](Env&) {
    return options_menu::InMainMenu() ? std::u16string()
                                      : std::u16string(u"Restart now? Anything since your last save will be lost.");
  };
  r.x_action = [](Env&) {
    SaveChanged();  // the new choice must be in settings.toml for the next start
    app_restart::RestartNow();
  };
}

// The labels are short ("Text", "Voices": the tab says Language) and the
// values just the names: longer ones ran into the labels (playtest photo,
// 2026-10-24).
Row TextLanguageRow() {
  Row r;
  r.label = u"Text";
  r.values = [](Env&) {
    std::vector<std::u16string> v;
    for (const auto& t : language::TextLanguages()) v.push_back(t.name);
    return v;
  };
  r.get = [](Env&) { return language::ChosenText(); };
  r.set = [](Env&, int i) { SetFlag("text_language", language::TextLanguages()[size_t(i)].key); };
  r.describe = [](Env&, int) {
    return std::u16string(u"Menus, tutorials and messages in this language. The voices are chosen below.");
  };
  AddRestart(r);
  return r;
}

Row VoiceLanguageRow() {
  Row r;
  r.label = u"Voices";
  r.values = [](Env&) {
    const auto& voices = language::VoiceLanguages();
    std::vector<std::u16string> v{u"Same as Text"};
    for (int i : VoicesOnDisc()) v.push_back(voices[size_t(i)].name);
    return v;
  };
  r.get = [](Env&) {
    const std::vector<int> on_disc = VoicesOnDisc();
    const auto it = std::find(on_disc.begin(), on_disc.end(), language::ChosenVoice());
    return it == on_disc.end() ? 0 : int(it - on_disc.begin()) + 1;
  };
  r.set = [](Env&, int i) {
    const std::vector<int> on_disc = VoicesOnDisc();
    SetFlag("voice_language", i == 0 || i > int(on_disc.size())
                                  ? std::string("auto")
                                  : std::string(language::VoiceLanguages()[size_t(on_disc[size_t(i - 1)])].key));
  };
  r.describe = [](Env&, int i) {
    if (i == 0) {
      // The text's own voices, or English when the disc has none (Russian).
      const auto& voices = language::VoiceLanguages();
      const auto& text = language::TextLanguages()[size_t(language::ChosenText())];
      const auto& voice = voices[size_t(language::AutomaticVoice(language::ChosenText()))];
      if (std::string(voice.key) != text.voice) {
        return std::u16string(u"The disc has no voices in the text's language: the characters speak English.");
      }
      return u"The characters speak the text's language: " + std::u16string(voice.name) + u".";
    }
    return std::u16string(u"The characters speak this language, whatever the text is.");
  };
  AddRestart(r);
  return r;
}

std::vector<Tab> Build() {
  // THE LAYOUT (sorted 2026-10-24: Display = the screen and the window,
  // Graphics = how the picture is drawn; Camera folded into Gameplay's Co-op
  // heading). Headings are rows that can't be selected.
  std::vector<Tab> tabs;
  Row renderer = RendererRow();
  AddDualMode(renderer);
  tabs.push_back({u"Display",
                  {WindowModeRow(), MonitorRow(), VsyncRow(), FrameRateRow(), BrightnessRow(),
                   Heading(u"Frame Counter"), FpsCounterRow(), FpsPositionRow()}});
  // Renderer first: it decides which rows under it are greyed out.
  tabs.push_back({u"Graphics",
                  {renderer, Heading(u"Quality"), RenderResolutionRow(), AntiAliasingRow(), SmoothCutoutsRow(),
                   TextureSharpnessRow(), SharpeningRow(), Heading(u"Effects"), ShadowSoftnessRow(),
                   MotionBlurRow(), DepthOfFieldRow()}});
  tabs.push_back(
      {u"Audio",
       {VolumeRow(u"Music", {global_options::Volume::kMusic, 0, 0},
                  u"How loud the music plays.", true),
        VolumeRow(u"Dialogue", {global_options::Volume::kDialogue, kSoundDialogueBank, kSoundDialogue},
                  u"How loud the characters talk.", false),
        VolumeRow(u"SFX", {global_options::Volume::kSfx, kSoundSfxBank, kSoundSfx},
                  u"How loud hits, crates, footsteps and the rest are.", false)}});
  tabs.push_back({u"Controls", ControlsRows()});
  tabs.push_back({u"Gameplay", GameplayRows()});
  tabs.push_back({u"Language", {TextLanguageRow(), VoiceLanguageRow()}});
  // BREATHING ROOM (playtest, 2026-10-24: the cyan headings alone still read
  // as one crowded list): an EMPTY heading row (blank, never selected) before
  // every heading that doesn't open its tab.
  for (Tab& tab : tabs) {
    std::vector<Row> spaced;
    for (Row& row : tab.rows) {
      if (row.kind == Kind::kHeader && !row.label.empty() && !spaced.empty()) spaced.push_back(Heading(u""));
      spaced.push_back(std::move(row));
    }
    tab.rows = std::move(spaced);
  }
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


std::u16string KeysText(int action) {
  const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver || action < 0 || action >= kbm::kActionCount) return u"";
  const kbm::Bindings b = driver->bindings();
  std::u16string text;
  for (const kbm::Input key : b.keys[size_t(action)]) {
    if (key == kbm::kNoInput) continue;
    if (!text.empty()) text += u", ";
    text += TitleCase(kbm::InputLabel(key));
  }
  return text.empty() ? u"None" : text;
}

std::u16string KeyPictures(int action) {
  const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver || action < 0 || action >= kbm::kActionCount) return u"";
  const kbm::Bindings b = driver->bindings();
  std::u16string text;
  for (const kbm::Input key : b.keys[size_t(action)]) {
    if (key == kbm::kNoInput) continue;
    const char16_t glyph = prompt_icons::KeyGlyph(key);
    if (!glyph) return u"";  // no key slots: the names instead
    if (!text.empty()) text += u" ";
    text += glyph;
  }
  return text;
}

std::u16string ActionName(int action) {
  return TitleCase(kbm::Info(kbm::Action(action)).label);
}

void SetHooks(Hooks hooks) { g_hooks = std::move(hooks); }

void SetFlagToSave(const char* name, const std::string& value) { SetFlag(name, value); }

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
