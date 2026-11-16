// =============================================================================
// subtitles/button_tags.h -- the BUTTON TAGS a subtitle line may hold
// =============================================================================
//
// A control named in a line ("press the Y button") can be written as a tag,
// "{Y}", and the line then shows the PLAYER'S OWN button picture there: the
// key bound on the keyboard and mouse, else the controller's button in the
// Controller Icons style (findings/63 s.27). Shared by the game
// (subtitles.cpp: tag -> the game's prompt character -> the picture
// prompt_icons draws) and the subtitle editor (tools/subtitle_editor: the
// preview draws Xelu's pictures for the style picked there, and the font
// check knows a tag isn't a stray "{").
//
// A tag is the 360 control's name in braces, any case. Header only, no
// dependencies (the editor is a program of its own).
// =============================================================================
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace button_tags {

// The buttons a tag shows, by name (the editor's pictures use these):
// "A" "B" "X" "Y" "LB" "RB" "LT" "RT" "Back" "Start" "LS" "RS" "Rotate".
struct Tag {
  std::string_view name;      // lower case, without braces
  std::u16string_view glyphs; // the game's prompt characters (its text bank's)
  std::string_view buttons;   // space-separated button names, in order
};

inline constexpr Tag kTags[] = {
    {"a", u"±", "A"},          {"b", u"²", "B"},
    {"x", u"³", "X"},          {"y", u"´", "Y"},
    {"back", u"µ", "Back"},    {"start", u"¶", "Start"},
    {"rt", u"·", "RT"},        {"rb", u"¸", "RB"},
    {"lt", u"½", "LT"},        {"lb", u"¾", "LB"},
    {"ls", u"¹", "LS"},        {"rs", u"¥", "RS"},
    {"rotate", u"¼", "Rotate"},
    // The game's own way of writing a spin: the left stick + the rotate arrow.
    {"spin", u"¹¼", "LS Rotate"},
};

// The longest tag name (a "{...}" longer than this isn't a tag).
inline constexpr size_t kLongestName = 6;

// The tag named `name` (what is between the braces, any case), or null.
template <typename Char>
const Tag* Find(std::basic_string_view<Char> name) {
  if (name.empty() || name.size() > kLongestName) return nullptr;
  std::string lower;
  for (Char c : name) {
    if (c >= 0x80) return nullptr;
    lower += char(std::tolower(int(c)));
  }
  for (const Tag& t : kTags) {
    if (t.name == lower) return &t;
  }
  return nullptr;
}

// Every "{...}" of `text` (start, length incl. braces) in order: `f(start,
// length, tag)` with tag = null for a brace pair that isn't a known tag.
template <typename Char, typename F>
void ForEach(std::basic_string_view<Char> text, F&& f) {
  for (size_t open = text.find(Char('{')); open != std::basic_string_view<Char>::npos;) {
    const size_t close = text.find(Char('}'), open + 1);
    if (close == std::basic_string_view<Char>::npos) return;
    // A "{" again before the "}": the earlier one isn't a tag's.
    const size_t again = text.find(Char('{'), open + 1);
    if (again != std::basic_string_view<Char>::npos && again < close) {
      open = again;
      continue;
    }
    f(open, close - open + 1, Find(text.substr(open + 1, close - open - 1)));
    open = text.find(Char('{'), close + 1);
  }
}

}  // namespace button_tags
