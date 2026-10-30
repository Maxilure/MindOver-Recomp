// =============================================================================
// ui/wii_cursor.cpp -- see wii_cursor.h (findings/47 s.4)
// =============================================================================
#include "wii_cursor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // the context's window list (InteractiveWindowOpen)
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "../data/data_patcher.h"
#include "../guest_memory.h"
#include "../data/pure3d.h"
#include "../input/keyboard_mouse.h"
#include "../input/mouse_menus.h"
#include "../input/mouse_reticle.h"
#include "../input/prompt_icons.h"
#include "../input/players.h"
#include "../pddi/intercept.h"
#include "../players/more_players.h"

// stb_image (from the SDK's source tree), private to this file.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

extern "C" REX_FUNC(__imp__sub_82102F28);  // CBehaviour: switch on / off (r3 it, r5 on): slot 15 / 16

namespace wii_cursor {
namespace {

using Clock = std::chrono::steady_clock;

// Where the bubble is (wii_cursor.h), and its glow: glow01.bmp, a soft round
// 64 x 64 glow of the game's effects, in the same package.
constexpr const char* kPackage = "package\\bca1bc0b.p3d";
constexpr const char* kBubbleTexture = "bubble01.bmp";
constexpr const char* kGlowTexture = "glow01.bmp";
constexpr uint32_t kChunkTexture = 0x19000, kChunkImage = 0x19001, kChunkImageData = 0x19002;

// The look, at the game's 720p (scaled with the picture): size, colour, and
// the trail (copies of the last positions, only while the mouse moves fast).
// Like the Wii cursor (its screenshots): a cyan ring with a whiter inner edge
// and a soft cyan halo. Drawn in three layers: the GLOW (glow01, kGlowScale x
// the bubble's size, faint cyan), the BUBBLE in cyan, and the bubble again in
// near-white, fainter (its bright rim = the whiter edge).
constexpr float kSize = 28.0f;                            // pixels across (34 first: a bit big)
constexpr ImU32 kColour = IM_COL32(120, 235, 255, 255);   // the Wii cursor's cyan
constexpr float kGlowScale = 2.6f;
constexpr ImU32 kGlowColour = IM_COL32(90, 220, 255, 120);
constexpr ImU32 kRimColour = IM_COL32(235, 255, 255, 140);
// THE OUTLINE: the bubble again, kOutlineScale x larger, in dark blue, UNDER
// everything: without it the light cyan bubble vanished over the menus' light
// cyan glass (playtest photo, Options screen). Two dark passes = a denser edge.
constexpr float kOutlineScale = 1.22f;
constexpr ImU32 kOutlineColour = IM_COL32(5, 25, 70, 235);
constexpr auto kTrailLife = std::chrono::milliseconds(100);
constexpr float kTrailSpeed = 1600.0f;  // 720p pixels per second before a trail shows
constexpr float kTrailAlpha = 60.0f;    // the newest copy's alpha (fainter: the stretch shows motion now)
// THE STRETCH: the bubble is drawn longer along its motion and thinner across
// it (same area), like a drop flying: length x (1 + speed / kStretchSpeed),
// at most kMaxStretch, width / sqrt(length). The speed is smoothed over
// ~kSmoothSeconds so a still mouse relaxes it and frame jitter doesn't shake it.
constexpr float kStretchSpeed = 2500.0f;  // 720p pixels per second for +100% length
constexpr float kMaxStretch = 1.8f;
constexpr float kSmoothSeconds = 0.05f;
// Over something to break the bubble grows by kGrow, eased in and out.
constexpr float kGrow = 1.5f;
constexpr float kGrowSeconds = 0.06f;

constexpr int kMaxPlayers = 4;

// Decided per game frame (OnFrameEnd), read by the overlay (UI thread).
std::atomic<bool> g_show{false};
// An ImGui window of ours (F5 cheats...) wants the mouse: the system pointer
// stays itself there (written by the overlay, UI thread).
std::atomic<bool> g_imgui_mouse{false};
// One of our INTERACTIVE ImGui windows is open (F5, F6, F2, the SDK's F4 /
// console / achievements...), hovered or not: the system pointer shows and
// the bubble steps aside (two pointers at once looked wrong). Overlays that
// take no input (banners, toasts, the build stamp) all carry NoInputs and
// don't count. Written by the overlay (UI thread).
std::atomic<bool> g_imgui_window{false};

// THE GAME'S OWN CURSOR (findings/47 s.5): every Crash is built with a
// CGameplayCursorBehaviour (players 1-4 alike), set up (slot 2, sub_8213FEA0:
// r3 it, r4 the Crash) but never switched on on the 360: its scripts call
// SetDesiredActive only under RAD_WII. SetDesiredActive (sub_82102BB0) = the
// behaviour's +24 bit 0x20; the behaviour manager then runs its activation
// (slot 15: registers its collector object +56 with the game's +48 and turns
// the player's reticle on) and its Update (slot 4, 0x82140038: a ray through
// the reticle's spot, sub_8224C6C0, and its "MOJO_VAC" collector placed where
// it lands = the Wii pointer's mojo vacuum). Here: switched on for the
// keyboard player's Crash on foot in crosshair mode, off otherwise (as a
// mask: the Wii scripts switched it off there too).
// A behaviour's +24: bit 0x40 = active, 0x20 = desired. SetDesiredActive only
// sets the wish; sub_82102FA0 compares the two and calls sub_82102F28(it, on),
// which runs slot 15 / 16 and sets both bits; nothing on the 360 does that for
// the cursor (a first version set the wish alone: its Update never ran). We
// call sub_82102F28 ourselves.
constexpr uint32_t kFlagsByte = 24;
constexpr uint8_t kActive = 0x40;
constexpr uint32_t kCoOpState = 0x8259B11C;  // per player: 2 = in a level, on foot
// Crash -> his cursor behaviour (set-up wrapper below; gone with its destructor).
std::unordered_map<uint32_t, uint32_t> g_cursor_of;
// The behaviours WE switched on (only those get switched off again).
std::unordered_map<uint32_t, bool> g_switched_on;
// The cursor's ray hit something that isn't plain scenery (an object: a prop,
// a crate, an enemy) this frame: the bubble grows (the Wii cursor did over
// things to break). Written by the ray wrapper (game thread), read by the
// overlay; steady clock ticks of the last such hit.
std::atomic<int64_t> g_over_target{0};

uint32_t Read32(uint32_t a) {
  const uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Is player p playing on the keyboard and mouse right now? Player 1 always
// has the keyboard and may also have a controller (players_setup.h): the
// device used LAST decides, as for the button pictures (prompt_icons.h
// KeysShown = player 1's look). Another player the keyboard answers for has
// no other device.
bool OnKeyboardNow(int p) { return p != 0 || prompt_icons::KeysShown(); }

// The player the keyboard plays as, if they're on foot in a level and the
// keyboard is their device right now; else -1 (2026-10-30: player 1 on a
// controller, or not in the game, has no in-play cursor).
int KeyboardPlayerOnFoot() {
  const kbm::PlayerAssignment* players = kbm::PlayerAssignment::Get();
  if (!players) return -1;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (players->KeyboardAnswersFor(uint32_t(p)) && Read32(kCoOpState + 4 * p) == 2 && OnKeyboardNow(p)) return p;
  }
  return -1;
}


// Should the cursor show (game thread)? In MENUS always, whatever Mouse Aiming
// says (on the Wii the menus are pointed at with the same bubble): the front
// end, and in a level any of the game's menus on screen (pause, totem, save
// list...). In PLAY only in crosshair mode, only for the keyboard's player
// ON FOOT and playing on the keyboard right now: not as a mask (they aim:
// the reticle shows, the Wii version hid its cursor then), not while out of
// the game, not while player 1 plays on a controller (2026-10-30, playtest:
// the bubble floated over play for nobody); back as soon as the keyboard or
// mouse is used again. Never while our own ImGui windows (F6 Controls, F5
// cheats) want the pointer.
bool ShouldShow() {
  const bool menu = !more_players::InPlay() || menu_input::MenuOnScreen();
  if ((!menu && mouse_reticle::Mode() != 2) || g_imgui_mouse.load() || g_imgui_window.load() ||
      (menu && mouse_menus::MouseBlocked())) {  // another player's menu: the mouse does nothing there
    return false;
  }
  const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  const kbm::PlayerAssignment* players = kbm::PlayerAssignment::Get();
  if (!driver || driver->menu_open() || !players) return false;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (!players->KeyboardAnswersFor(uint32_t(p))) continue;
    // Menus (the pause menu's Join Game included): always. In play: the
    // keyboard's player on foot (2) on the keyboard; not joined (0), a mask
    // (or getting there), or on a controller = none.
    if (menu) return true;
    return Read32(kCoOpState + 4 * p) == 2 && OnKeyboardNow(p);
  }
  return false;
}

void OnFrameEnd(void*) {
  const bool show = ShouldShow();
  g_show.store(show);
  // The system pointer's rule (keyboard_mouse.h SetPointerRule): shown while
  // an ImGui window of ours is open, hidden in play (whatever Mouse Aiming
  // says: Off had it back with every move), and hidden in a menu the mouse
  // doesn't work in (another player's: mouse_menus MouseBlocked). The bubble
  // hides there too; the system pointer took its place (2026-10-30, playtest:
  // player 1 out, player 2 paused = the plain pointer over player 2's menu).
  if (kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) {
    const bool menu = !more_players::InPlay() || menu_input::MenuOnScreen();
    const bool hide = !menu || mouse_menus::MouseBlocked();
    driver->SetPointerRule(g_imgui_window.load(), hide);
  }
  // The bubble stands in for the system pointer: hidden while it shows (it
  // comes back ~0.2 s after the last frame that asked, keyboard_mouse.h).
  if (show) {
    if (kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) driver->HidePointerWhileAiming();
  }
}

// The bubble as RGBA (white, alpha = its brightness, scaled so its brightest
// pixel is opaque): the game adds it onto the picture; alpha blending a
// tinted white with that alpha looks the same over the world.
bool LoadTexture(const char* name, std::vector<uint8_t>* rgba, int* w, int* h) {
  pure3d::Bytes d;
  std::string error;
  if (!data_patcher::ReadArchiveFile(kPackage, &d, &error)) {
    REXLOG_WARN("Wii cursor: can't read {} ({})", kPackage, error);
    return false;
  }
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(d, &root, &top)) return false;
  // The texture chunk named kTexture, anywhere in the package.
  std::vector<pure3d::Chunk> todo = top;
  while (!todo.empty()) {
    const pure3d::Chunk c = todo.back();
    todo.pop_back();
    if (c.id == kChunkTexture && pure3d::NameOf(d, c) == name) {
      for (const pure3d::Chunk& image : pure3d::ChildrenOf(d, c)) {
        if (image.id != kChunkImage) continue;
        for (const pure3d::Chunk& data : pure3d::ChildrenOf(d, image)) {
          if (data.id != kChunkImageData || data.data_size < 16) continue;
          const uint32_t size = pure3d::Le32(&d[data.offset + 12]);
          if (16 + size_t(size) > data.data_size) continue;
          int n = 0;
          stbi_uc* px = stbi_load_from_memory(&d[data.offset + 16], int(size), w, h, &n, 4);
          if (!px) continue;
          int brightest = 1;
          for (int i = 0; i < *w * *h; ++i) {
            brightest = std::max({brightest, int(px[4 * i]), int(px[4 * i + 1]), int(px[4 * i + 2])});
          }
          rgba->assign(size_t(*w) * *h * 4, 255);
          for (int i = 0; i < *w * *h; ++i) {
            const int v = std::max({int(px[4 * i]), int(px[4 * i + 1]), int(px[4 * i + 2])});
            (*rgba)[4 * i + 3] = uint8_t(std::min(255, v * 255 / brightest));
          }
          stbi_image_free(px);
          REXLOG_INFO("Wii cursor: {} ({} x {}) from {}", name, *w, *h, kPackage);
          return true;
        }
      }
    }
    const std::vector<pure3d::Chunk> kids = pure3d::ChildrenOf(d, c);
    todo.insert(todo.end(), kids.begin(), kids.end());
  }
  REXLOG_WARN("Wii cursor: no {} in {}", name, kPackage);
  return false;
}

