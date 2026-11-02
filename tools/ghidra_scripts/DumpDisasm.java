// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Andrew Spode
// Disassemble an address range and print it. Args: startHex endHex
// @category HX3302B
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;

public class DumpDisasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long s = Long.parseLong(a[0], 16), e = Long.parseLong(a[1], 16);
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        AddressSet set = new AddressSet(sp.getAddress(s), sp.getAddress(e));
        new DisassembleCommand(set, set, true).applyTo(currentProgram, monitor);
        Listing l = currentProgram.getListing();
        for (Instruction i : l.getInstructions(set, true))
            println(String.format("%08x  %-8s %s", i.getAddress().getOffset(), i.getMnemonicString(),
                i.toString().substring(i.getMnemonicString().length()).trim()));
    }
}
