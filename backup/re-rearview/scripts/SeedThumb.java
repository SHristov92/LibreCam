// Pre-analysis: add RAM/peripheral blocks and seed Thumb functions from a list.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import java.math.BigInteger;
import java.nio.file.*;

public class SeedThumb extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        var mem = currentProgram.getMemory();
        if (mem.getBlock(toAddr(0x400000)) == null) {
            mem.createUninitializedBlock("SRAM", toAddr(0x400000), 0x100000, false).setWrite(true);
            var p = mem.createUninitializedBlock("PERIPH", toAddr(0x800000), 0x100000, false);
            p.setWrite(true); p.setVolatile(true);
            mem.createUninitializedBlock("PSRAM", toAddr(0x900000), 0x800000, false).setWrite(true);
        }
        Register tm = currentProgram.getRegister("TMode");
        int n = 0;
        for (String line : Files.readAllLines(Paths.get(a[0]))) {
            line = line.trim(); if (line.isEmpty()) continue;
            Address ad = toAddr(Long.parseLong(line, 16));
            if (getInstructionContaining(ad) != null) continue;
            try {
                currentProgram.getProgramContext().setValue(tm, ad, ad, BigInteger.ONE);
                new DisassembleCommand(ad, null, true).applyTo(currentProgram, monitor);
                if (getFunctionAt(ad) == null && createFunction(ad, null) != null) n++;
            } catch (Exception e) { println("skip " + ad + ": " + e.getMessage()); }
        }
        println("seeded thumb functions: " + n);
    }
}
