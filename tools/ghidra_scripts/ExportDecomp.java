// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Andrew Spode
// Create functions at every address in config/symbols.txt and write decompiled C for each to out/decomp/<flashaddr>.c
// @category HX3302B
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String root = getScriptArgs()[0];
        List<String> lines = Files.readAllLines(Paths.get(root, "config", "symbols.txt"));
        Path outDir = Paths.get(root, "out", "decomp"); Files.createDirectories(outDir);
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
        int n = 0, ok = 0;
        for (String l : lines) {
            if (l.startsWith("#") || l.isBlank()) continue;
            String[] f = l.split("\\s+");
            long a = Long.parseLong(f[0].substring(2), 16);
            String name = f[3];
            Address ad = sp.getAddress(a);
            n++;
            try {
                if (getFunctionAt(ad) == null) {
                    AddressSet s = new AddressSet(ad, ad.add(Long.decode(f[2]) - 1));
                    new DisassembleCommand(s, s, true).applyTo(currentProgram, monitor);
                    new CreateFunctionCmd(ad).applyTo(currentProgram, monitor);
                }
                Function fn = getFunctionAt(ad);
                if (fn == null) continue;
                DecompileResults r = di.decompileFunction(fn, 30, monitor);
                if (r.decompileCompleted() && r.getDecompiledFunction() != null) {
                    Files.writeString(outDir.resolve(String.format("%08x.c", a)),
                        "// " + name + "  " + l.substring(l.indexOf(';') + 1).trim() + "\n" + r.getDecompiledFunction().getC());
                    ok++;
                }
            } catch (Exception e) { println("skip " + Long.toHexString(a) + ": " + e); }
            if (n % 100 == 0) println("progress " + n + " ok " + ok);
        }
        println("done " + n + " functions, " + ok + " decompiled");
    }
}