// Is one of our interactive ImGui windows open? Asked while drawing: a window
// counts if it was shown last frame or is shown this one (the dialogs draw in
// turn, so ones after ours haven't begun yet). Skipped: windows that take no
// input (NoMouseInputs: banners, toasts, the build stamp), children,
// tooltips, and ImGui's own fallback window "Debug##Default" (begun by every
// frame, shown only if something draws outside a window).
bool InteractiveWindowOpen() {
  const ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g) return false;
  for (const ImGuiWindow* w : g->Windows) {
    if (!(w->Active || w->WasActive) || w->Hidden) continue;
    if (w->Flags & (ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_ChildWindow |
                    ImGuiWindowFlags_Tooltip)) {
      continue;
    }
    if (w->IsFallbackWindow) continue;
    return true;
  }
  return false;
}

class CursorDialog : public rex::ui::ImGuiDialog {
 public:
  CursorDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate)
      : ImGuiDialog(drawer), immediate_(immediate) {}

  // The window repaints with every game frame anyway.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Clock::time_point now = Clock::now();
    g_imgui_mouse.store(io.WantCaptureMouse);
    g_imgui_window.store(InteractiveWindowOpen());
    if (!g_show.load() || !TextureReady()) {
      trail_.clear();
      have_last_ = false;  // it comes back round, not flung from where it was
      velocity_ = ImVec2(0, 0);
      grow_ = 1.0f;
      return;
    }
    const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
    float px = 0, py = 0;
    uint32_t moves = 0;
    if (!driver || !driver->Pointer(&px, &py, &moves)) return;

    // The picture's rectangle in the window (the game's 16:9, fitted like the
    // presenter does), and one pixel of its 720p.
    const float window_w = io.DisplaySize.x, window_h = io.DisplaySize.y;
    if (window_w <= 0 || window_h <= 0) return;
    float pic_w = window_w, pic_h = window_w * 9.0f / 16.0f;
    if (pic_h > window_h) {
      pic_h = window_h;
      pic_w = window_h * 16.0f / 9.0f;
    }
    const float pic_x = (window_w - pic_w) * 0.5f, pic_y = (window_h - pic_h) * 0.5f;
    const float unit = pic_h / 720.0f;
    const ImVec2 at(pic_x + px * pic_w, pic_y + py * pic_h);

    // The trail: the last positions while the mouse moves fast.
    if (trail_.empty() || trail_.back().at.x != at.x || trail_.back().at.y != at.y) {
      trail_.push_back({now, at});
    }
    while (!trail_.empty() && now - trail_.front().when > kTrailLife) trail_.pop_front();

    ImDrawList* list = ImGui::GetForegroundDrawList();
    const ImTextureID id = ImTextureID(reinterpret_cast<uintptr_t>(texture_.get()));
    const ImTextureID glow = ImTextureID(reinterpret_cast<uintptr_t>(glow_.get()));
    // Over a target (the game cursor's ray hit an object in the last ~0.1 s):
    // grow to kGrow, eased (~kGrowSeconds) both ways.
    const int64_t hit = g_over_target.load();
    const bool over = hit != 0 && (now.time_since_epoch().count() - hit) <
                                      std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(100)).count();
    const float frame_s = have_last_ ? std::chrono::duration<float>(now - last_when_).count() : 0.0f;
    grow_ += ((over ? kGrow : 1.0f) - grow_) * (1.0f - std::exp(-frame_s / kGrowSeconds));
    const float half = kSize * unit * 0.5f * grow_;
    for (size_t i = 1; i < trail_.size(); ++i) {
      const Point& a = trail_[i - 1];
      const Point& b = trail_[i];
      const float seconds = std::chrono::duration<float>(b.when - a.when).count();
      const float dx = (b.at.x - a.at.x) / unit, dy = (b.at.y - a.at.y) / unit;
      if (seconds <= 0 || std::sqrt(dx * dx + dy * dy) / seconds < kTrailSpeed) continue;
      // Older = smaller and fainter.
      const float age = std::chrono::duration<float>(now - b.when).count() /
                        std::chrono::duration<float>(kTrailLife).count();
      const float k = std::clamp(1.0f - age, 0.0f, 1.0f);
      const float r = half * (0.45f + 0.4f * k);
      const ImU32 c = (kColour & 0x00FFFFFFu) | (ImU32(kTrailAlpha * k) << IM_COL32_A_SHIFT);
      list->AddImage(id, ImVec2(b.at.x - r, b.at.y - r), ImVec2(b.at.x + r, b.at.y + r), ImVec2(0, 0),
                     ImVec2(1, 1), c);
    }

    // The smoothed velocity (720p pixels per second) from this draw's move.
    if (have_last_) {
      const float dt = std::chrono::duration<float>(now - last_when_).count();
      if (dt > 0) {
        const float vx = (at.x - last_at_.x) / unit / dt, vy = (at.y - last_at_.y) / unit / dt;
        const float blend = 1.0f - std::exp(-dt / kSmoothSeconds);
        velocity_.x += (vx - velocity_.x) * blend;
        velocity_.y += (vy - velocity_.y) * blend;
      }
    }
    have_last_ = true;
    last_at_ = at;
    last_when_ = now;

    // The bubble, stretched along that velocity WITHOUT turning: the square
    // stays upright (its highlight where the picture has it) and goes through
    // a pure stretch, M = stretch x d d^T + (1 / sqrt(stretch)) x n n^T (d
    // the direction, n across it): symmetric, so no rotation. (A first
    // version turned the square to face the motion: the highlight spun.)
    const float speed = std::sqrt(velocity_.x * velocity_.x + velocity_.y * velocity_.y);
    const float stretch = std::min(1.0f + speed / kStretchSpeed, kMaxStretch);
    const float dx = speed > 1.0f ? velocity_.x / speed : 1.0f, dy = speed > 1.0f ? velocity_.y / speed : 0.0f;
    const float thin = 1.0f / std::sqrt(stretch);
    const float m00 = stretch * dx * dx + thin * dy * dy, m11 = stretch * dy * dy + thin * dx * dx;
    const float m01 = (stretch - thin) * dx * dy;  // = m10
    auto corner = [&](float sx, float sy) {  // sx, sy = -1 / +1: the upright square's corner
      return ImVec2(at.x + (m00 * sx + m01 * sy) * half, at.y + (m01 * sx + m11 * sy) * half);
    };
    // The halo (round, a little stretched too), the bubble, its whiter rim.
    const float g = half * kGlowScale;
    list->AddImage(glow, ImVec2(at.x - g * std::sqrt(stretch), at.y - g * std::sqrt(stretch)),
                   ImVec2(at.x + g * std::sqrt(stretch), at.y + g * std::sqrt(stretch)), ImVec2(0, 0), ImVec2(1, 1),
                   kGlowColour);
    auto bubble = [&](float scale, ImU32 colour) {
      auto c = [&](float sx, float sy) {
        const ImVec2 v = corner(sx, sy);
        return ImVec2(at.x + (v.x - at.x) * scale, at.y + (v.y - at.y) * scale);
      };
      list->AddImageQuad(id, c(-1, -1), c(1, -1), c(1, 1), c(-1, 1), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                         ImVec2(0, 1), colour);
    };
    bubble(kOutlineScale, kOutlineColour);
    bubble(kOutlineScale, kOutlineColour);
    bubble(1.0f, kColour);
    bubble(1.0f, kRimColour);
  }

 private:
  // The bubble's texture, made the first time it's needed (UI thread; the
  // archive is read then: the runtime exists). A failure is final.
  bool TextureReady() {
    if (texture_ && glow_) return true;
    if (failed_ || !immediate_ || !rex::Runtime::instance()) return false;
    auto make = [&](const char* name) -> std::unique_ptr<rex::ui::ImmediateTexture> {
      std::vector<uint8_t> rgba;
      int w = 0, h = 0;
      if (!LoadTexture(name, &rgba, &w, &h)) return nullptr;
      return immediate_->CreateTexture(uint32_t(w), uint32_t(h),
                                       rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
    };
    texture_ = make(kBubbleTexture);
    glow_ = make(kGlowTexture);
    failed_ = !texture_ || !glow_;
    return !failed_;
  }

  struct Point {
    Clock::time_point when;
    ImVec2 at;
  };
  rex::ui::ImmediateDrawer* immediate_;
  std::unique_ptr<rex::ui::ImmediateTexture> texture_, glow_;
  bool failed_ = false;
  std::deque<Point> trail_;
  // The stretch's motion: the last draw's spot and time, the smoothed velocity.
  bool have_last_ = false;
  ImVec2 last_at_{0, 0};
  Clock::time_point last_when_;
  ImVec2 velocity_{0, 0};
  float grow_ = 1.0f;  // the size over targets, eased
};

