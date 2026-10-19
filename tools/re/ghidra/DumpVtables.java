import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;
public class DumpVtables extends GhidraScript {
  public void run() throws Exception {
    String out = getScriptArgs()[0];
    boolean lin = getScriptArgs()[1].equals("lin");
    PrintWriter pw = new PrintWriter(new BufferedWriter(new FileWriter(out)));
    Memory mem = currentProgram.getMemory(); Listing lst=currentProgram.getListing();
    long base = currentProgram.getImageBase().getOffset();
    for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
      String n = s.getName();
      if (!(n.equals("vftable") || n.equals("vtable") || n.startsWith("vftable_"))) continue;
      Address a = s.getAddress();
      if (lin) a = a.add(16);
      StringBuilder sb = new StringBuilder();
      sb.append(s.getParentNamespace().getName(true)).append("|").append(n).append("|").append(a.getOffset()-base).append("|");
      for (int i=0;i<400;i++) {
        long p;
        try { p = mem.getLong(a.add(i*8)); } catch(Exception e){ break; }
        Address t = toAddr(p);
        if (!mem.contains(t) || !mem.getBlock(t).isExecute()) break;
        if (i>0) {
          // stop at next symbol labelled vtable
          Symbol ps = currentProgram.getSymbolTable().getPrimarySymbol(a.add(i*8));
          if (ps!=null && (ps.getName().equals("vftable")||ps.getName().equals("vtable")||ps.getName().contains("RTTI"))) break;
        }
        sb.append(p-base).append(",");
      }
      pw.println(sb.toString());
    }
    pw.close();
  }
}
