# =============================================================================
# tools/gdb/debug.py -- game-level commands for an interactive gdb session
# =============================================================================
#
# Loaded by tools/gdb/debug.gdb (`tools/play.sh --gdb`). Plain gdb sees the PC
# side of the port: x86 registers, C++ frames. These commands translate to the
# GAME's side, the original PowerPC program:
#
#   gbreak 0x82436308 [if <gdb condition>]   stop when that game function runs
#   gbreak sub_82436308                      (same; any address in the function
#                                             is NOT supported: function starts)
#   gregs                                    r1-r12, lr, ctr, f1-f4 at the stop
#   gr <reg>                                 one register: gr r3, gr f1
#   gread <addr> [type] [count]              guest memory: u8 u16 u32 (default)
#                                            f32 f64 str; addr may be r3+0x5C
#   gwrite <addr> <type> <value>             change guest memory (big-endian)
#   gbt [n]                                  the game's call stack (sub_X frames)
#   ghelp                                    this list
#
# Plus gdb's own: c (continue), Ctrl-C (pause everything), info breakpoints,
# delete <n>, disable <n>, finish (run until the function returns).
#
# HOW IT MAPS (CLAUDE.md "Workflow for runtime hangs"):
# * Every game function became a C++ function. The original code is
#   __imp__sub_XXXXXXXX (sub_X itself may be our override that calls it), so a
#   breakpoint on __imp__sub_X stops whenever the ORIGINAL runs. If our C++
#   replaces the function without calling the original, use plain `break sub_X`.
# * At the function's first instruction the x86 register rdi holds the
#   PPCContext* (the PowerPC registers) and rsi the guest memory base. The stop
#   handler saves rdi in $ctx so gregs keeps working after `next`/`step`.
#   Registers change as the function runs: gregs shows the CURRENT values.
# * Guest address A lives at host 0x100000000 + A on Linux. Game memory is
#   BIG-endian, so reads/writes swap bytes.
# * A breakpoint pauses EVERY thread (gdb all-stop): picture, sound and input
#   freeze until `c`. The window may be marked "not responding": that's fine.
# =============================================================================
import re
import struct

import gdb

GUEST_BASE = 0x100000000
HELP = '''Game commands:
  gbreak 0x82436308 [if <cond>]   stop when that game function runs (function starts only)
  gregs                           PowerPC registers at the stop (r1-r12, r30, r31, lr, ctr, f1-f4)
  gr <reg>                        one register: gr r3 / gr f1 / gr lr
  gread <addr> [type] [count]     guest memory: u8 u16 u32 s32 f32 f64 str; addr may be r3+0x5C
  gwrite <addr> <type> <value>    change guest memory
  gbt [n]                         the game's call stack (newest first)
gdb's own: c (continue)  Ctrl-C (pause)  finish  info breakpoints  delete <n>  disable <n>  quit
Conditions use gdb syntax on the registers: gbreak 0x82436308 if ((PPCContext*)$rdi)->r4.u32 == 2'''
TYPES = {  # type -> (struct format, size)
    'u8': ('>B', 1), 'u16': ('>H', 2), 'u32': ('>I', 4),
    's8': ('>b', 1), 's16': ('>h', 2), 's32': ('>i', 4),
    'f32': ('>f', 4), 'f64': ('>d', 8),
}


def _ctx():
    """The saved PPCContext* (set when a gbreak stops), as a gdb value."""
    ctx = gdb.convenience_variable('ctx')
    if ctx is None or int(ctx) == 0:
        raise gdb.GdbError('no game registers yet: stop at a gbreak first')
    return ctx.cast(gdb.lookup_type('PPCContext').pointer())


def _reg(name):
    """A guest register by name: r0-r31, f0-f31, lr, ctr."""
    ctx = _ctx()
    name = name.lower()
    if name == 'lr':
        return int(ctx['lr']) & 0xFFFFFFFF
    if name == 'ctr':
        return int(ctx['ctr']['u32'])
    if re.fullmatch(r'f\d+', name):
        return float(ctx[name]['f64'])
    if re.fullmatch(r'r\d+', name):
        return int(ctx[name]['u32'])
    raise gdb.GdbError('unknown register ' + name)


def _addr(text):
    """A guest address: a number, a register, or a sum like r3+0x5C / r31-8."""
    total = 0
    for sign, part in re.findall(r'([+-]?)\s*([^+\-\s]+)', text):
        if re.fullmatch(r'[rR]\d+', part):
            value = _reg(part)
        else:
            value = int(part, 0)
        total += -value if sign == '-' else value
    return total & 0xFFFFFFFF


def _function_symbol(text):
    """'0x82436308' / '82436308' / 'sub_82436308' -> '__imp__sub_82436308'."""
    m = re.fullmatch(r'(?:__imp__)?(?:sub_)?(?:0x)?([0-9A-Fa-f]{8})', text)
    if not m:
        raise gdb.GdbError('expected a game function address like 0x82436308')
    name = '__imp__sub_' + m.group(1).upper()
    try:
        gdb.parse_and_eval('(void*)&' + name)
    except gdb.error:
        raise gdb.GdbError(name + ' does not exist: not a function start? '
                           'Find the function containing it with xexdis.')
    return name


