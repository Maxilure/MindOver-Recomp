// =============================================================================
// tools/subtitle_editor/button_pictures.cpp -- see button_pictures.h
// =============================================================================

#include "button_pictures.h"

#include <algorithm>
#include <cfloat>
#include <fstream>
#include <system_error>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "subtitles/button_tags.h"

namespace fs = std::filesystem;

namespace subtitle_editor {

const char* StyleName(ButtonStyle s) {
  switch (s) {
    case ButtonStyle::kKeyboard: return "Keyboard + mouse";
    case ButtonStyle::kKeyboardDark: return "Keyboard + mouse (dark keys)";
    case ButtonStyle::kXboxSeries: return "Xbox Series";
    case ButtonStyle::kDualSense: return "DualSense";
    case ButtonStyle::kDualShock: return "DualShock";
    case ButtonStyle::kJoyCon: return "Joy-Con";
    default: return "?";
  }
}

namespace {

// A controller set's files by button (the 360's PLACES: "A" = the bottom
// button, Cross on a PlayStation, B on a Switch), as the game's
// prompt_icons.cpp kPadSets.
struct PadSet {
  const char* folder;
  const char* prefix;
  // A B X Y Back Start RT RB LT LB LS RS
  const char* files[12];
};
constexpr const char* kButtonNames[12] = {"A", "B", "X", "Y", "Back", "Start", "RT", "RB", "LT", "LB", "LS", "RS"};
constexpr PadSet kXbox{"xbox-series", "XboxSeriesX_",
                       {"A", "B", "X", "Y", "View", "Menu", "RT", "RB", "LT", "LB", "Left_Stick", "Right_Stick"}};
constexpr PadSet kPs5{"ps5", "PS5_",
                      {"Cross", "Circle", "Square", "Triangle", "Share", "Options", "R2", "R1", "L2", "L1",
                       "Left_Stick", "Right_Stick"}};
constexpr PadSet kPs4{"ps4", "PS4_",
                      {"Cross", "Circle", "Square", "Triangle", "Share", "Options", "R2", "R1", "L2", "L1",
                       "Left_Stick", "Right_Stick"}};
constexpr PadSet kSwitch{"switch", "Switch_",
                         {"B", "A", "Y", "X", "Minus", "Plus", "RT", "RB", "LT", "LB", "Left_Stick", "Right_Stick"}};

// WHICH KEY: a tag names the 360 control, which on the keyboard is the
// ACTION bound to it (src/input/bindings.cpp kActions: Y = heavy attack...),
// so the game shows the key the player bound to that action, whatever it is.
// The editor shows the same: the keys of the controls.toml it found (the
// player's own, user/controls.toml), else the port's defaults. The first key
// of each action, as the game's prompts. An action without a key (LB by
// default) keeps the controller picture, as in the game (here: Xbox Series).
struct ActionOf {
  const char* button;
  const char* action;      // controls.toml's name
  const char* default_key;
};
constexpr ActionOf kActionsOf[] = {
    {"A", "jump", "Space"},          {"B", "jack", "E"},
    {"X", "light_attack", "LMB"},    {"Y", "heavy_attack", "RMB"},
    {"RT", "block", "Shift"},        {"RB", "pocket", "R"},
    {"LT", "titan_special", "F"},    {"LB", "left_bumper", ""},
    {"Back", "map", "Tab"},          {"Start", "pause", "Escape"},
    {"LS up", "move_up", "W"},       {"LS left", "move_left", "A"},
    {"LS down", "move_down", "S"},   {"LS right", "move_right", "D"},
    {"RS up", "right_stick_up", "Numpad8"},     {"RS left", "right_stick_left", "Numpad4"},
    {"RS down", "right_stick_down", "Numpad2"}, {"RS right", "right_stick_right", "Numpad6"},
};

int ButtonIndex(std::string_view b) {
  for (int i = 0; i < 12; ++i) {
    if (b == kButtonNames[i]) return i;
  }
  return -1;
}

}  // namespace

ButtonPictures::ButtonPictures(SDL_Renderer* renderer, const std::vector<fs::path>& folders)
    : renderer_(renderer) {
  std::error_code ec;
  for (const fs::path& f : folders) {
    if (fs::is_directory(f / "assets" / "prompts" / "xbox-series", ec)) {
      prompts_ = f / "assets" / "prompts";
      break;
    }
  }  ReadControls({});  // the defaults until the editor names a file
}

void ButtonPictures::ReadControls(const std::vector<fs::path>& files) {
  keys_.clear();
  for (const ActionOf& a : kActionsOf) keys_[a.button] = a.default_key;
  controls_file_.clear();
  std::error_code ec;
  for (const fs::path& f : files) {
    std::ifstream in(f);
    if (!in) continue;
    controls_file_ = f;
    controls_time_ = fs::last_write_time(f, ec);
    // Lines `action = "Key1, Key2"   # comment` (bindings.cpp writes them).
    std::string line;
    while (std::getline(in, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos || line[0] == '#') continue;
      std::string name = line.substr(0, eq);
      while (!name.empty() && name.back() == ' ') name.pop_back();
      const size_t q0 = line.find('"', eq), q1 = q0 == std::string::npos ? q0 : line.find('"', q0 + 1);
      if (q1 == std::string::npos) continue;
      std::string first = line.substr(q0 + 1, q1 - q0 - 1);
      first = first.substr(0, first.find(','));
      while (!first.empty() && first.back() == ' ') first.pop_back();
      for (const ActionOf& a : kActionsOf) {
        if (name == a.action) keys_[a.button] = first;  // "" = no key
      }
    }
    break;
  }
}

void ButtonPictures::KeepControlsFresh() {
  if (controls_file_.empty()) return;
  std::error_code ec;
  const auto t = fs::last_write_time(controls_file_, ec);
  if (!ec && t != controls_time_) ReadControls({controls_file_});  // rebound in the game: shown at once
}

ButtonPictures::~ButtonPictures() {
  for (auto& [name, t] : textures_) {
    if (t.texture) SDL_DestroyTexture(t.texture);
  }
}

const ButtonPictures::Texture* ButtonPictures::Load(const std::string& set, const std::string& file) {
  const std::string key = set + "/" + file;
  auto [it, added] = textures_.try_emplace(key);
  if (added && !prompts_.empty()) {
    int w = 0, h = 0, n = 0;
    const fs::path path = prompts_ / set / file;
    if (stbi_uc* px = stbi_load(path.string().c_str(), &w, &h, &n, 4)) {
      // The drawn part: Xelu's pictures leave an empty border (~13 px of 100).
      int x0 = w, y0 = h, x1 = -1, y1 = -1;
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          if (px[(size_t(y) * w + x) * 4 + 3] == 0) continue;
          x0 = std::min(x0, x);
          y0 = std::min(y0, y);
          x1 = std::max(x1, x);
          y1 = std::max(y1, y);
        }
      }
      if (x1 >= x0) {
        Texture& t = it->second;
        t.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (t.texture) {
          SDL_UpdateTexture(t.texture, nullptr, px, w * 4);
          SDL_SetTextureBlendMode(t.texture, SDL_BLENDMODE_BLEND);
          SDL_SetTextureScaleMode(t.texture, SDL_SCALEMODE_LINEAR);
        }
        t.u0 = float(x0) / float(w);
        t.v0 = float(y0) / float(h);
        t.u1 = float(x1 + 1) / float(w);
        t.v1 = float(y1 + 1) / float(h);
        t.aspect = float(x1 - x0 + 1) / float(y1 - y0 + 1);
      }
      stbi_image_free(px);
    }
  }
  return it->second.texture ? &it->second : nullptr;
}

