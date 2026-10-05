# =============================================================================
# tools/gdb/catch_fault.py -- where did the game write through a null pointer?
# =============================================================================
#
# Used by `tools/play.sh --catch` (tools/gdb/catch_fault.gdb loads this file).
#
# WHY: when the game's code reads or writes through a null pointer, the SDK's
# access-violation handler logs "Unhandled guest access violation: write of
# guest 0x00000110 ..." and the instruction runs again, forever (a fault loop:
# the game freezes and the log fills with that line). The log never says WHICH
# game function did it. This script stops at the first such fault and writes
# the game functions on the stack (newest first) to a file, then ends the game.
#
# HOW: a gdb breakpoint on the SDK's rex::memory::Memory::AccessViolationCallback,
# armed once the game's main loop (__imp__sub_8227AEE0) runs. Only faults in the
# NULL PAGE count (guest address below 0x10000 = host 0x100000000 + 0x10000):
# the same callback also serves page-protection write watches, which are normal.
# Recompiled game functions are named sub_XXXXXXXX / __imp__sub_XXXXXXXX, so
# the stack reads as game addresses: look them up
# with xexdis (CLAUDE.md "inspect code").
#
# Output: $CRASHMOM_FAULT_FILE (play.sh: logs/fault-<session>.txt), also printed.
# =============================================================================
import os
import gdb

GUEST_BASE = 0x100000000
NULL_PAGE_END = GUEST_BASE + 0x10000
out_path = os.environ.get('CRASHMOM_FAULT_FILE', 'fault.txt')
done = [False]


class Fault(gdb.Breakpoint):
    def stop(self):
        if done[0]:
            return False
        try:
            host = int(gdb.parse_and_eval('(unsigned long)host_address'))
        except gdb.error:
            return False
        if not (GUEST_BASE <= host < NULL_PAGE_END):
            return False  # not a null-pointer access (e.g. a write watch): carry on
        done[0] = True
        lines = ['Null-pointer access at guest 0x%X' % (host - GUEST_BASE),
                 'Game functions on the stack, newest first:']
        frame = gdb.newest_frame()
        while frame is not None and len(lines) < 80:
            name = frame.name() or ''
            if 'sub_' in name:
                lines.append('  ' + name.split('sub_')[1][:8])
            frame = frame.older()
        text = '\n'.join(lines) + '\n'
        with open(out_path, 'w') as f:
            f.write(text)
        print('\n' + text + 'Written to ' + out_path + '. Ending the game.', flush=True)
        gdb.post_event(lambda: gdb.execute('kill'))
        return True


class Arm(gdb.Breakpoint):
    def stop(self):
        if not getattr(self, 'armed', False):
            self.armed = True
            Fault('rex::memory::Memory::AccessViolationCallback', internal=True)
        return False


Arm('__imp__sub_8227AEE0', internal=True)