// The one dialog, kept for the whole session (like fps_overlay's).
CursorDialog* g_dialog = nullptr;

}  // namespace

void Install() { pddi::AddFrameEndListener(&OnFrameEnd, nullptr); }

void SwitchGameCursors(PPCContext& ctx, uint8_t* base) {
  const bool crosshair = mouse_reticle::Mode() == 2;
  const int keyboard = crosshair && more_players::InPlay() ? KeyboardPlayerOnFoot() : -1;
  const uint32_t want = keyboard >= 0 ? more_players::CharacterOfPlayer(keyboard) : 0;
  for (const auto& [actor, cursor] : g_cursor_of) {
    const bool active = *rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(cursor + kFlagsByte) & kActive;
    const bool on = actor == want;
    if (on == active || (!on && !g_switched_on.count(cursor))) continue;  // (never touch one we didn't switch on)
    const PPCContext saved = ctx;
    ctx.r3.u64 = cursor;
    ctx.r5.u64 = on ? 1 : 0;
    __imp__sub_82102F28(ctx, base);
    ctx = saved;
    if (on) {
      g_switched_on[cursor] = true;
      REXLOG_INFO("Wii cursor: the game's cursor switched on for player {}", keyboard + 1);
    } else {
      g_switched_on.erase(cursor);
      REXLOG_INFO("Wii cursor: the game's cursor switched off");
    }
  }
}

