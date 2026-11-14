// Gibt den decompilierten Pseudocode der Funktionen an den übergebenen Adressen aus.
// Aufruf (headless): -postScript DecompileAt.java 0x02005f24 0x02019a2c ...
// Ist eine Adresse noch keine Funktion, wird sie angelegt (ARM-Modus).
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Address addr = toAddr(arg);
            Function f = getFunctionAt(addr);
            if (f == null) {
                disassemble(addr);
                f = createFunction(addr, null);
            }
            if (f == null) {
                println("=== " + arg + ": keine Funktion");
                continue;
            }
            DecompileResults res = ifc.decompileFunction(f, 60, monitor);
            println("=== " + f.getName() + " @ " + arg);
            if (res.decompileCompleted()) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("Fehler: " + res.getErrorMessage());
            }
        }
        ifc.dispose();
    }
}
