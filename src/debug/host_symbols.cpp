// =============================================================================
// debug/host_symbols.cpp -- see host_symbols.h
// =============================================================================
#include "debug/host_symbols.h"

#if defined(__linux__)

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

namespace host_symbols {
namespace {

// One function of the executable: [start, start + size), name in g_names.
struct Symbol {
  uint64_t start;  // runtime address (load base already added)
  uint64_t size;
  uint32_t name;   // offset into g_names
};

std::once_flag g_once;
bool g_loaded = false;
uint64_t g_base = 0;                  // where the PIE executable was loaded
uint64_t g_exe_end = 0;               // end of its highest segment (runtime address)
std::vector<Symbol> g_symbols;        // sorted by start
std::vector<char> g_names;            // the .strtab section, as read
std::string g_exe_path;               // /proc/self/exe resolved (for llvm-symbolizer)

// dl_iterate_phdr lists the main program first, with an empty name.
int FindMainProgram(dl_phdr_info* info, size_t, void*) {
  g_base = info->dlpi_addr;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const auto& ph = info->dlpi_phdr[i];
    if (ph.p_type == PT_LOAD) g_exe_end = std::max<uint64_t>(g_exe_end, g_base + ph.p_vaddr + ph.p_memsz);
  }
  return 1;  // stop after the first entry
}

bool ReadAt(int fd, uint64_t offset, void* out, size_t size) {
  auto* p = static_cast<char*>(out);
  while (size) {
    const ssize_t n = pread(fd, p, size, off_t(offset));
    if (n <= 0) return false;
    p += n, offset += uint64_t(n), size -= size_t(n);
  }
  return true;
}

void LoadOnce() {
  dl_iterate_phdr(FindMainProgram, nullptr);
  char path[4096];
  const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len > 0) g_exe_path.assign(path, size_t(len));

  const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  Elf64_Ehdr eh;
  std::vector<Elf64_Shdr> sections;
  if (ReadAt(fd, 0, &eh, sizeof(eh)) && std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
      eh.e_ident[EI_CLASS] == ELFCLASS64 && eh.e_shentsize == sizeof(Elf64_Shdr)) {
    sections.resize(eh.e_shnum);
    if (!ReadAt(fd, eh.e_shoff, sections.data(), sections.size() * sizeof(Elf64_Shdr))) {
      sections.clear();
    }
  }
  for (const Elf64_Shdr& sh : sections) {
    if (sh.sh_type != SHT_SYMTAB || sh.sh_link >= sections.size()) continue;
    const Elf64_Shdr& strtab = sections[sh.sh_link];
    std::vector<Elf64_Sym> syms(sh.sh_size / sizeof(Elf64_Sym));
    g_names.resize(strtab.sh_size);
    if (!ReadAt(fd, sh.sh_offset, syms.data(), syms.size() * sizeof(Elf64_Sym)) ||
        !ReadAt(fd, strtab.sh_offset, g_names.data(), g_names.size())) {
      g_names.clear();
      break;
    }
    for (const Elf64_Sym& s : syms) {
      if (ELF64_ST_TYPE(s.st_info) != STT_FUNC || s.st_value == 0 || s.st_size == 0 ||
          s.st_name >= g_names.size()) {
        continue;
      }
      g_symbols.push_back({g_base + s.st_value, s.st_size, s.st_name});
    }
    break;
  }
  close(fd);
  // Same start twice = an alias (the generated `sub_X` is a weak alias of
  // `__imp__sub_X`): either name says the same, the sort keeps them adjacent.
  std::sort(g_symbols.begin(), g_symbols.end(),
            [](const Symbol& a, const Symbol& b) { return a.start < b.start; });
  g_loaded = !g_symbols.empty();
}

// The symbol containing the address, or null. Functions don't overlap, so the
// last one starting at or before it is the only candidate.
const Symbol* Find(uint64_t address) {
  if (!g_loaded) return nullptr;
  auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), address,
                             [](uint64_t a, const Symbol& s) { return a < s.start; });
  if (it == g_symbols.begin()) return nullptr;
  --it;
  return address < it->start + it->size ? &*it : nullptr;
}

// "0x82177D38" from "__imp__sub_82177D38" / "sub_82177D38"; 0 otherwise.
uint32_t GuestFromName(const char* name) {
  if (std::strncmp(name, "__imp__", 7) == 0) name += 7;
  if (std::strncmp(name, "sub_", 4) != 0) return 0;
  uint32_t v = 0;
  int digits = 0;
  for (const char* p = name + 4; *p; ++p, ++digits) {
    const char c = *p;
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0 || digits == 8) return 0;
    v = v << 4 | uint32_t(d);
  }
  return digits == 8 ? v : 0;
}

}  // namespace

bool Load() {
  std::call_once(g_once, LoadOnce);
  return g_loaded;
}

void Describe(uint64_t address, char* out, size_t size) {
  if (!size) return;
  if (const Symbol* s = Find(address)) {
    std::snprintf(out, size, "%s+0x%llx", &g_names[s->name],
                  static_cast<unsigned long long>(address - s->start));
    return;
  }
  if (address >= g_base && address < g_exe_end) {
    std::snprintf(out, size, "crash_mom+0x%llx", static_cast<unsigned long long>(address - g_base));
    return;
  }
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fname) {
    const char* file = std::strrchr(info.dli_fname, '/');
    file = file ? file + 1 : info.dli_fname;
    if (info.dli_sname) {
      std::snprintf(out, size, "%s!%s+0x%llx", file, info.dli_sname,
                    static_cast<unsigned long long>(address - uint64_t(info.dli_saddr)));
    } else {
      std::snprintf(out, size, "%s+0x%llx", file,
                    static_cast<unsigned long long>(address - uint64_t(info.dli_fbase)));
    }
    return;
  }
  std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
}

