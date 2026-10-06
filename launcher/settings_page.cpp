// =============================================================================
// settings_page.cpp -- see settings_page.h
// =============================================================================
#include "settings_page.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace fs = std::filesystem;

namespace settings_page {
namespace {

// -----------------------------------------------------------------------------
// Warnings: which values are risky, and why (the one place to change them)
// -----------------------------------------------------------------------------

struct Warning {
  const char* label = nullptr;   // short, beside the control
  const char* detail = nullptr;  // the tooltip
};

// `get` reads another setting's current value (some warnings depend on two).
template <typename Get>
Warning WarningFor(const std::string& name, const std::string& value, Get&& get) {
  using settings::AsBool;
  using settings::AsNumber;
  if (name == "fps_cap") {
    const double fps = AsNumber(value);
    if (fps == 0 || fps > 60) {
      return {"Experimental",
              "Above 60 fps is experimental. The game was made for 30: physics, animations, "
              "cutscenes and scripted moments may misbehave (60 is tested and fine). If something "
              "looks wrong, try 60 and report it."};
    }
  } else if (name == "picture") {
    if (value == "native") {
      return {"In development",
              "The native renderer draws nearly everything, but an effect may still be missing "
              "or look different. F9 switches back to the emulated picture while playing."};
    }
    if (value == "native_only") {
      return {"Experimental",
              "Only the native renderer draws: the fastest, but anything it can't draw yet is "
              "missing, and F10 photos only have the native half. F9 turns the emulated one back "
              "on."};
    }
    if (value == "dual") {
      return {"Heavy on the GPU",
              "Two windows: the emulated picture and the native one, both drawn live. Meant for "
              "comparing the two; it needs a strong graphics card."};
    }
  } else if (name == "local_players") {
    if (AsNumber(value) > 2) {
      return {"Experimental",
              "Players 3 and 4 are this port's addition (the original has 2): their HUD, "
              "markers, camera and menus are new and still being tested. Expect rough edges."};
    }
  } else if (name == "coop_camera") {
    if (AsBool(value)) {
      return {"Experimental",
              "The new co-op camera keeps every player in the picture. It needs more testing in "
              "fights, tight places and cutscenes. Turn it off for the original camera."};
    }
  } else if (name == "ground_grace_ms") {
    const double fps = AsNumber(get("fps_cap"));
    if (AsNumber(value) == 0 && fps != 30) {
      return {"Falls",
              "Off above 30 fps: Crash may flash into his fall animation for a split second when "
              "stepping down uneven ground. 40 ms is the tested value."};
    }
  }
  return {};
}

// A yellow warning triangle (drawn, so it needs no symbol font); the tooltip
// explains. Returns nothing: it's only a sign.
void WarningIcon(const Warning& warning, const ImVec4& colour) {
  const float size = ImGui::GetFontSize();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float top = at.y + (ImGui::GetFrameHeight() - size) * 0.5f;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImU32 fill = ImGui::GetColorU32(colour);
  const ImU32 mark = IM_COL32(30, 22, 8, 255);
  draw->AddTriangleFilled({at.x + size * 0.5f, top}, {at.x + size, top + size * 0.92f},
                          {at.x, top + size * 0.92f}, fill);
  // The "!": a bar and a dot.
  const float cx = at.x + size * 0.5f, w = std::max(1.5f, size * 0.09f);
  draw->AddRectFilled({cx - w, top + size * 0.32f}, {cx + w, top + size * 0.62f}, mark);
  draw->AddRectFilled({cx - w, top + size * 0.70f}, {cx + w, top + size * 0.80f}, mark);
  ImGui::Dummy({size, ImGui::GetFrameHeight()});
  if (ImGui::IsItemHovered()) {
    ImGui::SetNextWindowSize({size * 26, 0});
    ImGui::BeginTooltip();
    ImGui::TextWrapped("%s", warning.detail);
    ImGui::EndTooltip();
  }
  ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(colour, "%s", warning.label);
  if (ImGui::IsItemHovered()) {
    ImGui::SetNextWindowSize({size * 26, 0});
    ImGui::BeginTooltip();
    ImGui::TextWrapped("%s", warning.detail);
    ImGui::EndTooltip();
  }
}

// The Picture choice <-> its five flags.
constexpr const char* kPictureFlags[] = {"renderer", "native_only", "native_window",
                                         "emulated_only"};
struct PictureChoice {
  const char* id;
  const char* label;
};
constexpr PictureChoice kPictures[] = {
    {"emulated", "Emulated (the default)"},
    {"native", "Native renderer"},
    {"native_only", "Native only (fastest)"},
    {"dual", "Both, in two windows"},
    {"emulated_only", "Emulated only"},
};

// Not offered in All settings: load_save / log_file are given per session by
// the launcher (settings.h); save_library stays ON (the save list replaces the
// game's 3 slots for good; the flag remains a command-line switch for
// development, e.g. comparing with the original screen).
bool IsHidden(const std::string& name) {
  return name == "load_save" || name == "log_file" || name == "save_library";
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return text;
}

// A number for display: "180" not "180.000000".
std::string NumberText(double value) {
  char text[48];
  std::snprintf(text, sizeof(text), "%g", value);
  return text;
}

}  // namespace

Page::Page(fs::path exe, fs::path game_folder, fs::path user_folder)
    : exe_(std::move(exe)),
      game_folder_(std::move(game_folder)),
      values_(user_folder / "settings.toml") {
  values_.ReloadIfChanged();
  // The catalogue may have to ask the game (a fraction of a second, more on
  // a cold disk): off the UI thread.
  loader_ = std::thread([this] {
    std::string error;
    if (!catalogue_.Load(exe_, game_folder_, game_folder_ / "cache" / "launcher" / "settings_list.toml",
                         &error)) {
      load_error_ = error;
    }
    loaded_ = true;
  });
}

Page::~Page() {
  if (loader_.joinable()) {
    loader_.join();
  }
}

void Page::BeforeLaunch() {
  if (loaded_) {
    values_.RemoveSessionOnly(catalogue_);
  }
}

std::string Page::Value(const std::string& name) const {
  const settings::Setting* setting = catalogue_.Find(name);
  return setting ? values_.Get(*setting) : std::string();
}

void Page::SetValue(const std::string& name, const std::string& value) {
  if (const settings::Setting* setting = catalogue_.Find(name)) {
    std::string error;
    if (!values_.Set(*setting, value, &error)) {
      write_error_ = error;
    } else {
      write_error_.clear();
    }
  }
}

void Page::Row(const char* label, const char* help) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 26, 0});
    ImGui::BeginTooltip();
    ImGui::TextWrapped("%s", help);
    ImGui::EndTooltip();
  }
  ImGui::TableNextColumn();
  ImGui::SetNextItemWidth(-FLT_MIN);
}

