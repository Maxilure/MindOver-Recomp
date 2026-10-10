#!/usr/bin/env python3
"""native_progress.py -- the README's "Going native" progress bars.

WHAT IT MEASURES
    The game's own code already runs as native C++ (static recompilation),
    but it still talks to a FAKE Xbox 360: it imports 155 system services
    (xboxkrnl + xam), answered by the ReXGlue runtime. Going native = the
    port answering them with its own PC code, piece by piece, until the game
    no longer needs the Xbox at all.

    Each import below belongs to one Xbox PIECE (graphics driver, profiles,
    saves, ...). A service counts as DONE when the game no longer reaches the
    SDK's emulated version of it: our own code answers it (a hook on the
    game-side caller, or the caller rewritten), or the game stopped calling it.
    The Xbox calls meter (src/debug/xbox_calls.h; <log>-xbox-calls.txt, the
    debug console's `xbox`) shows which services a session still calls and
    from where: use it to check an entry before moving it to DONE.

    KERNEL BASICS (threads, locks, events, memory, files, ...) are listed but
    NOT counted in the bars: they are thin 1:1 translations to the host OS
    already, and replacing them only comes with the full decompilation.

USAGE
    python3 tools/native_progress.py            print the table
    python3 tools/native_progress.py --readme   rewrite the block between
        <!-- native-progress:begin --> and <!-- native-progress:end --> in README.md
    python3 tools/native_progress.py --check generated/default/crash_mom_init.cpp
        every import in the generated function table is classified exactly once
"""
import argparse
import re
import sys
from pathlib import Path

# -----------------------------------------------------------------------------
# The Xbox pieces: (title, what replaces it, [services])
# -----------------------------------------------------------------------------
PIECES = [
    ("Graphics driver",
     "the port's own Vulkan renderer, then the emulated GPU off",
     ["VdCallGraphicsNotificationRoutines", "VdEnableDisableClockGating",
      "VdEnableRingBufferRPtrWriteBack", "VdGetCurrentDisplayGamma",
      "VdGetCurrentDisplayInformation", "VdGetSystemCommandBuffer", "VdInitializeEngines",
      "VdInitializeRingBuffer", "VdInitializeScalerCommandBuffer", "VdIsHSIOTrainingSucceeded",
      "VdPersistDisplay", "VdQueryVideoFlags", "VdQueryVideoMode", "VdRetrainEDRAM",
      "VdRetrainEDRAMWorker", "VdSetDisplayMode", "VdSetGraphicsInterruptCallback",
      "VdSetSystemCommandBufferGpuIdentifierAddress", "VdShutdownEngines", "VdSwap"]),
    ("Profiles and sign-in",
     "no profiles at all: who plays = which controllers play",
     ["XamUserGetSigninState", "XamUserGetName", "XamShowSigninUI",
      "XamUserReadProfileSettings"]),
    ("Saves and storage",
     "plain save files in one folder: no storage devices, no content packages",
     ["XamContentClose", "XamContentCreateEnumerator", "XamContentCreateEx",
      "XamContentDelete", "XamContentFlush", "XamContentGetCreator",
      "XamContentGetDeviceData", "XamContentGetDeviceState", "XamContentSetThumbnail",
      "XamEnumerate", "XamShowDeviceSelectorUI"]),
    ("Controllers",
     "the input manager fed directly (keyboard and mouse, any SDL controller, rumble)",
     ["XamInputGetCapabilities", "XamInputGetState", "XamInputSetState"]),
    ("Sound output",
     "each mixed frame straight to the PC's audio",
     ["XAudioGetVoiceCategoryVolume", "XAudioGetVoiceCategoryVolumeChangeMask",
      "XAudioRegisterRenderDriverClient", "XAudioSubmitRenderDriverFrame",
      "XAudioUnregisterRenderDriverClient"]),
    ("XMA sound decoder",
     "a software decoder instead of the emulated sound chip",
     ["XMACreateContext", "XMAReleaseContext"]),
    ("System pop-ups and notifications",
     "PC behaviour (the port's own messages); gone once profiles and saves are native",
     ["XamShowMessageBoxUIEx", "XamShowDirtyDiscErrorUI", "XamNotifyCreateListener",
      "XNotifyGetNext", "XNotifyPositionUI"]),
    ("System messages (achievements, ...)",
     "the port's own handlers",
     ["XMsgInProcessCall", "XMsgStartIORequest", "XMsgStartIORequestEx"]),
    ("Console settings",
     "the port's settings (language, region, video mode)",
     ["XGetAVPack", "XGetGameRegion", "XGetLanguage", "XGetVideoMode", "ExGetXConfigSetting",
      "XamGetSystemVersion", "XamGetExecutionId", "XexCheckExecutablePrivilege"]),
    ("Quit and launch",
     "quit the program cleanly",
     ["XamLoaderLaunchTitle", "XamLoaderTerminateTitle", "HalReturnToFirmware",
      "ExRegisterTitleTerminateNotification"]),
    ("Network",
     "not needed (no online features)",
     ["NetDll_WSAStartup"]),
]

