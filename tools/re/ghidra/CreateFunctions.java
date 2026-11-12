// Ghidra headless post-script: create a function, named after the recompiler
// (sub_XXXXXXXX), at every address listed in the file given as the first
// script argument ("<hex address> <name>" per line), and mark the image
// executable. tools/re/ghidra/setup.py runs it after the raw import.
// @category skate3

import java.nio.file.Files;
import java.nio.file.Paths;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class CreateFunctions extends GhidraScript {
  @Override
  protected void run() throws Exception {
    for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
      b.setExecute(true);
      b.setWrite(true);
    }
    int made = 0;
    for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
      String[] parts = line.trim().split("\\s+");
      if (parts.length < 2) {
        continue;
      }
      Address addr = toAddr(Long.parseLong(parts[0], 16));
      Function fn = getFunctionAt(addr);
      if (fn == null) {
        disassemble(addr);
        fn = createFunction(addr, parts[1]);
        if (fn != null) {
          made++;
        }
      } else {
        fn.setName(parts[1], SourceType.USER_DEFINED);
      }
    }
    println("CreateFunctions: " + made + " functions created");
  }
}
