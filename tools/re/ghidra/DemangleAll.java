import ghidra.app.script.GhidraScript;
import ghidra.app.util.demangler.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.util.*;
public class DemangleAll extends GhidraScript {
  public void run() throws Exception {
    SymbolTable st = currentProgram.getSymbolTable();
    List<Symbol> syms = new ArrayList<>();
    for (Symbol s : st.getAllSymbols(true)) if (s.getName().startsWith("_Z")) syms.add(s);
    int ok=0;
    DemanglerOptions opt = new DemanglerOptions();
    opt.setApplySignature(true);
    for (Symbol s : syms) {
      try {
        List<DemangledObject> dl = DemanglerUtil.demangle(currentProgram, s.getName(), s.getAddress());
        if (dl==null||dl.isEmpty()) continue;
        DemangledObject d = dl.get(0);
        if (d.applyTo(currentProgram, s.getAddress(), opt, monitor)) ok++;
      } catch (Exception e) {}
    }
    println("demangled "+ok+" of "+syms.size());
  }
}