std::string ButtonPictures::KeyFile(std::string_view key) const {
  const bool dark = style == ButtonStyle::kKeyboardDark;
  const std::string suffix = dark ? "_Key_Dark.png" : "_Key_Light.png";
  // Key names = bindings.cpp's; Xelu's files are named after the KEY CAP's
  // character (the game's prompt_icons.cpp PictureFile, same table).
  static const std::map<std::string_view, std::string_view> kFiles = {
      {"LMB", "Mouse_Left"},         {"RMB", "Mouse_Right"},        {"MMB", "Mouse_Middle"},
      {"WheelUp", "Mouse_Middle"},   {"WheelDown", "Mouse_Middle"}, {"Return", "Enter"},
      {"Escape", "Esc"},             {"Control", "Ctrl"},           {"CapsLock", "Caps_Lock"},
      {"Up", "Arrow_Up"},            {"Down", "Arrow_Down"},        {"Left", "Arrow_Left"},
      {"Right", "Arrow_Right"},      {"Delete", "Del"},             {"PageUp", "Page_Up"},
      {"PageDown", "Page_Down"},     {"Backtick", "Tilda"},         {"LBracket", "Bracket_Left"},
      {"RBracket", "Bracket_Right"}, {"Backslash", "Slash"},        {"Comma", "Mark_Left"},
      {"Period", "Mark_Right"},      {"Slash", "Question"},         {"NumpadPlus", "Plus"},
      {"NumpadMinus", "Minus"},      {"NumpadStar", "Asterisk"},    {"NumLock", "Num_Lock"},
      {"PrintScreen", "Print_Screen"},
  };
  if (const auto it = kFiles.find(key); it != kFiles.end()) return std::string(it->second) + suffix;
  if (key.rfind("Numpad", 0) == 0 && key.size() == 7) return std::string(key.substr(6)) + suffix;  // Numpad8 = the 8
  return std::string(key) + suffix;  // Space, Shift, Tab, Alt, Insert, letters, digits, F1-F12...
}

