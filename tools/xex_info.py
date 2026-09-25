#!/usr/bin/env python3
"""
xex_info.py -- print everything interesting from an Xbox 360 XEX2 header.

WHAT IS A XEX?
--------------
default.xex is the game's executable. Under the hood it's a normal Windows
PE (.exe) compiled for the Xbox 360's PowerPC CPU ("Xenon"), wrapped in a
Microsoft container called XEX2 that adds:
  * signing/encryption (the PE image is AES-encrypted on retail discs)
  * compression (usually LZX)
  * a list of "optional headers" describing how the console should load it

We do NOT decrypt anything here -- `rexglue codegen` does that itself when
it translates the code. The header (everything before the PE image) is
plain text, and it already tells us a lot about what the port will need:

  * entry point + image base      -> where the code lives in memory
  * page descriptors              -> which address ranges are code vs data
  * static libraries              -> the exact XDK (Xbox SDK) version, and
                                     middleware linked into the exe (D3D,
                                     XAudio, XACT, Bink video, networking...)
  * import libraries              -> OS functions the game calls into
                                     (xboxkrnl.exe = kernel, xam.xex = system
                                     UI/profiles/achievements). ReXGlue's
                                     runtime must provide every one of them.

FORMAT CHEAT-SHEET (all numbers are BIG-endian -- PowerPC)
----------------------------------------------------------
    0x00  "XEX2"
    0x04  module flags
    0x08  offset of the PE image (== size of all headers)
    0x10  offset of the security info block
    0x14  number of optional headers
    0x18  optional headers: array of (u32 key, u32 value)

Optional-header key encoding: the LOW BYTE of the key says how to read value:
    0x00 / 0x01   -> `value` IS the data (a single u32)
    0xFF          -> `value` is a file offset; first u32 there = size in bytes
    anything else -> `value` is a file offset; size = low_byte * 4 bytes

Field layouts follow the Xenia emulator's xex2_info.h (the de-facto public
reference for this format).

USAGE
-----
    python3 tools/xex_info.py game/default.xex
"""

import struct
import sys