# Not counted (see the header): threads, locks, events, memory, files, C library.
KERNEL_BASICS = [
    "__C_specific_handler", "DbgBreakPoint", "DbgPrint", "ExCreateThread", "ExTerminateThread",
    "KeAcquireSpinLockAtRaisedIrql", "KeBugCheck", "KeBugCheckEx", "KeDelayExecutionThread",
    "KeEnterCriticalRegion", "KeGetCurrentProcessType", "KeInitializeSemaphore",
    "KeLeaveCriticalRegion", "KeLockL2", "KeQueryPerformanceFrequency", "KeQuerySystemTime",
    "KeRaiseIrqlToDpcLevel", "KeReleaseSemaphore", "KeReleaseSpinLockFromRaisedIrql",
    "KeResetEvent", "KeResumeThread", "KeSetAffinityThread", "KeSetBasePriorityThread",
    "KeSetCurrentStackPointers", "KeSetEvent", "KeTlsAlloc", "KeTlsFree", "KeTlsGetValue",
    "KeTlsSetValue", "KeUnlockL2", "KeWaitForMultipleObjects", "KeWaitForSingleObject",
    "KfAcquireSpinLock", "KfLowerIrql", "KfReleaseSpinLock", "KiApcNormalRoutineNop",
    "MmAllocatePhysicalMemoryEx", "MmCreateKernelStack", "MmDeleteKernelStack",
    "MmFreePhysicalMemory", "MmGetPhysicalAddress", "MmQueryAddressProtect", "MmQueryStatistics",
    "NtAllocateVirtualMemory", "NtClose", "NtCreateEvent", "NtCreateFile", "NtCreateMutant",
    "NtCreateSemaphore", "NtDuplicateObject", "NtFlushBuffersFile", "NtFreeVirtualMemory",
    "NtOpenFile", "NtQueryDirectoryFile", "NtQueryInformationFile", "NtQueryVirtualMemory",
    "NtQueryVolumeInformationFile", "NtReadFile", "NtReadFileScatter", "NtReleaseMutant",
    "NtReleaseSemaphore", "NtResumeThread", "NtSetEvent", "NtSetInformationFile",
    "NtWaitForMultipleObjectsEx", "NtWaitForSingleObjectEx", "NtWriteFile",
    "ObCreateSymbolicLink", "ObDeleteSymbolicLink", "ObDereferenceObject",
    "ObReferenceObjectByHandle", "RtlCaptureContext", "RtlCompareMemoryUlong",
    "RtlEnterCriticalSection", "RtlFillMemoryUlong", "RtlImageXexHeaderField",
    "RtlInitAnsiString", "RtlInitializeCriticalSection", "RtlLeaveCriticalSection",
    "RtlMultiByteToUnicodeN", "RtlNtStatusToDosError", "RtlRaiseException",
    "RtlTimeFieldsToTime", "RtlTimeToTimeFields", "RtlUnwind", "sprintf", "_vsnprintf",
    "XexGetModuleHandle", "XexGetProcedureAddress",
]

