// =============================================================================
// settings.h -- the game's settings: what exists, and user/settings.toml
// =============================================================================
//
// TWO HALVES:
//
// 1. THE CATALOGUE: every setting the game has, with its type, default,
//    allowed values, range and description. Only the game knows them (the
//    SDK's flag registry), so the launcher asks it:
//      <exe> --list_settings=<game folder>/cache/launcher/settings_list.toml
//    (src/settings_list.h: written in a fraction of a second, before any
//    window opens). Kept in that file and asked again only when the exe is
//    newer than it (a new build may add settings).
//
// 2. THE PLAYER'S VALUES: user/settings.toml, the file the game loads at
//    every start and the SDK's F4 menu writes (flat `name = value` lines).
//    Only values that differ from the default are kept; a setting set back to
//    its default disappears from the file. Keys the catalogue doesn't know
//    (a newer or older game's) are kept as they are. Written through a
//    temporary file + rename, so a crash mid-write can't leave half a file.
//    Re-read when it changes on disk (F4 in the game writes it too).
//
// SESSION-ONLY FLAGS: `load_save` and `log_file` are given per session (the
// launcher passes them on the command line), but the game's F4 menu saves
// command-line flags into settings.toml as well: a saved load_save would
// jump into that save at every start, a saved log_file sent every session's
// log into one old file (seen). RemoveSessionOnly() drops them; the launcher
// calls it before each start and every write leaves them out.
// =============================================================================
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace settings {

enum class Type { kBool, kInt, kUint, kDouble, kString };

struct Setting {
  std::string name;
  Type type = Type::kString;
  std::string category;          // "CrashMoM" = the port's own, else the SDK's part
  std::string description;
  std::string default_value;     // as text
  std::vector<std::string> allowed;
  std::optional<double> min, max;
  bool restart = false, init_only = false, debug_only = false;
};

// The catalogue (half 1).
class Catalogue {
 public:
  // Reads the cached list, or asks the game for a fresh one when it's missing
  // or older than the exe. Blocking (well under a second); call off the UI
  // thread. False + `error` when the game couldn't be asked.
  bool Load(const std::filesystem::path& exe, const std::filesystem::path& work_folder,
            const std::filesystem::path& cache_file, std::string* error);

  const std::vector<Setting>& all() const { return settings_; }
  const Setting* Find(const std::string& name) const;

 private:
  std::vector<Setting> settings_;
  std::map<std::string, size_t> index_;
};

// The player's values (half 2).
class Values {
 public:
  explicit Values(std::filesystem::path file) : file_(std::move(file)) {}

  // (Re-)reads the file; true if it changed since the last read/write.
  bool ReloadIfChanged();

  // The value as text: the file's, else the default.
  std::string Get(const Setting& setting) const;
  bool IsSet(const std::string& name) const { return values_.count(name) != 0; }
  // Sets a value (the default = removed from the file) and writes the file.
  bool Set(const Setting& setting, const std::string& value, std::string* error);
  bool Reset(const std::string& name, std::string* error);
  // Drops load_save / log_file (see the header). Writes only if one was there.
  void RemoveSessionOnly(const Catalogue& catalogue);

  const std::filesystem::path& file() const { return file_; }
  const std::string& last_error() const { return last_error_; }

 private:
  bool Write(const Catalogue* catalogue, std::string* error);

  std::filesystem::path file_;
  std::map<std::string, std::string> values_;  // name -> value as text
  std::map<std::string, Type> types_;           // remembered for writing
  std::filesystem::file_time_type read_time_{};
  bool read_once_ = false;
  std::string last_error_;
};

// A value as a number / bool (lenient: "true"/"1", "180.000000").
bool AsBool(const std::string& text);
double AsNumber(const std::string& text);

}  // namespace settings
