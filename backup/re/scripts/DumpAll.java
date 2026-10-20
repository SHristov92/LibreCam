// Post-analysis: decompile every function into one C file; write string->function xrefs.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DumpAll extends GhidraScript {
    @Override public void run() throws Exception {
        String out = getScriptArgs()[0];
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(out + ".c"))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) break;
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                w.println("// ==== " + f.getEntryPoint() + " " + f.getName());
                if (r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
                else w.println("// decompile failed: " + r.getErrorMessage());
            }
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(out + ".strxref.tsv"))) {
            for (Data d : currentProgram.getListing().getDefinedData(true)) {
                if (!d.hasStringValue()) continue;
                String s = String.valueOf(d.getValue()).replace("\n","\\n").replace("\t","\\t");
                for (Reference ref : getReferencesTo(d.getAddress())) {
                    Function f = getFunctionContaining(ref.getFromAddress());
                    w.println(d.getAddress() + "\t" + (f == null ? "-" : f.getEntryPoint() + ":" + f.getName()) + "\t" + ref.getFromAddress() + "\t" + s);
                }
            }
        }
        println("dump done");
    }
}