// The row's last column: its warning (if any) and a Default button when the
// value isn't the default. Returns true if Default was pressed.
bool Page::Status(const std::string& name) {
  ImGui::TableNextColumn();
  const Warning warning =
      WarningFor(name, name == "picture" ? std::string() : Value(name),
                 [this](const std::string& other) { return Value(other); });
  if (warning.label) {
    WarningIcon(warning, look_->warn);
    ImGui::SameLine();
  }
  bool reset = false;
  if (values_.IsSet(name)) {
    ImGui::PushID(name.c_str());
    reset = ImGui::SmallButton("Default");
    ImGui::PopID();
    if (reset) {
      std::string error;
      values_.Reset(name, &error);
    }
  }
  return reset;
}

// ----------------------------------------------------------------------------
// The main settings
// ----------------------------------------------------------------------------

void Page::DrawMain() {
  const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit;
  const float unit = ImGui::GetFontSize();

  auto section = [&](const char* title) {
    ImGui::Spacing();
    ImGui::PushFont(look_->bold, ImGui::GetStyle().FontSizeBase * 1.1f);
    ImGui::TextColored(look_->accent, "%s", title);
    ImGui::PopFont();
    return ImGui::BeginTable(title, 3, flags);
  };
  auto columns = [&] {
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, unit * 13);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, unit * 15);
    ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch);
  };
  // A tick box for a bool setting.
  auto check = [&](const char* label, const char* name, const char* help) {
    Row(label, help);
    bool on = settings::AsBool(Value(name));
    ImGui::PushID(name);
    if (ImGui::Checkbox("##v", &on)) {
      SetValue(name, on ? "true" : "false");
    }
    ImGui::PopID();
    Status(name);
  };
  // A slider for a number setting; written when the slider is let go.
  static std::map<std::string, float> dragging;
  auto slider = [&](const char* label, const char* name, float min, float max, const char* format,
                    const char* help) {
    Row(label, help);
    float value = dragging.count(name) ? dragging[name] : float(settings::AsNumber(Value(name)));
    ImGui::PushID(name);
    if (ImGui::SliderFloat("##v", &value, min, max, format)) {
      dragging[name] = value;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      SetValue(name, NumberText(std::round(value * 100) / 100));
      dragging.erase(name);
    }
    ImGui::PopID();
    Status(name);
  };

  // --- Frame rate -------------------------------------------------------------
  if (section("Frame rate")) {
    columns();
    Row("Frame rate cap",
        "How many frames per second the game runs at. 30 is the original game's rate.");
    const int fps = int(settings::AsNumber(Value("fps_cap")));
    static const int kRates[] = {30, 60, 120, 144, 165, 180, 240, 0};
    static bool custom = false;
    const bool preset = std::find(std::begin(kRates), std::end(kRates), fps) != std::end(kRates);
    char current[48];
    if (custom || !preset) {
      std::snprintf(current, sizeof(current), "Other: %d", fps);
    } else if (fps == 0) {
      std::snprintf(current, sizeof(current), "Unlimited");
    } else {
      std::snprintf(current, sizeof(current), fps == 30 ? "%d (the original)" : "%d", fps);
    }
    if (ImGui::BeginCombo("##fps", current)) {
      for (int rate : kRates) {
        char label[32];
        if (rate == 0) {
          std::snprintf(label, sizeof(label), "Unlimited");
        } else {
          std::snprintf(label, sizeof(label), rate == 30 ? "%d (the original)" : "%d", rate);
        }
        if (ImGui::Selectable(label, !custom && preset && rate == fps)) {
          custom = false;
          SetValue("fps_cap", std::to_string(rate));
        }
      }
      if (ImGui::Selectable("Other...", custom || !preset)) {
        custom = true;
      }
      ImGui::EndCombo();
    }
    if (Status("fps_cap")) {
      custom = false;
    }
    if (custom || !preset) {
      Row("   Frames per second", nullptr);
      int value = fps;
      if (ImGui::InputInt("##fpsother", &value, 1, 10)) {
        value = std::clamp(value, 0, 1000);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemEdited() && !ImGui::IsItemActive())) {
        SetValue("fps_cap", std::to_string(std::clamp(value, 0, 1000)));
      }
      ImGui::TableNextColumn();
    }
    slider("Ground grace", "ground_grace_ms", 0, 100, "%.0f ms",
           "Above 30 fps: how long a lost ground contact is forgiven, so Crash doesn't flash into "
           "his fall animation stepping down uneven ground. 40 ms is the tested value; 0 = off.");
    ImGui::EndTable();
  }

  // --- Picture ----------------------------------------------------------------
  if (section("Picture")) {
    columns();
    Row("Renderer",
        "Emulated = the Xbox 360's graphics chip, emulated (the reference picture). Native = this "
        "port's own renderer drawing the game directly (in development). F9 switches between "
        "them while playing, F8 opens the second window.");
    std::string picture = "emulated";
    if (settings::AsBool(Value("emulated_only"))) {
      picture = "emulated_only";
    } else if (settings::AsBool(Value("native_only"))) {
      picture = "native_only";
    } else if (settings::AsBool(Value("native_window"))) {
      picture = "dual";
    } else if (Value("renderer") == "native") {
      picture = "native";
    }
    const char* shown = picture.c_str();
    for (const auto& choice : kPictures) {
      if (picture == choice.id) {
        shown = choice.label;
      }
    }
    if (ImGui::BeginCombo("##picture", shown)) {
      for (const auto& choice : kPictures) {
        if (ImGui::Selectable(choice.label, picture == choice.id)) {
          const std::string id = choice.id;
          SetValue("renderer", id == "native" ? "native" : "emulated");
          SetValue("native_only", id == "native_only" ? "true" : "false");
          SetValue("native_window", id == "dual" ? "true" : "false");
          SetValue("emulated_only", id == "emulated_only" ? "true" : "false");
        }
      }
      ImGui::EndCombo();
    }
    ImGui::TableNextColumn();
    const Warning warning = WarningFor("picture", picture, [](const std::string&) {
      return std::string();
    });
    if (warning.label) {
      WarningIcon(warning, look_->warn);
      ImGui::SameLine();
    }
    bool any_set = false;
    for (const char* flag : kPictureFlags) {
      any_set |= values_.IsSet(flag);
    }
    if (any_set && ImGui::SmallButton("Default##picture")) {
      std::string error;
      for (const char* flag : kPictureFlags) {
        values_.Reset(flag, &error);
      }
    }
    ImGui::EndTable();
  }

  // --- Window -----------------------------------------------------------------
  if (section("Window")) {
    columns();
    check("Fullscreen", "fullscreen", "Start in fullscreen (borderless, at the desktop's mode).");
    ImGui::EndTable();
  }

  // --- Co-op ------------------------------------------------------------------
  if (section("Co-op")) {
    columns();
    Row("Players", "How many players can play at once. The original game has 2; players join "
                   "from the pause menu (Join Game).");
    const int players = int(settings::AsNumber(Value("local_players")));
    const char* labels[] = {"2 (the original)", "3", "4"};
    const int index = std::clamp(players - 2, 0, 2);
    if (ImGui::BeginCombo("##players", labels[index])) {
      for (int i = 0; i < 3; ++i) {
        if (ImGui::Selectable(labels[i], i == index)) {
          SetValue("local_players", std::to_string(i + 2));
        }
      }
      ImGui::EndCombo();
    }
    Status("local_players");
    check("Co-op camera", "coop_camera",
          "Keep every player in the picture: the camera centres between the players and backs "
          "off until everyone fits. Off = the original camera, which follows one player.");
    if (settings::AsBool(Value("coop_camera"))) {
      slider("   Farthest zoom-out", "coop_camera_max_distance", 20, 80, "%.0f",
             "How far the co-op camera may back off to fit everyone (the original camera's "
             "range is about 12-20). Past it, a player who runs off leaves the screen.");
      slider("   Group focus", "coop_camera_group_focus", 0, 4, "%.1f",
             "3-4 players: how much more a group of players counts than a player on their own "
             "when the camera picks its centre. 0 = everyone the same.");
    }
    check("Lost controller pause", "lost_controller",
          "When a player's controller disconnects in a level: pause with a question box, so "
          "another player can drop that player out, or reconnect it and resume.");
    ImGui::EndTable();
  }

  // --- Controls and saves -----------------------------------------------------
  if (section("Controls")) {
    columns();
    check("Keyboard and mouse", "keyboard_mouse",
          "Play with keyboard and mouse (a controller keeps working too). The keys are changed "
          "in the game with F6.");
    ImGui::EndTable();
  }

  // --- Sound and logging ------------------------------------------------------
  if (section("Sound and logging")) {
    columns();
    check("Mute the sound", "audio_mute", nullptr);
    check("Frame rate in the log", "debug_log_fps",
          "Every 5 s the log gets the average frame rate and the 1% / 0.1% lows (the Game log "
          "tab shows them; the report shows the session's at the end).");
    check("Sound names in the log", "debug_audio_trace",
          "The log names every sound the game plays (handy to report a missing voice line).");
    ImGui::EndTable();
  }
}

