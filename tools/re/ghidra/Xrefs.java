import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.util.*;
import java.util.regex.*;
public class Xrefs extends GhidraScript {
  public void run() throws Exception {
    String[] args = getScriptArgs();
    PrintWriter pw = new PrintWriter(new FileWriter(args[0]));
    Listing lst = currentProgram.getListing();
    for (int i=1;i<args.length;i++) {
      Pattern p = Pattern.compile(args[i]);
      for (Function f : lst.getFunctions(true)) {
        if (!p.matcher(f.getName(true)).find()) continue;
        pw.println("== "+f.getName(true)+" @ "+f.getEntryPoint());
        Set<String> seen = new TreeSet<>();
        for (Reference r : currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint())) {
          Function c = lst.getFunctionContaining(r.getFromAddress());
          seen.add("   <- "+(c==null?"?":c.getName(true))+" @ "+r.getFromAddress());
        }
        for (String s: seen) pw.println(s);
      }
    }
    pw.close();
  }
}
