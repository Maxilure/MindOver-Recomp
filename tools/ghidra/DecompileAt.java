// =============================================================================
// DecompileAt.java -- Ghidra headless script: print the decompiled C-like
// pseudo-code of game functions (run by tools/ghidra_decompile.sh)
// =============================================================================
//
// Ghidra (https://ghidra-sre.org) turns machine code into readable pseudo-C.
// With the XEXLoaderWV extension it reads the game's default.xex directly
// (decrypting and decompressing it like the console). The project lives in
// out/ghidra/ (gitignored: it holds the game's code). This script runs inside
// Ghidra's headless analyzer on that already-analysed project.
//
// Arguments: one or more guest addresses (0x82433510 or 82433510). Each may
// sit anywhere inside a function: the function CONTAINING it is printed.
// Addresses Ghidra has no function for (code reached only through a
// pointer it hasn't found) get one created there first, like pressing F in
// the UI; the program is opened read-only, so nothing is saved.
//
// Output per function, on stdout between marker lines (the wrapper strips
// Ghidra's own log around them):
//   ===== sub_82433510 @ 82433510 (size N bytes) =====
//   <pseudo-C>
//
// Names: our codegen calls functions sub_XXXXXXXX; Ghidra's default is
// FUN_XXXXXXXX, the XEX loader's Function_XXXXXXXX. The wrapper renames both
// to sub_ in the text so everything reads the same.
// =============================================================================

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    if (args.length == 0) {
      println("usage: DecompileAt <address> [address...]");
      return;
    }
    DecompInterface decompiler = new DecompInterface();
    decompiler.setOptions(new DecompileOptions());
    if (!decompiler.openProgram(currentProgram)) {
      println("decompiler failed to start: " + decompiler.getLastMessage());
      return;
    }
    try {
      for (String arg : args) {
        String text = arg.toLowerCase().startsWith("0x") ? arg.substring(2) : arg;
        Address address = toAddr(Long.parseLong(text, 16));
        Function function = getFunctionContaining(address);
        if (function == null) {
          // Not found by Ghidra's analysis: make one starting here.
          new CreateFunctionCmd(address).applyTo(currentProgram, monitor);
          function = getFunctionContaining(address);
        }
        if (function == null) {
          System.out.println("===== " + arg + ": no function here =====");
          continue;
        }
        // 120 s: the biggest game functions (compiled fight trees) are slow.
        DecompileResults results = decompiler.decompileFunction(function, 120, monitor);
        System.out.println(String.format("===== %s @ %s (size %d bytes) =====", function.getName(),
            function.getEntryPoint(), function.getBody().getNumAddresses()));
        if (results.decompileCompleted()) {
          System.out.println(results.getDecompiledFunction().getC());
        } else {
          System.out.println("(decompile failed: " + results.getErrorMessage() + ")");
        }
      }
    } finally {
      decompiler.dispose();
    }
  }
}