// ----------------------------------------------------------------------------
// All settings
// ----------------------------------------------------------------------------

void Page::SettingControl(const settings::Setting& setting) {
  const std::string value = values_.Get(setting);
  ImGui::PushID(setting.name.c_str());
  ImGui::SetNextItemWidth(-FLT_MIN);
  static std::map<std::string, std::string> editing;  // text being typed, per setting
  switch (setting.type) {
    case settings::Type::kBool: {
      bool on = settings::AsBool(value);
      if (ImGui::Checkbox("##v", &on)) {
        SetValue(setting.name, on ? "true" : "false");
      }
      break;
    }
    default: {
      if (!setting.allowed.empty()) {
        if (ImGui::BeginCombo("##v", value.c_str())) {
          for (const std::string& allowed : setting.allowed) {
            if (ImGui::Selectable(allowed.c_str(), allowed == value)) {
              SetValue(setting.name, allowed);
            }
          }
          ImGui::EndCombo();
        }
        break;
      }
      // Numbers and text: typed, written on Enter or leaving the box.
      std::string& text = editing.try_emplace(setting.name, value).first->second;
      char buffer[512];
      std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
      const ImGuiInputTextFlags input_flags =
          setting.type == settings::Type::kString ? ImGuiInputTextFlags_None
                                                  : ImGuiInputTextFlags_CharsScientific;
      ImGui::InputText("##v", buffer, sizeof(buffer), input_flags);
      if (ImGui::IsItemActive()) {
        text = buffer;
      } else {
        text = value;  // not being edited: show the current value
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::string typed = buffer;
        if (setting.type != settings::Type::kString) {
          double number = settings::AsNumber(typed);
          if (setting.min) number = std::max(number, *setting.min);
          if (setting.max) number = std::min(number, *setting.max);
          typed = setting.type == settings::Type::kDouble ? NumberText(number)
                                                          : std::to_string(int64_t(number));
        }
        SetValue(setting.name, typed);
        editing.erase(setting.name);
      }
      break;
    }
  }
  ImGui::PopID();
}

