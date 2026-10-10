// =============================================================================
// ui/scrooby.cpp -- see scrooby.h
// =============================================================================
#include "scrooby.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"

extern "C" REX_FUNC(__imp__sub_82371AD0);  // page: find a text by name
extern "C" REX_FUNC(__imp__sub_82371A90);  // page: find a picture by name
extern "C" REX_FUNC(__imp__sub_82371A50);  // page: find a menu by name
extern "C" REX_FUNC(__imp__sub_82373360);  // text: set its string (UTF-16, copied)
extern "C" REX_FUNC(__imp__sub_823574E8);  // make heap r3 current, returns the previous
extern "C" REX_FUNC(__imp__sub_82370A58);  // element: reset its transform
extern "C" REX_FUNC(__imp__sub_82371198);  // element: scale around its centre (f1)
extern "C" REX_FUNC(__imp__sub_823710C8);  // element: turn around its centre (f1, r5 = axis)
extern "C" REX_FUNC(__imp__sub_82373F28);  // picture: show frame r4
extern "C" REX_FUNC(__imp__sub_82370840);  // element: colour r4 (0xAARRGGBB)
extern "C" REX_FUNC(__imp__sub_8237A4C0);  // Scrooby Menu::SetSelection(index)
extern "C" REX_FUNC(__imp__sub_82261800);  // front end: play a sound (bank, name, f1-f3)