bool GameCursorActive(uint32_t crash) {
  const auto it = g_cursor_of.find(crash);
  return it != g_cursor_of.end() &&
         (*rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(it->second + kFlagsByte) & kActive);
}

void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate) {
  if (drawer && !g_dialog) g_dialog = new CursorDialog(drawer, immediate);
}

}  // namespace wii_cursor

// --- the game's cursor behaviour ------------------------------------------------

extern "C" REX_FUNC(__imp__sub_8213FEA0);  // CGameplayCursorBehaviour: set up (slot 2: r3 it, r4 its Crash)
extern "C" REX_FUNC(__imp__sub_8213FE40);  // CGameplayCursorBehaviour: destructor body
extern "C" REX_FUNC(__imp__sub_8224C6C0);  // ray through a screen spot (r3 world, r4 spot, r5 query, r6 hit)

extern "C" REX_FUNC(sub_8213FEA0) {
  wii_cursor::g_cursor_of[ctx.r4.u32] = ctx.r3.u32;
  __imp__sub_8213FEA0(ctx, base);
}
extern "C" REX_FUNC(sub_8213FE40) {
  const uint32_t cursor = ctx.r3.u32;
  for (auto it = wii_cursor::g_cursor_of.begin(); it != wii_cursor::g_cursor_of.end();) {
    it = it->second == cursor ? wii_cursor::g_cursor_of.erase(it) : std::next(it);
  }
  wii_cursor::g_switched_on.erase(cursor);
  __imp__sub_8213FE40(ctx, base);
}

