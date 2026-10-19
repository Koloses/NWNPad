import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.util.*;
public class DumpFuncs extends GhidraScript {
  String esc(String s){ StringBuilder b=new StringBuilder(); for(char c: s.toCharArray()){ if(c=='"'||c=='\\') b.append('\\').append(c); else if(c<32||c>126) b.append(String.format("\\u%04x",(int)c)); else b.append(c);} return b.toString(); }
  public void run() throws Exception {
    String out = getScriptArgs()[0];
    PrintWriter pw = new PrintWriter(new BufferedWriter(new FileWriter(out)));
    Listing lst = currentProgram.getListing();
    ReferenceManager rm = currentProgram.getReferenceManager();
    Memory mem = currentProgram.getMemory();
    long base = currentProgram.getImageBase().getOffset();
    for (Function f : lst.getFunctions(true)) {
      if (f.isThunk() && f.getThunkedFunction(false)!=null && f.getThunkedFunction(false).isExternal()) continue;
      StringBuilder sb = new StringBuilder();
      sb.append("{\"a\":").append(f.getEntryPoint().getOffset()-base);
      sb.append(",\"n\":\"").append(esc(f.getName(true))).append("\"");
      sb.append(",\"sz\":").append(f.getBody().getNumAddresses());
      List<String> calls = new ArrayList<>(); List<String> strs = new ArrayList<>(); List<String> datas=new ArrayList<>(); List<String> imms=new ArrayList<>();
      InstructionIterator ii = lst.getInstructions(f.getBody(), true);
      int ninst=0;
      while (ii.hasNext()) {
        Instruction in = ii.next(); ninst++;
        for (Reference r : in.getReferencesFrom()) {
          Address to = r.getToAddress();
          if (r.getReferenceType().isCall()) {
            Function c = lst.getFunctionAt(to);
            if (c!=null && c.isThunk()) { Function t=c.getThunkedFunction(true); if(t!=null) c=t; }
            if (c!=null) calls.add(c.isExternal()? "\"X:"+esc(c.getName())+"\"" : String.valueOf(c.getEntryPoint().getOffset()-base));
          } else if (r.getReferenceType().isData() || r.getReferenceType().isRead()) {
            Data d = lst.getDataAt(to);
            if (d!=null && d.hasStringValue()) { Object v=d.getValue(); if(v!=null) strs.add("\""+esc(v.toString())+"\""); }
            else if (mem.contains(to) && !to.isStackAddress()) datas.add(String.valueOf(to.getOffset()-base));
          }
        }
        for (int op=0; op<in.getNumOperands(); op++) for (Object o: in.getOpObjects(op)) if (o instanceof Scalar) { long v=((Scalar)o).getSignedValue(); if (Math.abs(v)>=0x10 && Math.abs(v)<0x100000) imms.add(String.valueOf(v)); }
      }
      sb.append(",\"ni\":").append(ninst);
      sb.append(",\"c\":[").append(String.join(",",calls)).append("]");
      sb.append(",\"s\":[").append(String.join(",",strs)).append("]");
      sb.append(",\"d\":[").append(String.join(",",datas)).append("]");
      sb.append(",\"i\":[").append(String.join(",",imms)).append("]}");
      pw.println(sb.toString());
    }
    pw.close();
  }
}
