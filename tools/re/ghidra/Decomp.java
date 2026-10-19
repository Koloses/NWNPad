// Decompile functions whose (demangled) name matches regex args. Output to file given by env OUT.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*;
import java.util.*;
import java.util.regex.*;

public class Decomp extends GhidraScript {
  public void run() throws Exception {
    String[] args = getScriptArgs();
    String out = args[0];
    PrintWriter pw = new PrintWriter(new FileWriter(out));
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    Listing lst = currentProgram.getListing();
    SymbolTable st = currentProgram.getSymbolTable();
    for (int i = 1; i < args.length; i++) {
      String a = args[i];
      List<Address> addrs = new ArrayList<>();
      if (a.startsWith("0x")) {
        addrs.add(toAddr(Long.parseUnsignedLong(a.substring(2),16)));
      } else {
        Pattern p = Pattern.compile(a);
        SymbolIterator it = st.getAllSymbols(true);
        while (it.hasNext()) {
          Symbol s = it.next();
          if (s.getSymbolType() != SymbolType.FUNCTION && s.getSymbolType()!=SymbolType.LABEL) continue;
          String n = s.getName(true);
          if (p.matcher(n).find() && currentProgram.getMemory().getBlock(s.getAddress())!=null && currentProgram.getMemory().getBlock(s.getAddress()).isExecute()) {
            if(!addrs.contains(s.getAddress())) addrs.add(s.getAddress());
          }
          if (addrs.size() > 60) break;
        }
      }
      for (Address ad : addrs) {
        Function f = lst.getFunctionAt(ad);
        if (f == null) {
          new DisassembleCommand(ad, null, true).applyTo(currentProgram, monitor);
          new CreateFunctionCmd(ad).applyTo(currentProgram, monitor);
          f = lst.getFunctionAt(ad);
        }
        if (f == null) { pw.println("// no func at " + ad); continue; }
        try {
          if (!f.getParentNamespace().isGlobal() && !"__thiscall".equals(f.getCallingConventionName()) && System.getenv("NOTHIS")==null) {
            f.setCallingConvention("__thiscall");
          }
        } catch (Exception e) {}
        // ensure callees exist as functions for nicer output
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        pw.println("// ===== " + f.getName(true) + " @ " + ad);
        if (r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
        else pw.println("// FAILED " + r.getErrorMessage());
      }
    }
    pw.close();
  }
}