// The ray: when the game's cursor casts it (its Update, call at 0x82140120,
// returns to 0x82140124), a SECOND ray from the same spot asks for targets:
// the cursor's own query tests only the scenery (flags +0x1C = 0x90, mask
// +0x14 = 0), enough to place its mojo collector, so it never reports what
// it points at. Ours is the aiming reticle's kind (sub_822AC668: flags 0xA0 =
// scenery + TARGETABLES, the nearest in front of the scenery wins), every
// target type (mask ~0, sub_8216EB88's "all"), the keyboard player's own Crash
// ignored (list at +0, its end at +0x10). A target in the result (+0x1C, the
// targetable; sub_8224C7F8) = the bubble grows. (A first try read the
// cursor's own result: only scenery, never an object.)
constexpr uint32_t kQueryBytes = 0x24, kResultBytes = 0x20;
constexpr uint32_t kQueryEnd = 0x10, kQueryMask = 0x14, kQueryFlags = 0x1C;
constexpr uint8_t kScenery = 0x80, kTargets = 0x20;
constexpr uint32_t kResultTarget = 0x1C;

// THE "ALL MOJO AT ONCE" BUG (playtest, 2026-10-24): now and then, leaving
// Crash's house, the keyboard player picked up EVERY mojo in the level the
// moment it loaded. Cause: the ray's hit spot (result +16/+20/+24) came back
// NaN. The ray (sub_8224C6C0) unprojects the screen spot with the camera
// (sub_82265C80) into a near and a far point and normalises far - near
// (sub_82424880); while the level loads the camera isn't set up yet (or the
// spot lands on a degenerate one), far - near = 0, and 0 / 0 = NaN. The
// cursor's Update then puts its MOJO_VAC collector there (sub_822515D8: a
// sphere, centre +0x3C, radius +0x48, box +4..+0x18 = centre -/+ radius).
// With a NaN centre the box is NaN, and every "is this mojo outside the
// box?" comparison is false (NaN compares false with everything): each mojo
// in the level counts as inside the vacuum. On the Wii the cursor only ran
// once the scripts switched it on in play; we switch it on as soon as the
// player is on foot, which can be under the loading screen.
// FIX: a hit spot that isn't a finite, sane number is replaced by one far
// below the world (kParked), where the vacuum touches nothing; the next good
// frame puts it back on the pointer.
constexpr uint32_t kResultSpot = 0x10;
constexpr float kSaneRange = 1.0e6f;  // world units; levels are a few thousand across
constexpr float kParked[3] = {0.0f, -1.0e5f, 0.0f};

