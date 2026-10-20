// Force Thumb functions at the given hex addresses (clearing conflicting code), decompile them to <out>.
// Args: <out-file> <addr> [addr...]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import java.io.*;
import java.math.BigInteger;

public class DecompAt extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        Register tm = currentProgram.getRegister("TMode");
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            for (int i = 1; i < a.length; i++) {
                Address ad = toAddr(Long.parseLong(a[i].replace("0x", ""), 16) & ~1L);
                Function f = getFunctionAt(ad);
                if (f == null) {
                    Function owner = getFunctionContaining(ad);
                    if (owner != null) removeFunction(owner);
                    clearListing(ad, ad.add(64));
                    currentProgram.getProgramContext().setValue(tm, ad, ad.add(1), BigInteger.ONE);
                    new DisassembleCommand(ad, null, true).applyTo(currentProgram, monitor);
                    f = createFunction(ad, null);
                }
                w.println("// ==== " + ad + (f == null ? " (no function)" : " " + f.getName()));
                if (f == null) continue;
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
    }
}
