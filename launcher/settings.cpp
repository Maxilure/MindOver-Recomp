// =============================================================================
// settings.cpp -- see settings.h
// =============================================================================
#include "settings.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>


#include <toml++/toml.hpp>

#include "platform.h"

namespace fs = std::filesystem;

namespace settings {
namespace {

constexpr const char* kSessionOnly[] = {"load_save", "log_file"};

bool IsSessionOnly(const std::string& name) {
  for (const char* key : kSessionOnly) {
    if (name == key) {
      return true;
    }
  }
  return false;
}

Type TypeFromName(const std::string& name) {
  if (name == "bool") return Type::kBool;
  if (name == "int32" || name == "int64") return Type::kInt;
  if (name == "uint32" || name == "uint64") return Type::kUint;
  if (name == "double") return Type::kDouble;
  return Type::kString;
}

// Runs `argv` in `work_folder` with no output, waits; its exit code (-1 if
// it couldn't run, crashed or was stopped).
int RunQuietly(const std::vector<std::string>& args, const fs::path& work_folder) {
  bool ok = false;
  platform::Capture(args, &ok, work_folder, 60000);
  return ok ? 0 : -1;
}

// A TOML value as the text the game's own parser would make of it.
std::string NodeText(const toml::node& node, Type* type) {
  if (auto v = node.as_boolean()) {
    *type = Type::kBool;
    return v->get() ? "true" : "false";
  }
  if (auto v = node.as_integer()) {
    *type = Type::kInt;
    return std::to_string(v->get());
  }
  if (auto v = node.as_floating_point()) {
    *type = Type::kDouble;
    std::ostringstream text;
    text << v->get();
    return text.str();
  }
  if (auto v = node.as_string()) {
    *type = Type::kString;
    return v->get();
  }
  *type = Type::kString;  // arrays, tables, dates: no setting uses them
  return std::string();
}

// Two values the same once both are read as their type ("180.000000" = "180").
bool SameValue(Type type, const std::string& a, const std::string& b) {
  switch (type) {
    case Type::kBool: return AsBool(a) == AsBool(b);
    case Type::kInt:
    case Type::kUint:
    case Type::kDouble: return AsNumber(a) == AsNumber(b);
    case Type::kString: return a == b;
  }
  return a == b;
}

}  // namespace

bool AsBool(const std::string& text) {
  return text == "true" || text == "1" || text == "yes" || text == "on";
}

double AsNumber(const std::string& text) {
  return std::strtod(text.c_str(), nullptr);
}

// -----------------------------------------------------------------------------
// The catalogue
// -----------------------------------------------------------------------------

bool Catalogue::Load(const fs::path& exe, const fs::path& work_folder, const fs::path& cache_file,
                     std::string* error) {
  std::error_code ec;
  const auto exe_time = fs::last_write_time(exe, ec);
  if (ec) {
    *error = "The game isn't built yet: its settings come from the game itself.";
    return false;
  }
  const auto cache_time = fs::last_write_time(cache_file, ec);
  if (ec || cache_time < exe_time) {
    fs::create_directories(cache_file.parent_path(), ec);
    const int code = RunQuietly({exe.string(), "--list_settings=" + cache_file.string(),
                                 #if defined(_WIN32)
                                 "--log_file=NUL"
#else
                                 "--log_file=/dev/null"
#endif
},
                                work_folder);
    if (code != 0) {
      *error = "The game couldn't list its settings (exit code " + std::to_string(code) +
               "). A build from before 2026-10-06 doesn't know --list_settings: rebuild it.";
      return false;
    }
  }
  toml::table file;
  try {
    file = toml::parse_file(cache_file.string());
  } catch (const toml::parse_error& err) {
    *error = std::string("The settings list is damaged: ") + err.what();
    return false;
  }
  settings_.clear();
  index_.clear();
  if (const toml::array* list = file["setting"].as_array()) {
    for (const toml::node& node : *list) {
      const toml::table* entry = node.as_table();
      if (!entry) {
        continue;
      }
      Setting setting;
      setting.name = (*entry)["name"].value_or(std::string());
      setting.type = TypeFromName((*entry)["type"].value_or(std::string()));
      setting.category = (*entry)["category"].value_or(std::string());
      setting.description = (*entry)["description"].value_or(std::string());
      setting.default_value = (*entry)["default"].value_or(std::string());
      if (const toml::array* allowed = (*entry)["allowed"].as_array()) {
        for (const toml::node& value : *allowed) {
          setting.allowed.push_back(value.value_or(std::string()));
        }
      }
      if (auto min = (*entry)["min"].value<double>()) setting.min = *min;
      if (auto max = (*entry)["max"].value<double>()) setting.max = *max;
      setting.restart = (*entry)["restart"].value_or(false);
      setting.init_only = (*entry)["init_only"].value_or(false);
      setting.debug_only = (*entry)["debug_only"].value_or(false);
      if (!setting.name.empty()) {
        index_[setting.name] = settings_.size();
        settings_.push_back(std::move(setting));
      }
    }
  }
  if (settings_.empty()) {
    *error = "The settings list is empty.";
    return false;
  }
  return true;
}

const Setting* Catalogue::Find(const std::string& name) const {
  const auto it = index_.find(name);
  return it == index_.end() ? nullptr : &settings_[it->second];
}

// -----------------------------------------------------------------------------
// The player's values
// -----------------------------------------------------------------------------

bool Values::ReloadIfChanged() {
  std::error_code ec;
  const auto time = fs::last_write_time(file_, ec);
  if (ec) {
    // No file (yet): everything at its default.
    const bool changed = !values_.empty();
    values_.clear();
    types_.clear();
    read_once_ = true;
    return changed;
  }
  if (read_once_ && time == read_time_) {
    return false;
  }
  try {
    const toml::table table = toml::parse_file(file_.string());
    values_.clear();
    types_.clear();
    for (const auto& [key, node] : table) {
      Type type;
      const std::string name(key.str());
      values_[name] = NodeText(node, &type);
      types_[name] = type;
    }
    last_error_.clear();
  } catch (const toml::parse_error& err) {
    // Keep what we had; say why (a hand edit gone wrong).
    last_error_ = std::string("user/settings.toml can't be read: ") + err.what();
  }
  read_time_ = time;
  read_once_ = true;
  return true;
}

std::string Values::Get(const Setting& setting) const {
  const auto it = values_.find(setting.name);
  return it == values_.end() ? setting.default_value : it->second;
}

bool Values::Set(const Setting& setting, const std::string& value, std::string* error) {
  ReloadIfChanged();  // don't overwrite what F4 just wrote with an old copy
  if (SameValue(setting.type, value, setting.default_value)) {
    values_.erase(setting.name);
    types_.erase(setting.name);
  } else {
    values_[setting.name] = value;
    types_[setting.name] = setting.type;
  }
  return Write(nullptr, error);
}

bool Values::Reset(const std::string& name, std::string* error) {
  ReloadIfChanged();
  values_.erase(name);
  types_.erase(name);
  return Write(nullptr, error);
}

void Values::RemoveSessionOnly(const Catalogue& catalogue) {
  ReloadIfChanged();
  bool found = false;
  for (const char* key : kSessionOnly) {
    found |= values_.erase(key) != 0;
    types_.erase(key);
  }
  if (found) {
    std::string ignored;
    Write(&catalogue, &ignored);
  }
}

bool Values::Write(const Catalogue*, std::string* error) {
  toml::table table;
  for (const auto& [name, text] : values_) {
    if (IsSessionOnly(name)) {
      continue;
    }
    const auto type_it = types_.find(name);
    const Type type = type_it == types_.end() ? Type::kString : type_it->second;
    switch (type) {
      case Type::kBool:
        table.insert(name, AsBool(text));
        break;
      case Type::kInt:
      case Type::kUint:
        table.insert(name, int64_t(std::strtoll(text.c_str(), nullptr, 0)));
        break;
      case Type::kDouble:
        table.insert(name, AsNumber(text));
        break;
      case Type::kString:
        table.insert(name, text);
        break;
    }
  }
  std::error_code ec;
  fs::create_directories(file_.parent_path(), ec);
  const fs::path temp = file_.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    out << "# The game's settings: only the ones changed from their defaults.\n"
           "# Written by the launcher's Settings tab and the game's F4 menu;\n"
           "# a line can be deleted to put that setting back to its default.\n";
    out << table << "\n";
    if (!out) {
      *error = "Couldn't write " + temp.string();
      last_error_ = *error;
      return false;
    }
  }
  fs::rename(temp, file_, ec);
  if (ec) {
    *error = "Couldn't replace " + file_.string() + ": " + ec.message();
    last_error_ = *error;
    return false;
  }
  read_time_ = fs::last_write_time(file_, ec);
  last_error_.clear();
  return true;
}

}  // namespace settings