void Page::DrawAll() {
  ImGui::TextColored(look_->muted,
                     "Every setting the game has, straight from the game. Most are for "
                     "development or troubleshooting: change them only if you know what they do.");
  const float unit = ImGui::GetFontSize();
  ImGui::SetNextItemWidth(unit * 18);
  ImGui::InputTextWithHint("##search", "Search names and descriptions...", search_,
                           sizeof(search_));
  ImGui::SameLine();
  ImGui::Checkbox("Show developer settings (debug_...)", &show_debug_);

  const std::string needle = Lower(search_);

  // Group: category -> settings (the catalogue is sorted by name).
  std::map<std::string, std::vector<const settings::Setting*>> groups;
  for (const settings::Setting& setting : catalogue_.all()) {
    if (IsHidden(setting.name)) {
      continue;
    }
    const bool debug = setting.debug_only || setting.name.starts_with("debug_");
    if (debug && !show_debug_ && !values_.IsSet(setting.name)) {
      continue;
    }
    if (!needle.empty() && Lower(setting.name).find(needle) == std::string::npos &&
        Lower(setting.description).find(needle) == std::string::npos) {
      continue;
    }
    groups[setting.category.empty() ? "Other" : setting.category].push_back(&setting);
  }
  for (const auto& [group, list] : groups) {
    const std::string title = (group == "CrashMoM" ? std::string("This port's own") : group) +
                              " (" + std::to_string(list.size()) + ")###" + group;
    if (!ImGui::CollapsingHeader(title.c_str(),
                                 needle.empty() ? ImGuiTreeNodeFlags_None
                                                : ImGuiTreeNodeFlags_DefaultOpen)) {
      continue;
    }
    if (!ImGui::BeginTable(group.c_str(), 3,
                           ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH)) {
      continue;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, unit * 17);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, unit * 13);
    ImGui::TableSetupColumn("about", ImGuiTableColumnFlags_WidthStretch);
    for (const settings::Setting* setting : list) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(setting->name.c_str());
      ImGui::TableNextColumn();
      SettingControl(*setting);
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (values_.IsSet(setting->name)) {
        ImGui::PushID(setting->name.c_str());
        if (ImGui::SmallButton("Default")) {
          std::string error;
          values_.Reset(setting->name, &error);
        }
        ImGui::PopID();
        ImGui::SameLine();
      }
      // The first line of the description; all of it on hover.
      const std::string& about = setting->description;
      const size_t newline = about.find('\n');
      ImGui::TextColored(look_->muted, "%s", about.substr(0, newline).c_str());
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetNextWindowSize({unit * 30, 0});
        ImGui::BeginTooltip();
        ImGui::TextWrapped("%s", about.c_str());
        ImGui::TextColored(look_->muted, "Default: %s%s", setting->default_value.c_str(),
                           setting->init_only ? "  (read at start only)" : "");
        ImGui::EndTooltip();
      }
    }
    ImGui::EndTable();
  }
}