# ---------------------------------------------------------------------------
# Optional header keys we understand. Names match Xenia / the XDK.
# ---------------------------------------------------------------------------
KEYS = {
    0x000002FF: "RESOURCE_INFO",
    0x000003FF: "FILE_FORMAT_INFO",
    0x00000405: "BASE_REFERENCE",
    0x000005FF: "DELTA_PATCH_DESCRIPTOR",
    0x000080FF: "BOUNDING_PATH",
    0x00008105: "DEVICE_ID",
    0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP",
    0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP",
    0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE",
    0x00020301: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE",
    0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS",
    0x00030000: "SYSTEM_FLAGS",
    0x00030100: "SYSTEM_FLAGS_32",
    0x00040006: "EXECUTION_INFO",
    0x000401FF: "SERVICE_ID_LIST",
    0x00040201: "TITLE_WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}

ENCRYPTION = {0: "none", 1: "AES (retail)"}
COMPRESSION = {0: "none", 1: "basic (zero-fill runs)", 2: "normal (LZX)", 3: "delta patch"}
SECTION_TYPES = {1: "code", 2: "data", 3: "read-only data"}

# The page descriptor list lives inside the security info block.
SEC_IMAGE_SIZE = 0x004
SEC_IMAGE_FLAGS = 0x10C
SEC_LOAD_ADDRESS = 0x110
SEC_REGION = 0x178
SEC_PAGE_DESC_COUNT = 0x180
SEC_PAGE_DESCS = 0x184
PAGE_DESC_SIZE = 4 + 20        # u32 (info:4 | page_count:28) + SHA-1 digest
IMAGE_FLAG_4KB_PAGES = 0x10000000


def u16(b, o): return struct.unpack_from(">H", b, o)[0]
def u32(b, o): return struct.unpack_from(">I", b, o)[0]


def version_str(v):
    """XEX versions are packed: major:4 minor:4 build:16 qfe:8 (from the top bit)."""
    return f"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


class Xex:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"XEX2":
            raise ValueError(f"not a XEX2 file (magic {d[:4]!r})")
        self.module_flags = u32(d, 0x04)
        self.pe_offset = u32(d, 0x08)
        self.security_offset = u32(d, 0x10)

        # Collect optional headers as {key: (raw_value, payload_bytes_or_None)}
        self.headers = {}
        for i in range(u32(d, 0x14)):
            key, value = struct.unpack_from(">II", d, 0x18 + i * 8)
            size_code = key & 0xFF
            if size_code in (0x00, 0x01):
                payload = None                      # value is the data itself
            elif size_code == 0xFF:
                payload = d[value:value + u32(d, value)]
            else:
                payload = d[value:value + size_code * 4]
            self.headers[key] = (value, payload)

    def value(self, name):
        key = next(k for k, n in KEYS.items() if n == name)
        return self.headers.get(key, (None, None))[0]

    def payload(self, name):
        key = next(k for k, n in KEYS.items() if n == name)
        return self.headers.get(key, (None, None))[1]

    # --- decoders for the structured headers -----------------------------

    def execution_info(self):
        p = self.payload("EXECUTION_INFO")
        if not p:
            return None
        media_id, ver, base_ver, title_id = struct.unpack_from(">IIII", p, 0)
        platform, exe_table, disc_no, disc_count = struct.unpack_from(">BBBB", p, 16)
        return {
            "title_id": title_id, "media_id": media_id,
            "version": version_str(ver), "base_version": version_str(base_ver),
            "disc": f"{disc_no}/{disc_count}",
        }

    def file_format(self):
        p = self.payload("FILE_FORMAT_INFO")
        if not p:
            return None
        return ENCRYPTION.get(u16(p, 4), u16(p, 4)), COMPRESSION.get(u16(p, 6), u16(p, 6))

    def static_libraries(self):
        """16-byte records: char name[8]; u16 major, minor, build; u8 approval; u8 qfe"""
        p = self.payload("STATIC_LIBRARIES")
        if not p:
            return []
        libs = []
        for off in range(4, len(p) - 15, 16):
            name = cstr(p[off:off + 8])
            major, minor, build = struct.unpack_from(">HHH", p, off + 8)
            approval, qfe = p[off + 14], p[off + 15]
            libs.append((name, f"{major}.{minor}.{build}.{qfe}", approval))
        return libs

    def import_libraries(self):
        """
        Layout:  u32 total_size | u32 strtab_size | u32 lib_count | strtab... | libs...
        Each lib: u32 size | 20B digest | u32 id | u32 version | u32 min_version
                  | u16 name_index | u16 record_count | u32 records[record_count]
        The records are addresses INSIDE the encrypted image where the loader
        patches in the real kernel function/variable, so all we can learn from
        the header is how many there are. Functions use 2 records (a descriptor
        + a call thunk), variables use 1.
        """
        p = self.payload("IMPORT_LIBRARIES")
        if not p:
            return []
        strtab_size, lib_count = u32(p, 4), u32(p, 8)
        names = [s.decode("ascii") for s in p[12:12 + strtab_size].split(b"\0") if s]
        libs, off = [], 12 + strtab_size
        for _ in range(lib_count):
            size = u32(p, off)
            ver, ver_min = u32(p, off + 0x1C), u32(p, off + 0x20)
            name_index, count = u16(p, off + 0x24), u16(p, off + 0x26)
            libs.append((names[name_index], version_str(ver), version_str(ver_min), count))
            off += size
        return libs

    def sections(self):
        """Walk the page descriptors -> merged (start, end, type) address ranges."""
        s = self.security_offset
        d = self.data
        flags = u32(d, s + SEC_IMAGE_FLAGS)
        page_size = 0x1000 if flags & IMAGE_FLAG_4KB_PAGES else 0x10000
        addr = self.value("IMAGE_BASE_ADDRESS") or u32(d, s + SEC_LOAD_ADDRESS)
        ranges = []
        for i in range(u32(d, s + SEC_PAGE_DESC_COUNT)):
            v = u32(d, s + SEC_PAGE_DESCS + i * PAGE_DESC_SIZE)
            kind, pages = v & 0xF, v >> 4
            end = addr + pages * page_size
            if ranges and ranges[-1][2] == kind:
                ranges[-1][1] = end                 # merge with previous run
            else:
                ranges.append([addr, end, kind])
            addr = end
        return page_size, ranges

    def resources(self):
        """XEX resources: 16-byte records of char name[8]; u32 address; u32 size"""
        p = self.payload("RESOURCE_INFO")
        if not p:
            return []
        return [(cstr(p[o:o + 8]), u32(p, o + 8), u32(p, o + 12))
                for o in range(4, len(p) - 15, 16)]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("USAGE")[1])
    x = Xex(sys.argv[1])
    d, s = x.data, x.security_offset

    print(f"== {sys.argv[1]} ({len(d):,} bytes) ==\n")

    ei = x.execution_info()
    if ei:
        print(f"Title ID        : {ei['title_id']:08X}  (publisher code "
              f"'{chr(ei['title_id'] >> 24)}{chr((ei['title_id'] >> 16) & 0xFF)}')")
        print(f"Media ID        : {ei['media_id']:08X}")
        print(f"Version         : {ei['version']} (base {ei['base_version']}), disc {ei['disc']}")
    print(f"Original PE name: {cstr(x.payload('ORIGINAL_PE_NAME')[4:]) if x.payload('ORIGINAL_PE_NAME') else '?'}")
    print(f"Module flags    : 0x{x.module_flags:08X}")
    enc, comp = x.file_format() or ("?", "?")
    print(f"Encryption      : {enc}")
    print(f"Compression     : {comp}")
    print(f"Region mask     : 0x{u32(d, s + SEC_REGION):08X}")
    print()

    base = x.value("IMAGE_BASE_ADDRESS") or u32(d, s + SEC_LOAD_ADDRESS)
    img_size = u32(d, s + SEC_IMAGE_SIZE)
    print(f"Image base      : 0x{base:08X}")
    print(f"Image size      : 0x{img_size:08X} ({img_size / 1048576:.1f} MB in memory)")
    print(f"Entry point     : 0x{x.value('ENTRY_POINT'):08X}")
    for name in ("DEFAULT_STACK_SIZE", "DEFAULT_HEAP_SIZE", "DEFAULT_FILESYSTEM_CACHE_SIZE",
                 "TITLE_WORKSPACE_SIZE", "SYSTEM_FLAGS"):
        v = x.value(name)
        if v is not None:
            print(f"{name.replace('_', ' ').lower():16s}: 0x{v:08X}")
    tls = x.payload("TLS_INFO")
    if tls:
        slots, raw_addr, data_size, raw_size = struct.unpack(">IIII", tls)
        print(f"TLS             : {slots} slots, template @0x{raw_addr:08X}, {data_size} bytes")
    print()

    page_size, ranges = x.sections()
    print(f"Memory layout ({page_size // 1024} KB pages):")
    for start, end, kind in ranges:
        print(f"  0x{start:08X}-0x{end:08X}  {SECTION_TYPES.get(kind, f'type {kind}'):15s}"
              f" {(end - start) / 1048576:6.2f} MB")
    print()

    libs = x.static_libraries()
    if libs:
        print(f"Static libraries linked into the exe ({len(libs)}):")
        for name, ver, approval in libs:
            print(f"  {name:10s} {ver:16s} (approval 0x{approval:02X})")
        print()

    imps = x.import_libraries()
    if imps:
        print("Imported system libraries (must be provided by the runtime):")
        for name, ver, ver_min, count in imps:
            print(f"  {name:14s} v{ver:14s} (min v{ver_min})  {count} import records")
        print()

    res = x.resources()
    if res:
        print("Embedded resources:")
        for name, addr, size in res:
            print(f"  {name:10s} @0x{addr:08X}  {size:,} bytes")
        print()

    print("All optional headers present:")
    for key, (value, payload) in sorted(x.headers.items()):
        size = f"{len(payload)} bytes" if payload is not None else f"= 0x{value:08X}"
        print(f"  0x{key:08X}  {KEYS.get(key, '(unknown)'):30s} {size}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as e:
        sys.exit(f"[xex_info] error: {e}")
