import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.mem.*;
import java.util.*;
public class FixFuncs extends GhidraScript {
  public void run() throws Exception {
    Listing lst = currentProgram.getListing();
    Memory mem = currentProgram.getMemory();
    List<Address> addrs = new ArrayList<>();
    for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
      MemoryBlock b = mem.getBlock(s.getAddress());
      if (b==null || !b.isExecute()) continue;
      if (s.getSymbolType()==SymbolType.FUNCTION || s.getSymbolType()==SymbolType.LABEL) addrs.add(s.getAddress());
    }
    println("candidates "+addrs.size());
    AddressSet set = new AddressSet();
    for (Address a: addrs) set.add(a);
    new DisassembleCommand(set, null, true).applyTo(currentProgram, monitor);
    int n=0,t=0;
    for (Address a: addrs) {
      Function f = lst.getFunctionAt(a);
      if (f==null) { new CreateFunctionCmd(a).applyTo(currentProgram, monitor); f=lst.getFunctionAt(a);}      
      if (f==null) continue; n++;
      try { if (!f.getParentNamespace().isGlobal() && !(f.getParentNamespace().getName().equals("std"))) { f.setCallingConvention("__thiscall"); t++; } } catch(Exception e){}
    }
    println("funcs "+n+" thiscall "+t);
  }
}
