// =============================================================================
// settings_list.cpp -- see settings_list.h
// =============================================================================
#include "settings_list.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <toml++/toml.hpp>

REXCVAR_DEFINE_STRING(list_settings, "", "CrashMoM",
                      "Write every setting (name, type, default, allowed values, description) to "
                      "this TOML file and quit at once (used by the launcher's Settings tab)");

namespace settings_list {
namespace {

const char* TypeName(rex::cvar::FlagType type) {
  using rex::cvar::FlagType;
  switch (type) {
    case FlagType::Boolean: return "bool";
    case FlagType::Int32: return "int32";
    case FlagType::Int64: return "int64";
    case FlagType::Uint32: return "uint32";
    case FlagType::Uint64: return "uint64";
    case FlagType::Double: return "double";
    case FlagType::String: return "string";
    case FlagType::Command: return "command";
  }
  return "?";
}

}  // namespace

void WriteAndQuitIfAsked() {
  const std::string path = REXCVAR_GET(list_settings);
  if (path.empty()) {
    return;
  }
  // A copy, sorted by name (the registry is in registration order).
  std::vector<const rex::cvar::FlagEntry*> entries;
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.type != rex::cvar::FlagType::Command && entry.name != "list_settings") {
      entries.push_back(&entry);
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto* a, const auto* b) { return a->name < b->name; });

  toml::array settings;
  for (const auto* entry : entries) {
    toml::table setting{
        {"name", entry->name},
        {"type", TypeName(entry->type)},
        {"category", entry->category},
        {"description", entry->description},
        {"default", entry->default_value},
    };
    if (entry->constraints.HasAllowedValues()) {
      toml::array allowed;
      for (const auto& value : entry->constraints.allowed_values) {
        allowed.push_back(value);
      }
      setting.insert("allowed", std::move(allowed));
    }
    if (entry->constraints.min) {
      setting.insert("min", *entry->constraints.min);
    }
    if (entry->constraints.max) {
      setting.insert("max", *entry->constraints.max);
    }
    if (entry->lifecycle == rex::cvar::Lifecycle::kRequiresRestart) {
      setting.insert("restart", true);
    }
    if (entry->lifecycle == rex::cvar::Lifecycle::kInitOnly) {
      setting.insert("init_only", true);
    }
    if (entry->is_debug_only) {
      setting.insert("debug_only", true);
    }
    settings.push_back(std::move(setting));
  }
  toml::table file{{"setting", std::move(settings)}};

  std::ofstream out(path, std::ios::trunc);
  out << "# Every setting of the game (crash_mom --list_settings, src/settings_list.h)\n";
  out << file << "\n";
  out.close();
  const bool ok = bool(out);
  REXLOG_INFO("list_settings: {} settings -> {} ({})", entries.size(), path,
              ok ? "written" : "FAILED");
  std::fflush(nullptr);
  // Straight out: nothing else has started yet (no window, audio, threads of
  // the game), and the SDK's normal shutdown expects a running runtime.
  std::_Exit(ok ? 0 : 1);
}

}  // namespace settings_list