uint32_t GuestFunction(uint64_t address) {
  const Symbol* s = Find(address);
  return s ? GuestFromName(&g_names[s->name]) : 0;
}

uint64_t ExeOffset(uint64_t address) {
  return address >= g_base && address < g_exe_end ? address - g_base : 0;
}

std::vector<GuestInstruction> GuestInstructions(const std::vector<uint64_t>& host_addresses) {
  std::vector<GuestInstruction> result(host_addresses.size());
  Load();
  if (g_exe_path.empty()) return result;

  // 1. Which generated source line each address came from. llvm-symbolizer
  //    prints, per address, its inline chain (function line + "file:line:col"
  //    line per frame, innermost first) and then a blank line. The frame we
  //    want is the first one in a generated/ file (inlined SDK helpers such as
  //    the condition-register compare come before it).
  std::string command = "llvm-symbolizer --inlining --obj='" + g_exe_path + "'";
  std::vector<size_t> asked;  // which result each queried address fills
  for (size_t i = 0; i < host_addresses.size(); ++i) {
    const uint64_t offset = ExeOffset(host_addresses[i]);
    if (!offset) continue;
    char hex[32];
    std::snprintf(hex, sizeof(hex), " 0x%llx", static_cast<unsigned long long>(offset));
    command += hex;
    asked.push_back(i);
  }
  if (asked.empty()) return result;
  command += " 2>/dev/null";
  struct Where {
    std::string file;
    long line = 0;
  };
  std::vector<Where> where(asked.size());
  if (FILE* pipe = popen(command.c_str(), "r")) {
    size_t index = 0;
    bool odd = false;  // lines alternate: function name, then file:line:col
    char text[8192];
    while (index < asked.size() && std::fgets(text, sizeof(text), pipe)) {
      std::string line(text);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      if (line.empty()) {  // end of this address's frames
        ++index;
        odd = false;
        continue;
      }
      odd = !odd;
      if (odd || !where[index].file.empty()) continue;  // a function name / already found
      if (line.find("/generated/") == std::string::npos) continue;
      // "file:line:col" (the file itself has no ':' on Linux paths here)
      const size_t c2 = line.rfind(':');
      const size_t c1 = c2 == std::string::npos ? c2 : line.rfind(':', c2 - 1);
      if (c1 == std::string::npos) continue;
      where[index].file = line.substr(0, c1);
      where[index].line = std::strtol(line.c_str() + c1 + 1, nullptr, 10);
    }
    pclose(pipe);
  }

  // 2. Read each file once, up to its highest wanted line, counting the asm
  //    comments from the last anchor (see host_symbols.h).
  std::map<std::string, std::vector<size_t>> by_file;  // file -> indexes into `where`
  for (size_t k = 0; k < where.size(); ++k) {
    if (!where[k].file.empty() && where[k].line > 0) by_file[where[k].file].push_back(k);
  }
  for (auto& [file, ks] : by_file) {
    std::sort(ks.begin(), ks.end(), [&](size_t a, size_t b) { return where[a].line < where[b].line; });
    std::ifstream in(file);
    if (!in) continue;
    std::string line;
    long number = 0;
    uint32_t anchor = 0, count = 0, current = 0;
    std::string current_text;
    size_t next = 0;
    while (next < ks.size() && std::getline(in, line)) {
      ++number;
      // Anchors: "DEFINE_REX_FUNC(sub_820B0038) {" and "loc_820B0064:".
      size_t p = line.find("(sub_");
      if (line.rfind("DEFINE_REX_FUNC", 0) == 0 && p != std::string::npos) {
        anchor = GuestFromName(line.substr(p + 1, 12).c_str());
        count = 0, current = 0;
      } else if (line.rfind("loc_", 0) == 0 && line.size() >= 13 && line[12] == ':') {
        anchor = GuestFromName(("sub_" + line.substr(4, 8)).c_str());
        count = 0, current = 0;
      } else {
        const size_t first = line.find_first_not_of(" \t");
        if (anchor && first != std::string::npos && line.compare(first, 3, "// ") == 0) {
          current = anchor + 4 * count++;
          current_text = line.substr(first + 3);
        }
      }
      while (next < ks.size() && where[ks[next]].line == number) {
        GuestInstruction& out = result[asked[ks[next]]];
        out.address = current;
        if (current) out.text = current_text;
        ++next;
      }
    }
  }
  return result;
}

}  // namespace host_symbols

#else  // not Linux: see host_symbols.h

#include <cstdio>

namespace host_symbols {
bool Load() { return false; }
void Describe(uint64_t address, char* out, size_t size) {
  if (size) std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
}
uint32_t GuestFunction(uint64_t) { return 0; }
uint64_t ExeOffset(uint64_t) { return 0; }
std::vector<GuestInstruction> GuestInstructions(const std::vector<uint64_t>& host_addresses) {
  return std::vector<GuestInstruction>(host_addresses.size());
}
}  // namespace host_symbols

#endif
