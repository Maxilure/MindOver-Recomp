// =============================================================================
// xexdis -- disassemble any address range of the game's PowerPC code
// =============================================================================
//
// WHY: when `rexglue codegen` complains about an address ("target not in any
// function", a bad jump table, a weird crash in sub_82xxxxxx...), we need to
// SEE the original machine code there to decide how to fix it.
//
// HOW: we do exactly what `rexglue codegen` does internally:
//   1. boot a ReXGlue runtime in "tool mode" (no window, no GPU, no audio)
//   2. load game:\default.xex, which decrypts + decompresses it and maps
//      the image into guest memory at 0x82000000
//   3. read instructions straight out of guest memory and feed them to
//      the binutils PowerPC disassembler, configured for the Xbox 360's
//      Xenon CPU (PPC64 + AltiVec + Microsoft's VMX128 extension)
//
// USAGE:
//   xexdis <game_dir> <start> [end]
//     start, end  guest addresses in hex, e.g. 0x82170160 0x82170200
//                 (end is exclusive, default = start + 0x80)
//
//   xexdis <game_dir> --dump <out_file>
//     Write the whole decrypted, decompressed image (as loaded at
//     0x82000000) to a raw file, for scripts like
//     tools/find_missing_functions.py. The dump IS game code, so keep it
//     somewhere gitignored (e.g. out/).
//
//   Example:
//   out/build/linux-amd64-relwithdebinfo/xexdis game 0x82170100 0x82170200
//
// OUTPUT: one line per instruction:
//   82170160  7d8802a6  mflr r12
//   ^address  ^raw word ^decoded instruction
// A blank line follows every unconditional branch / return (b, blr, bctr),
// so basic blocks and function ends are easy to spot.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>

// binutils disassembler (compiled into the rex::disasm library)
#include <dis-asm.h>

// The SDK's X_STATUS_SUCCESS macro casts to an unqualified X_STATUS.
using rex::X_STATUS;

namespace {

// Which instruction sets to decode. These are binutils PPC_OPCODE_* bits;
// the combination below is the one ReXGlue's own codegen uses for Xenon.
constexpr uintptr_t kXenonDialect = 0x1          // base PowerPC
                                    | 0x4        // 64-bit instructions
                                    | 0x4000     // POWER4 additions
                                    | 0x8000000  // Cell (same PPE core family as Xenon)
                                    | 0x200      // AltiVec / VMX
                                    | 0x1000000  // VMX128 (Xbox 360-only vector extension)
                                    | 0x10000;   // classic PPC mnemonics

// Parses "0x82170160" or "82170160".
uint32_t ParseAddress(const char* text) {
  return static_cast<uint32_t>(std::strtoul(text, nullptr, 16));
}

// The instruction word is big-endian in guest memory; the host (x86) is little-endian.
uint32_t LoadBigEndian32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Does this instruction end a straight-line run of code? (b, blr, bctr, but
// not the "and link" call forms bl/blrl/bctrl, which return to the next line)
bool EndsBlock(uint32_t insn) {
  uint32_t primary = insn >> 26;
  if (primary == 18) return (insn & 1) == 0;  // b / ba (unconditional, no link)
  if (primary == 19) {
    uint32_t ext = (insn >> 1) & 0x3FF;
    uint32_t bo = (insn >> 21) & 0x1F;
    bool always = (bo & 0x14) == 0x14;       // BO = "branch always"
    bool link = insn & 1;
    return always && !link && (ext == 16 || ext == 528);  // blr / bctr
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <game_dir> <start_hex> [end_hex]\n"
                 "       %s <game_dir> --dump <out_file>\n",
                 argv[0], argv[0]);
    return 1;
  }
  const std::string game_dir = argv[1];
  const bool dump_mode = std::strcmp(argv[2], "--dump") == 0;
  if (dump_mode && argc < 4) {
    std::fprintf(stderr, "xexdis: --dump needs an output file\n");
    return 1;
  }

  // --- 1+2: headless runtime, load + decrypt the XEX into guest memory ------
  auto runtime = std::make_unique<rex::Runtime>(game_dir);
  if (runtime->Setup(rex::RuntimeConfig{
          .kernel_init = rex::kernel::InitializeKernel,
          .tool_mode = true,  // no graphics/audio/window
      }) != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "xexdis: runtime setup failed\n");
    return 1;
  }
  if (runtime->LoadXexImage("game:\\default.xex") != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "xexdis: could not load game:\\default.xex from %s\n", game_dir.c_str());
    return 1;
  }

  // --- dump mode: write the loaded image to a file and stop ------------------
  if (dump_mode) {
    auto module = runtime->kernel_state()->GetExecutableModule();
    const uint32_t base = module->xex_module()->base_address();
    const uint32_t size = module->xex_module()->image_size();
    const uint8_t* image = runtime->memory()->TranslateVirtual<const uint8_t*>(base);
    FILE* out = std::fopen(argv[3], "wb");
    if (!out || std::fwrite(image, 1, size, out) != size) {
      std::fprintf(stderr, "xexdis: failed writing %s\n", argv[3]);
      return 1;
    }
    std::fclose(out);
    std::printf("dumped 0x%08X-0x%08X (%u bytes) to %s\n", base, base + size, size, argv[3]);
    return 0;
  }

  const uint32_t start = ParseAddress(argv[2]) & ~3u;  // instructions are 4-byte aligned
  const uint32_t end = argc > 3 ? ParseAddress(argv[3]) : start + 0x80;

  // --- 3: set up binutils for big-endian Xenon --------------------------------
  disassemble_info info{};
  INIT_DISASSEMBLE_INFO(info, stdout, fprintf);
  info.arch = bfd_arch_powerpc;
  info.endian = BFD_ENDIAN_BIG;
  info.private_data = reinterpret_cast<void*>(kXenonDialect);

  for (uint32_t addr = start; addr < end; addr += 4) {
    const uint8_t* code = runtime->memory()->TranslateVirtual<const uint8_t*>(addr);
    const uint32_t word = LoadBigEndian32(code);

    ppc_insn insn{};
    info.buffer = const_cast<bfd_byte*>(code);
    info.buffer_vma = addr;
    info.buffer_length = 4;
    decode_insn_ppc(addr, &info, &insn);

    if (insn.opcode) {
      std::printf("%08X  %08X  %-8s %s\n", addr, word, insn.opcode->name, insn.op_str);
    } else {
      // Not a valid instruction: usually data (jump table, constant pool, padding)
      std::printf("%08X  %08X  .long    0x%08X\n", addr, word, word);
    }
    if (EndsBlock(word)) std::printf("\n");
  }
  return 0;
}
