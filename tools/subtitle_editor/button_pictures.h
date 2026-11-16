// =============================================================================
// tools/subtitle_editor/button_pictures.h -- the button tags' pictures in the
// editor (Xelu's free prompts, the same files the game uses)
// =============================================================================
//
// A subtitle line may hold BUTTON TAGS ("press {Y} to slam them": src/
// subtitles/button_tags.h, findings/63 s.27); the game shows each as the
// player's own button picture. The editor shows them the same way, live as
// the text is typed, in a STYLE picked in the editor (the player's device
// isn't known here): the keyboard and mouse (the player's OWN keys from
// user/controls.toml: a tag means the action bound to that control, so a
// rebind changes the picture, as in the game) or one of the controller sets.
//
// THE PICTURES: Xelu's Free Controller & Key Prompts (CC0,
// https://thoseawesomeguys.com/prompts/), assets/prompts/<set>/ of the
// source tree or of an installed game's program/. Not game data. The game's
// own Xbox 360 pictures are in the disc's font, which the editor doesn't
// read: no "360" style here (the Xbox Series set stands in).
//
// RichText: a line's words and tags as runs, measured and drawn with ImGui
// (the preview's subtitles, the "Shows as" line under a text box).
// =============================================================================
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace subtitle_editor {

enum class ButtonStyle { kKeyboard, kKeyboardDark, kXboxSeries, kDualSense, kDualShock, kJoyCon, kCount };
const char* StyleName(ButtonStyle s);

class ButtonPictures {
 public:
  // `folders` = where to look for assets/prompts (the first that has it wins).
  ButtonPictures(SDL_Renderer* renderer, const std::vector<std::filesystem::path>& folders);
  ~ButtonPictures();
  bool found() const { return !prompts_.empty(); }

  ButtonStyle style = ButtonStyle::kKeyboard;

  // The keys shown in the keyboard styles: the first controls.toml of
  // `files` that exists (the player's own user/controls.toml), else the
  // port's defaults. KeepControlsFresh (every frame) reads it again when it
  // changed on disk (a rebind in the game, F6 or Options).
  void ReadControls(const std::vector<std::filesystem::path>& files);
  void KeepControlsFresh();
  const std::filesystem::path& controls_file() const { return controls_file_; }

  // The width (pixels) of tag `buttons` ("Y", "LS Rotate"...) at line height `h`.
  float Width(std::string_view buttons, float h);
  // Draws it with its left edge at x, centred on the line from y to y + h;
  // `alpha` 0..255. Returns its width.
  float Draw(ImDrawList* dl, std::string_view buttons, float x, float y, float h, int alpha = 255);

 private:
  struct Texture {
    SDL_Texture* texture = nullptr;
    float aspect = 1;  // width / height of its drawn part
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // the drawn part (Xelu leaves an empty border)
  };
  // One piece of a button's picture: a texture in a box (0..1 of the unit
  // square the button stands in, its height = the line's).
  struct Piece {
    const Texture* texture;
    float x, y, w, h;  // in line heights
  };
  std::vector<Piece> PiecesOf(std::string_view button);
  const Texture* Load(const std::string& set, const std::string& file);
  // The keyboard file for a key name of bindings.cpp ("Space", "LMB", "W").
  std::string KeyFile(std::string_view key) const;

  SDL_Renderer* renderer_;
  std::filesystem::path prompts_;  // .../assets/prompts
  std::map<std::string, std::string> keys_;  // "Y" / "LS up" -> its first key ("" = none)
  std::filesystem::path controls_file_;
  std::filesystem::file_time_type controls_time_{};
  std::map<std::string, Texture> textures_;  // "<set>/<file>" (a failed load: no texture)
};

// A line as runs: text, or a tag's buttons.
struct RichRun {
  std::string text;      // the words (when !is_tag)
  std::string buttons;   // the tag's buttons (when is_tag)
  bool is_tag = false;
};
std::vector<RichRun> SplitTags(std::string_view text);
// Width of `text` (tags as pictures) at font `size`.
float RichWidth(ImFont* font, float size, ButtonPictures* pics, std::string_view text);
// Draws `text` from (x, y) at font `size`; words in `colour` (with a dark
// outline when `outline`), tags as pictures (a little taller than the
// letters, like the game's prompts). Returns the width drawn.
float DrawRich(ImDrawList* dl, ImFont* font, float size, ButtonPictures* pics, float x, float y,
               ImU32 colour, std::string_view text, bool outline);

// The same, WRAPPED at spaces to `max_width` (rows `size` x 1.25 apart);
// returns the height drawn (the "Shows as" lines under the text boxes).
float DrawRichWrapped(ImDrawList* dl, ImFont* font, float size, ButtonPictures* pics, float x, float y,
                      float max_width, ImU32 colour, std::string_view text);

}  // namespace subtitle_editor
