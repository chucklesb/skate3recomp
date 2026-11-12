// Ghidra headless pre-script: analysis options for the Xbox 360 image.
// "Non-Returning Functions - Discovered" marks the compiler's register
// save/restore helpers (__savegprlr_N / __restgprlr_N, called from every
// prologue and epilogue near 0x82F52000) as non-returning, which cuts every
// function off after its prologue. Turn it off.
// @category skate3

import ghidra.app.script.GhidraScript;

public class SetOptions extends GhidraScript {
  @Override
  protected void run() throws Exception {
    setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
    println("SetOptions: non-returning function discovery off");
  }
}