void Page::Draw(const Look& look, bool game_running) {
  look_ = &look;
  if (!loaded_) {
    ImGui::TextColored(look.muted, "Asking the game for its settings...");
    return;
  }
  if (!load_error_.empty()) {
    ImGui::TextColored(look.bad, "%s", load_error_.c_str());
    return;
  }
  values_.ReloadIfChanged();  // the game's F4 menu may have written it

  ImGui::BeginChild("settings", {0, 0}, ImGuiChildFlags_None);
  if (game_running) {
    ImGui::TextColored(look.warn,
                       "The game is running: changes apply the next time it starts. (Its F4 menu "
                       "writes the same file: use one or the other.)");
  } else {
    ImGui::TextColored(look.muted,
                       "Saved right away; they apply the next time the game starts. Hover a name "
                       "for what it does, a triangle for why it's risky.");
  }
  if (!values_.last_error().empty()) {
    ImGui::TextColored(look.bad, "%s", values_.last_error().c_str());
  }
  if (!write_error_.empty()) {
    ImGui::TextColored(look.bad, "%s", write_error_.c_str());
  }
  DrawMain();
  ImGui::Spacing();
  ImGui::Spacing();
  if (ImGui::CollapsingHeader("All settings (advanced)")) {
    DrawAll();
  }
  ImGui::EndChild();
}

}  // namespace settings_page