std::vector<ButtonPictures::Piece> ButtonPictures::PiecesOf(std::string_view button) {
  std::vector<Piece> out;
  const bool keys = style == ButtonStyle::kKeyboard || style == ButtonStyle::kKeyboardDark;
  const std::string key_set = style == ButtonStyle::kKeyboardDark ? "keyboard-mouse-dark" : "keyboard-mouse";
  // One picture filling the line height, its own shape.
  auto whole = [&](const Texture* t) {
    if (t) out.push_back({t, 0, 0, t->aspect, 1});
  };
  if (button == "Rotate") {
    // The circling arrow (the game draws Xelu's on keys, its own 360 arrow
    // on a controller; the editor: Xelu's for every style).
    whole(Load("keyboard-mouse", "Directional_Arrow_Full_Circle.png"));
    return out;
  }
  if (keys && (button == "LS" || button == "RS")) {
    // The four direction keys in the usual shape: up over left / down / right
    // (the game's prompt pictures likewise; a direction without a key = the
    // stick's picture).
    const std::string b(button);
    const std::string dirs[4] = {b + " up", b + " left", b + " down", b + " right"};
    const float s = 0.52f;  // each key, in line heights
    const float place[4][2] = {{s, 0}, {0, s}, {s, s}, {2 * s, s}};
    std::vector<Piece> grid;
    for (int i = 0; i < 4; ++i) {
      const std::string& key = keys_[dirs[i]];
      const Texture* t = key.empty() ? nullptr : Load(key_set, KeyFile(key));
      if (!t) {
        grid.clear();
        break;
      }
      grid.push_back({t, place[i][0] + 0.02f, place[i][1] + 0.02f, s - 0.04f, s - 0.04f});
    }
    if (!grid.empty()) return grid;
  } else if (keys) {
    const std::string& key = keys_[std::string(button)];
    if (const Texture* t = key.empty() ? nullptr : Load(key_set, KeyFile(key))) {
      whole(t);
      return out;
    }
    // No key (LB by default), or a key Xelu has no picture of: the
    // controller's picture (the game draws a labelled blank key for the
    // latter; the editor keeps it simple).
  }
  const int index = ButtonIndex(button);
  if (index < 0) return out;
  const PadSet& set = style == ButtonStyle::kDualSense   ? kPs5
                      : style == ButtonStyle::kDualShock ? kPs4
                      : style == ButtonStyle::kJoyCon    ? kSwitch
                                                         : kXbox;
  whole(Load(set.folder, std::string(set.prefix) + set.files[index] + ".png"));
  return out;
}

float ButtonPictures::Width(std::string_view buttons, float h) {
  float w = 0;
  bool first = true;
  for (size_t at = 0; at < buttons.size();) {
    size_t end = buttons.find(' ', at);
    if (end == std::string_view::npos) end = buttons.size();
    float right = 0;
    for (const Piece& p : PiecesOf(buttons.substr(at, end - at))) right = std::max(right, p.x + p.w);
    if (right > 0) {
      w += (first ? 0 : h * 0.08f) + right * h;
      first = false;
    }
    at = end + 1;
  }
  return w;
}