float ReadFloat(uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  const uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WriteFloat(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  uint8_t* p = GuestPtr(base, a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// The cursor ray's hit spot is usable (finite and inside kSaneRange); if
// not, it is parked (above) and the event logged (first one, then every
// 300th: a loading screen can produce one per frame).
void KeepSpotSane(uint8_t* base, uint32_t result) {
  bool sane = true;
  for (int i = 0; i < 3; ++i) {
    const float v = ReadFloat(base, result + kResultSpot + 4 * i);
    if (!std::isfinite(v) || std::fabs(v) > kSaneRange) sane = false;
  }
  if (sane) return;
  static uint32_t count = 0;
  if (count++ % 300 == 0) {
    REXLOG_WARN("Wii cursor: aim ray gave a bad spot ({}, {}, {}); mojo vacuum parked (x{})",
                ReadFloat(base, result + kResultSpot), ReadFloat(base, result + kResultSpot + 4),
                ReadFloat(base, result + kResultSpot + 8), count);
  }
  for (int i = 0; i < 3; ++i) WriteFloat(base, result + kResultSpot + 4 * i, kParked[i]);
}

extern "C" REX_FUNC(sub_8224C6C0) {
  const bool cursor = uint32_t(ctx.lr) == 0x82140124;
  const uint32_t world = ctx.r3.u32, spot = ctx.r4.u32, query = ctx.r5.u32, cursor_result = ctx.r6.u32;
  __imp__sub_8224C6C0(ctx, base);
  if (!cursor) return;
  KeepSpotSane(base, cursor_result);  // before the Update places its vacuum there
  static uint32_t room = 0;  // our query + result, guest memory, made once
  if (!room) room = rex::system::kernel_memory()->SystemHeapAlloc(kQueryBytes + kResultBytes);
  if (!room) return;
  const uint32_t ours = room, result = room + kQueryBytes;
  std::memcpy(GuestPtr(base, ours), GuestPtr(base, query), kQueryBytes);
  std::memset(GuestPtr(base, result), 0, kResultBytes);
  auto put32 = [&](uint32_t a, uint32_t v) {
    uint8_t* p = GuestPtr(base, a);
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
  };
  // Ignore list: the keyboard player's Crash (the cursor's own player).
  const int p = wii_cursor::KeyboardPlayerOnFoot();
  const uint32_t self = p >= 0 ? more_players::CharacterOfPlayer(p) : 0;
  put32(ours, self);
  put32(ours + kQueryEnd, self ? ours + 4 : ours);
  put32(ours + kQueryMask, 0xFFFFFFFFu);
  *GuestPtr(base, ours + kQueryFlags) = uint8_t(*GuestPtr(base, ours + kQueryFlags) | kScenery | kTargets);
  const PPCContext saved = ctx;
  ctx.r3.u64 = world; ctx.r4.u64 = spot; ctx.r5.u64 = ours; ctx.r6.u64 = result;
  __imp__sub_8224C6C0(ctx, base);
  ctx = saved;
  const uint32_t target = wii_cursor::Read32(result + kResultTarget);
  if (target) wii_cursor::g_over_target.store(std::chrono::steady_clock::now().time_since_epoch().count());
}
