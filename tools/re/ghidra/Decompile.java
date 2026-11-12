// Ghidra headless post-script: print the decompiled C of the functions
// containing each address given as a script argument (hex), between
// "=== decomp <addr> <name>" and "=== end" markers (tools/re/decomp.py reads
// them).
// @category skate3

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {
  @Override
  protected void run() throws Exception {
    DecompInterface ifc = new DecompInterface();
    ifc.openProgram(currentProgram);
    for (String a : getScriptArgs()) {
      Address addr = toAddr(Long.parseLong(a, 16));
      Function fn = getFunctionContaining(addr);
      if (fn == null) {
        System.out.println("=== decomp " + a + " ?\nno function contains " + a + "\n=== end");
        continue;
      }
      DecompileResults res = ifc.decompileFunction(fn, 120, monitor);
      String body = res != null && res.getDecompiledFunction() != null
          ? res.getDecompiledFunction().getC()
          : "decompile failed: " + (res != null ? res.getErrorMessage() : "?");
      System.out.println("=== decomp " + a + " " + fn.getName() + "\n" + body + "\n=== end");
    }
  }
}