class GuestBreakpoint(gdb.Breakpoint):
    """A breakpoint on a game function's entry that remembers its registers."""

    def stop(self):
        # At the entry instruction rdi = PPCContext*, the guest registers.
        gdb.set_convenience_variable('ctx', gdb.parse_and_eval('$rdi'))
        return True


class GBreak(gdb.Command):
    """gbreak <game function address> [if <condition>]: stop when it runs."""

    def __init__(self):
        super().__init__('gbreak', gdb.COMMAND_BREAKPOINTS)

    def invoke(self, arg, from_tty):
        parts = arg.split(None, 1)
        if not parts:
            raise gdb.GdbError('usage: gbreak 0x82436308 [if <condition>]')
        name = _function_symbol(parts[0])
        bp = GuestBreakpoint('*' + name)
        if len(parts) > 1:
            cond = parts[1]
            bp.condition = cond[3:] if cond.startswith('if ') else cond
        print('Breakpoint %d on game function %s (gregs / gbt when it stops, c to go on)'
              % (bp.number, name[len('__imp__'):]))


class GRegs(gdb.Command):
    """gregs: the game's PowerPC registers at the current stop."""

    def __init__(self):
        super().__init__('gregs', gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        regs = ['r1', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12',
                'r30', 'r31']
        line = []
        for i, r in enumerate(regs):
            line.append('%-3s %08X' % (r, _reg(r)))
            if len(line) == 4:
                print('   '.join(line))
                line = []
        if line:
            print('   '.join(line))
        print('lr  %08X   ctr %08X' % (_reg('lr'), _reg('ctr')))
        print('   '.join('%s %g' % (f, _reg(f)) for f in ('f1', 'f2', 'f3', 'f4')))


class GR(gdb.Command):
    """gr <register>: one game register (r3, f1, lr, ctr)."""

    def __init__(self):
        super().__init__('gr', gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        value = _reg(arg.strip())
        print('%s = %s' % (arg.strip(), ('%g' % value) if isinstance(value, float)
                           else '0x%08X (%d)' % (value, value)))


class GRead(gdb.Command):
    """gread <addr> [type] [count]: read guest memory (big-endian)."""

    def __init__(self):
        super().__init__('gread', gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        parts = arg.split()
        if not parts:
            raise gdb.GdbError('usage: gread <addr> [u8|u16|u32|f32|f64|str] [count]')
        addr = _addr(parts[0])
        kind = parts[1] if len(parts) > 1 else 'u32'
        count = int(parts[2], 0) if len(parts) > 2 else 1
        mem = gdb.selected_inferior()
        if kind == 'str':
            raw = bytes(mem.read_memory(GUEST_BASE + addr, max(count, 64)))
            print('%08X: "%s"' % (addr, raw.split(b'\0')[0].decode('latin-1')))
            return
        if kind not in TYPES:
            raise gdb.GdbError('type: one of ' + ' '.join(TYPES) + ' str')
        fmt, size = TYPES[kind]
        raw = bytes(mem.read_memory(GUEST_BASE + addr, size * count))
        for i in range(count):
            (v,) = struct.unpack(fmt, raw[i * size:(i + 1) * size])
            shown = ('%g' % v) if kind.startswith('f') else '0x%0*X (%d)' % (size * 2, v & (1 << size * 8) - 1, v)
            print('%08X: %s' % (addr + i * size, shown))


class GWrite(gdb.Command):
    """gwrite <addr> <type> <value>: change guest memory (big-endian)."""

    def __init__(self):
        super().__init__('gwrite', gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        parts = arg.split()
        if len(parts) != 3 or parts[1] not in TYPES:
            raise gdb.GdbError('usage: gwrite <addr> <u8|u16|u32|s32|f32|f64> <value>')
        addr = _addr(parts[0])
        fmt, size = TYPES[parts[1]]
        value = float(parts[2]) if parts[1].startswith('f') else int(parts[2], 0)
        gdb.selected_inferior().write_memory(GUEST_BASE + addr, struct.pack(fmt, value))
        print('%08X <- %s (%s)' % (addr, parts[2], parts[1]))


class GBt(gdb.Command):
    """gbt [n]: the game's call stack (game functions only, newest first)."""

    def __init__(self):
        super().__init__('gbt', gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        limit = int(arg, 0) if arg.strip() else 40
        frame = gdb.newest_frame()
        shown = 0
        while frame is not None and shown < limit:
            name = frame.name() or ''
            m = re.search(r'sub_([0-9A-Fa-f]{8})', name)
            if m:
                print('  sub_%s' % m.group(1).upper())
                shown += 1
            elif name.endswith('_entry'):  # kernel export: what it waits on
                print('  [xbox] %s' % name)
            frame = frame.older()
        if shown == 0:
            print('no game functions on this thread\'s stack (try `info threads`, '
                  '`thread <n>`; the game\'s main thread is "Main XThread")')


class GHelp(gdb.Command):
    """ghelp: the game-level commands."""

    def __init__(self):
        super().__init__('ghelp', gdb.COMMAND_SUPPORT)

    def invoke(self, arg, from_tty):
        print(HELP)


for command in (GBreak, GRegs, GR, GRead, GWrite, GBt, GHelp):
    command()

print('Game debugger ready. `run` starts the game; Ctrl-C pauses it; `ghelp` lists '
      'the game commands (gbreak, gregs, gread, gwrite, gbt).')