float ButtonPictures::Draw(ImDrawList* dl, std::string_view buttons, float x, float y, float h, int alpha) {
  const float start = x;
  bool first = true;
  for (size_t at = 0; at < buttons.size();) {
    size_t end = buttons.find(' ', at);
    if (end == std::string_view::npos) end = buttons.size();
    const std::vector<Piece> pieces = PiecesOf(buttons.substr(at, end - at));
    float right = 0;
    for (const Piece& p : pieces) right = std::max(right, p.x + p.w);
    if (right > 0) {
      if (!first) x += h * 0.08f;
      first = false;
      for (const Piece& p : pieces) {
        const ImVec2 a(x + p.x * h, y + p.y * h), b(x + (p.x + p.w) * h, y + (p.y + p.h) * h);
        dl->AddImage(ImTextureID(reinterpret_cast<intptr_t>(p.texture->texture)), a, b,
                     ImVec2(p.texture->u0, p.texture->v0), ImVec2(p.texture->u1, p.texture->v1),
                     IM_COL32(255, 255, 255, alpha));
      }
      x += right * h;
    }
    at = end + 1;
  }
  return x - start;
}

std::vector<RichRun> SplitTags(std::string_view text) {
  std::vector<RichRun> out;
  size_t from = 0;
  button_tags::ForEach(text, [&](size_t at, size_t length, const button_tags::Tag* tag) {
    if (!tag) return;  // not a tag: stays in the words
    if (at > from) out.push_back({std::string(text.substr(from, at - from)), {}, false});
    out.push_back({{}, std::string(tag->buttons), true});
    from = at + length;
  });
  if (from < text.size()) out.push_back({std::string(text.substr(from)), {}, false});
  return out;
}

namespace {
// A picture's height against the letters' size: the game's prompts are a
// little taller than its capitals (an A button ~ the line's cell).
constexpr float kPictureHeight = 1.15f;
}  // namespace

float RichWidth(ImFont* font, float size, ButtonPictures* pics, std::string_view text) {
  float w = 0;
  for (const RichRun& r : SplitTags(text)) {
    if (r.is_tag) w += pics ? pics->Width(r.buttons, size * kPictureHeight) : 0;
    else w += font->CalcTextSizeA(size, FLT_MAX, 0, r.text.c_str()).x;
  }
  return w;
}

float DrawRich(ImDrawList* dl, ImFont* font, float size, ButtonPictures* pics, float x, float y,
               ImU32 colour, std::string_view text, bool outline) {
  const float start = x;
  const int alpha = int((colour >> IM_COL32_A_SHIFT) & 0xFF);
  for (const RichRun& r : SplitTags(text)) {
    if (r.is_tag) {
      if (!pics) continue;
      const float h = size * kPictureHeight;
      // Centred on the letters' middle (ImGui's text box: its top at y, `size` tall).
      x += pics->Draw(dl, r.buttons, x, y + size * 0.5f - h * 0.5f, h, alpha);
      continue;
    }
    if (r.text.empty()) continue;
    if (outline) {
      const float o = std::max(1.0f, size * 0.06f);
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
          if (dx || dy) {
            dl->AddText(font, size, ImVec2(x + dx * o, y + dy * o), IM_COL32(0, 0, 0, std::min(alpha, 220)),
                        r.text.c_str());
          }
        }
      }
    }
    dl->AddText(font, size, ImVec2(x, y), colour, r.text.c_str());
    x += font->CalcTextSizeA(size, FLT_MAX, 0, r.text.c_str()).x;
  }
  return x - start;
}

float DrawRichWrapped(ImDrawList* dl, ImFont* font, float size, ButtonPictures* pics, float x, float y,
                      float max_width, ImU32 colour, std::string_view text) {
  // Words (a tag is part of its word: "{Y}," stays whole) laid out in rows.
  const float row_h = size * 1.25f, space = font->CalcTextSizeA(size, FLT_MAX, 0, " ").x;
  float pen = 0, top = 0;
  for (size_t i = 0; i < text.size();) {
    if (text[i] == ' ' || text[i] == '\n') {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\n') ++j;
    const std::string_view word = text.substr(i, j - i);
    const float w = RichWidth(font, size, pics, word);
    if (pen > 0 && pen + space + w > max_width) {
      pen = 0;
      top += row_h;
    } else if (pen > 0) {
      pen += space;
    }
    DrawRich(dl, font, size, pics, x + pen, y + top + (row_h - size) * 0.5f, colour, word, false);
    pen += w;
    i = j;
  }
  return top + row_h;
}

}  // namespace subtitle_editor
