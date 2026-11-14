// Listet alle Funktionen, die eine Funktion an den übergebenen Adressen aufrufen.
// Aufruf (headless): -postScript ListCallers.java 0x0203c180 ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class ListCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address target = toAddr(arg);
            println("=== Aufrufer von " + arg);
            for (Reference ref : getReferencesTo(target)) {
                if (!ref.getReferenceType().isCall()) continue;
                Function f = getFunctionContaining(ref.getFromAddress());
                println("  " + ref.getFromAddress() + "  in " +
                        (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
            }
        }
    }
}