# -----------------------------------------------------------------------------
# DONE: services the game no longer reaches in the emulated SDK.
# Every entry: why it's done + where (file), checked with the Xbox calls meter.
# -----------------------------------------------------------------------------
DONE = {
    # Profiles and sign-in (2026-10-10, src/players/who_plays.*): the game's
    # sign-in functions replaced by PC versions ("who plays" = which
    # controllers play; no profile, name, settings or sign-in pop-up). Their
    # only callers were those functions; SDK patch 0011 removed.
    "XamUserGetSigninState": "who_plays.cpp: sub_8227CEA8, sub_8227B1D8, sub_8235B400",
    "XamUserGetName": "who_plays.cpp: sub_8227CEA8, sub_8227B1D8",
    "XamUserReadProfileSettings": "who_plays.cpp: sub_82266398 (defaults), sub_82322960",
    "XamShowSigninUI": "who_plays.cpp: sub_8235B400 (no sign-in loop)",
    # Saves and storage (2026-10-10, src/saves/pc_save_drive.*): Radical's
    # XenonSaveDrive replaced by plain files; the only callers of these were
    # its methods and the device selector (findings/32).
    "XamContentClose": "pc_save_drive.cpp",
    "XamContentCreateEnumerator": "pc_save_drive.cpp",
    "XamContentCreateEx": "pc_save_drive.cpp",
    "XamContentDelete": "pc_save_drive.cpp",
    "XamContentFlush": "pc_save_drive.cpp",
    "XamContentGetCreator": "pc_save_drive.cpp",
    "XamContentGetDeviceData": "pc_save_drive.cpp",
    "XamContentGetDeviceState": "pc_save_drive.cpp",
    "XamContentSetThumbnail": "pc_save_drive.cpp",
    "XamEnumerate": "pc_save_drive.cpp",
    "XamShowDeviceSelectorUI": "pc_save_drive.cpp: sub_823002D8",
    # Notifications: both listeners (sign-in, storage) are gone.
    "XamNotifyCreateListener": "who_plays.cpp: sub_8227CE70; pc_save_drive.cpp: sub_823007D0",
    "XNotifyGetNext": "who_plays.cpp: sub_8227B1D8 (and the save drive's device check, unused)",
}

BEGIN = "<!-- native-progress:begin -->"
END = "<!-- native-progress:end -->"
BAR_CELLS = 10


def bar(done, total):
    filled = round(BAR_CELLS * done / total) if total else 0
    # At least one filled cell once anything is done, never full until all are.
    if done and not filled:
        filled = 1
    if done < total and filled == BAR_CELLS:
        filled = BAR_CELLS - 1
    return "▰" * filled + "▱" * (BAR_CELLS - filled)


def table():
    rows = []
    all_done = all_total = 0
    for title, replacement, services in PIECES:
        done = sum(1 for s in services if s in DONE)
        all_done += done
        all_total += len(services)
        rows.append(f"| {title} | `{bar(done, len(services))}` {done} / {len(services)} | {replacement} |")
    pct = round(100 * all_done / all_total)
    lines = [
        BEGIN,
        "<!-- Written by tools/native_progress.py --readme: edit the tool, not this block. -->",
        f"**Xbox services replaced: {all_done} of {all_total} ({pct}%)** "
        f"`{bar(all_done, all_total)}`",
        "",
        "| Xbox piece | Replaced | Becomes |",
        "|---|---|---|",
        *rows,
        "",
        f"Not counted: {len(KERNEL_BASICS)} kernel basics (threads, locks, memory, files), "
        "already thin translations to the PC's own.",
        END,
    ]
    return "\n".join(lines)


def check(init_cpp):
    text = Path(init_cpp).read_text()
    imported = set(re.findall(r"__imp__([A-Za-z0-9_]+) \}", text))
    classified = [s for _, _, services in PIECES for s in services] + KERNEL_BASICS
    ok = True
    for s in sorted(imported - set(classified)):
        print(f"not classified: {s}")
        ok = False
    for s in sorted(set(classified) - imported):
        print(f"classified but not imported: {s}")
        ok = False
    dupes = {s for s in classified if classified.count(s) > 1}
    for s in sorted(dupes):
        print(f"classified twice: {s}")
        ok = False
    for s in sorted(set(DONE) - set(classified)):
        print(f"DONE entry not a classified service: {s}")
        ok = False
    print(f"{len(imported)} imports, {len(classified)} classified: {'OK' if ok else 'PROBLEMS'}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--readme", action="store_true", help="rewrite the block in README.md")
    ap.add_argument("--check", metavar="INIT_CPP", help="generated/default/crash_mom_init.cpp")
    args = ap.parse_args()
    if args.check:
        sys.exit(0 if check(args.check) else 1)
    block = table()
    if not args.readme:
        print(block)
        return
    readme = Path(__file__).resolve().parent.parent / "README.md"
    text = readme.read_text()
    if BEGIN not in text or END not in text:
        sys.exit(f"{readme}: no {BEGIN} ... {END} block")
    start = text.index(BEGIN)
    end = text.index(END) + len(END)
    readme.write_text(text[:start] + block + text[end:])
    print(f"{readme}: updated")


if __name__ == "__main__":
    main()
