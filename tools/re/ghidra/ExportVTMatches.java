// Ghidra headless script: write every function match of a Version Tracking
// session as TSV (tools/re/sk2_vt.py runs it after AutoVersionTrackingScript).
// Args: <session path in the project, e.g. /sk2_tu3> <output .tsv>
// Columns: source (Skate 2) address, destination (TU3) address, correlator,
// similarity, confidence, association status.
// Opens the session file directly (the VT script helper reopens the
// destination program, which the headless run already holds: "busy/locked").
// @category skate3

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.feature.vt.api.main.*;
import ghidra.framework.model.DomainFile;

public class ExportVTMatches extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    DomainFile file = state.getProject().getProjectData().getFile(args[0]);
    if (file == null) {
      printerr("no session " + args[0]);
      return;
    }
    // End the script's transaction on the program first: VT takes locks
    // (as AutoVersionTrackingScript does).
    end(true);
    VTSession session = (VTSession) file.getDomainObject(this, false, false, monitor);
    int n = 0;
    try (PrintWriter out = new PrintWriter(args[1], "UTF-8")) {
      for (VTMatchSet set : session.getMatchSets()) {
        String correlator = set.getProgramCorrelatorInfo().getName();
        for (VTMatch m : set.getMatches()) {
          VTAssociation a = m.getAssociation();
          if (a.getType() != VTAssociationType.FUNCTION) {
            continue;
          }
          out.printf("%08X\t%08X\t%s\t%s\t%s\t%s%n", a.getSourceAddress().getOffset(),
              a.getDestinationAddress().getOffset(), correlator,
              m.getSimilarityScore().getFormattedScore(),
              m.getConfidenceScore().getFormattedScore(), a.getStatus());
          n++;
        }
      }
    } finally {
      session.release(this);
    }
    println("ExportVTMatches: " + n + " function matches -> " + args[1]);
  }
}