namespace scrooby {
namespace {

constexpr uint32_t kGameGlobal = 0x8259B190;  // the game (front end at +52)
constexpr uint32_t kFrontEndHeap = 7;         // the heap the game's screens set texts under
// The front end's sound bank and the two menu sounds CMenuAction plays
// (sub_820D2A00: "down_left" for down / left, "up_right" for up / right).
constexpr uint32_t kSoundBank = 0x820222E4;     // "FE_SFX"
constexpr uint32_t kSoundDownLeft = 0x82023220; // "down_left"
constexpr uint32_t kSoundUpRight = 0x8202322C;  // "up_right"

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint32_t GuestBytes(const uint8_t* bytes, size_t size) {
  // One lock: all callers are on the game's main thread today, but the pool
  // is cheap to keep safe.
  static std::mutex mutex;
  static std::unordered_map<std::string, uint32_t> cache;
  static uint32_t block = 0, used = 0;
  constexpr uint32_t kBlockSize = 4096;
  std::lock_guard lock(mutex);
  const std::string key(reinterpret_cast<const char*>(bytes), size);
  if (const auto it = cache.find(key); it != cache.end()) return it->second;
  if (size > kBlockSize) return 0;
  if (!block || used + size > kBlockSize) {
    block = rex::system::kernel_memory()->SystemHeapAlloc(kBlockSize);
    used = 0;
    if (!block) return 0;
  }
  const uint32_t address = block + used;
  std::memcpy(rex::system::kernel_memory()->TranslateVirtual(address), bytes, size);
  used += uint32_t((size + 3) & ~size_t(3));
  cache.emplace(key, address);
  return address;
}

}  // namespace

uint32_t Call(GuestFunction function, PPCContext& ctx, uint8_t* base, uint32_t r3, uint32_t r4,
              uint32_t r5) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

uint32_t CallF(GuestFunction function, PPCContext& ctx, uint8_t* base, uint32_t r3, double f1,
               double f2, double f3) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.f1.f64 = f1;
  ctx.f2.f64 = f2;
  ctx.f3.f64 = f3;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

uint32_t GuestAscii(std::string_view text) {
  std::string bytes(text);
  bytes.push_back('\0');
  return GuestBytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

uint32_t GuestUtf16(std::u16string_view text) {
  std::string bytes;
  for (char16_t c : text) {
    bytes.push_back(char(c >> 8));  // the guest's byte order
    bytes.push_back(char(c & 0xFF));
  }
  bytes.append(2, '\0');
  return GuestBytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

uint32_t FindText(PPCContext& ctx, uint8_t* base, uint32_t page, std::string_view name) {
  return page ? Call(__imp__sub_82371AD0, ctx, base, page, GuestAscii(name)) : 0;
}
uint32_t FindPicture(PPCContext& ctx, uint8_t* base, uint32_t page, std::string_view name) {
  return page ? Call(__imp__sub_82371A90, ctx, base, page, GuestAscii(name)) : 0;
}
uint32_t FindMenu(PPCContext& ctx, uint8_t* base, uint32_t page, std::string_view name) {
  return page ? Call(__imp__sub_82371A50, ctx, base, page, GuestAscii(name)) : 0;
}

void SetText(PPCContext& ctx, uint8_t* base, uint32_t text, std::u16string_view value) {
  if (!text) return;
  // The text copies the string into the CURRENT heap; the game's own
  // screens make heap 7 current around it (0x8225E160), else the copy
  // asserts (players/lost_controller.cpp).
  const uint32_t heap = Call(__imp__sub_823574E8, ctx, base, kFrontEndHeap);
  Call(__imp__sub_82373360, ctx, base, text, GuestUtf16(value));
  Call(__imp__sub_823574E8, ctx, base, heap);
}

void SetVisible(uint8_t* base, uint32_t element, bool visible) {
  if (!element) return;
  uint8_t* flags = GuestPtr(base, element + 110);
  *flags = visible ? (*flags | 0x80) : (*flags & ~0x80);
}
bool Visible(uint8_t* base, uint32_t element) {
  return element && (*GuestPtr(base, element + 110) & 0x80);
}

void SetScale(PPCContext& ctx, uint8_t* base, uint32_t element, float scale) {
  if (!element) return;
  Call(__imp__sub_82370A58, ctx, base, element);
  CallF(__imp__sub_82371198, ctx, base, element, scale);
}

void Rotate(PPCContext& ctx, uint8_t* base, uint32_t element, float degrees) {
  if (!element) return;
  // The axis (0, 0, 1) as three big-endian floats in guest memory.
  static const uint8_t kAxis[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0x3F, 0x80, 0, 0};
  const uint32_t axis = GuestBytes(kAxis, sizeof(kAxis));
  if (!axis) return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = element;
  ctx.r5.u64 = axis;
  ctx.f1.f64 = degrees;
  __imp__sub_823710C8(ctx, base);
  ctx = saved;
}

void SetFrame(PPCContext& ctx, uint8_t* base, uint32_t picture, uint32_t frame) {
  if (picture) Call(__imp__sub_82373F28, ctx, base, picture, frame);
}
void SetColour(PPCContext& ctx, uint8_t* base, uint32_t element, uint32_t argb) {
  if (element) Call(__imp__sub_82370840, ctx, base, element, argb);
}

int ItemCount(uint8_t* base, uint32_t menu) {
  if (!menu) return 0;
  return int((Read32(base, menu + 116) - Read32(base, menu + 112)) / 4);
}
uint32_t Item(uint8_t* base, uint32_t menu, int index) {
  if (index < 0 || index >= ItemCount(base, menu)) return 0;
  return Read32(base, Read32(base, menu + 112) + 4 * uint32_t(index));
}
int Selection(uint8_t* base, uint32_t menu) {
  return menu ? int(int8_t(*GuestPtr(base, menu + 152))) : -1;
}
void Select(PPCContext& ctx, uint8_t* base, uint32_t menu, int index) {
  if (menu) Call(__imp__sub_8237A4C0, ctx, base, menu, uint32_t(index));
}
void SetSelectable(uint8_t* base, uint32_t item, bool selectable) {
  if (!item) return;
  uint8_t* flags = GuestPtr(base, item + 137);
  *flags = selectable ? (*flags | 0x80) : (*flags & ~0x80);
}

void PlaySound(PPCContext& ctx, uint8_t* base, Sound sound) {
  const uint32_t game = Read32(base, kGameGlobal);
  const uint32_t front_end = game ? Read32(base, game + 52) : 0;
  if (!front_end) return;
  // As CMenuAction plays them: (front end, "FE_SFX", name, 1.0, 0.0, 1.0).
  const PPCContext saved = ctx;
  ctx.r3.u64 = front_end;
  ctx.r4.u64 = kSoundBank;
  ctx.r5.u64 = sound == Sound::kMove ? kSoundDownLeft : kSoundUpRight;
  ctx.f1.f64 = 1.0;
  ctx.f2.f64 = 0.0;
  ctx.f3.f64 = 1.0;
  __imp__sub_82261800(ctx, base);
  ctx = saved;
}

}  // namespace scrooby
