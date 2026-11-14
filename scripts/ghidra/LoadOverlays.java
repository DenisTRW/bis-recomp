// Lädt alle ARM9-Overlays als Ghidra-Overlay-Blöcke und übernimmt die
// Funktionen (Name, ARM/Thumb, Adresse) aus den dsd-symbols.txt.
//
// Aufruf (headless, vor der Analyse):
//   -preScript LoadOverlays.java <Projektwurzel>
//
// Mehrfach ausführbar: vorhandene Overlay-Blöcke werden übersprungen,
// vorhandene Funktionen nur umbenannt.
import java.io.ByteArrayInputStream;
import java.math.BigInteger;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class LoadOverlays extends GhidraScript {

    private static final Pattern FUNC = Pattern.compile(
        "^(\\S+) kind:function\\((arm|thumb)[^)]*\\) addr:0x([0-9a-fA-F]+)");

    private Register tmode;
    private int created, renamed, failed;
    private final List<String> failLog = new ArrayList<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("Argument fehlt: Projektwurzel");
            return;
        }
        Path root = Path.of(args[0]);
        Path ext = root.resolve("extract/eur/arm9_overlays");
        Path cfg = root.resolve("config/eur/arm9");
        tmode = currentProgram.getRegister("TMode");
        Memory mem = currentProgram.getMemory();

        // Hauptprogramm: Symbole aus dsd übernehmen
        AddressSpace mainSpace = currentProgram.getAddressFactory().getDefaultAddressSpace();
        applySymbols(cfg.resolve("symbols.txt"), mainSpace);
        println("main: " + created + " Funktionen neu, " + renamed + " umbenannt, " + failed + " Fehler");

        // Overlays
        for (OverlayInfo ov : parseOverlays(ext.resolve("overlays.yaml"))) {
            monitor.checkCancelled();
            String name = String.format("ov%03d", ov.id);
            MemoryBlock block = mem.getBlock(name);
            if (block == null) {
                byte[] code = Files.readAllBytes(ext.resolve(name + ".bin"));
                byte[] all = new byte[code.length + ov.bssSize];   // BSS mit Nullen
                System.arraycopy(code, 0, all, 0, code.length);
                block = mem.createInitializedBlock(name, toAddr(ov.base),
                    new ByteArrayInputStream(all), all.length, monitor, true);
                block.setRead(true);
                block.setWrite(true);
                block.setExecute(true);
            }
            created = renamed = failed = 0;
            Path syms = cfg.resolve("overlays/" + name + "/symbols.txt");
            if (Files.exists(syms)) {
                applySymbols(syms, block.getStart().getAddressSpace());
            }
            println(name + " @ 0x" + Long.toHexString(ov.base) + ": " + created +
                    " Funktionen neu, " + renamed + " umbenannt, " + failed + " Fehler");
        }
        for (String s : failLog) println("FEHLER " + s);
    }

    private void applySymbols(Path file, AddressSpace space) throws Exception {
        for (String line : Files.readAllLines(file)) {
            Matcher m = FUNC.matcher(line);
            if (!m.find()) continue;
            String name = m.group(1);
            boolean thumb = m.group(2).equals("thumb");
            Address addr = space.getAddress(Long.parseLong(m.group(3), 16));
            try {
                Function f = getFunctionAt(addr);
                if (f == null) {
                    // Modus nur setzen, solange dort noch kein Befehl steht
                    if (getInstructionAt(addr) == null) {
                        if (tmode != null) {
                            currentProgram.getProgramContext().setValue(tmode, addr, addr,
                                thumb ? BigInteger.ONE : BigInteger.ZERO);
                        }
                        disassemble(addr);
                    }
                    f = createFunction(addr, name);
                    if (f != null) {
                        created++;
                    } else {
                        failed++;
                        if (failLog.size() < 20) failLog.add(addr + " " + name);
                    }
                } else if (!f.getName().equals(name)) {
                    f.setName(name, SourceType.IMPORTED);
                    renamed++;
                }
            } catch (Exception e) {
                failed++;
                if (failLog.size() < 20) failLog.add(addr + " " + name + ": " + e.getMessage());
            }
        }
    }

    private static class OverlayInfo {
        int id;
        long base;
        int bssSize;
    }

    private List<OverlayInfo> parseOverlays(Path yaml) throws Exception {
        List<OverlayInfo> list = new ArrayList<>();
        OverlayInfo cur = null;
        for (String raw : Files.readAllLines(yaml)) {
            String line = raw.trim();
            if (line.startsWith("- id:")) {
                cur = new OverlayInfo();
                cur.id = Integer.parseInt(line.substring(5).trim());
                list.add(cur);
            } else if (cur != null && line.startsWith("base_address:")) {
                cur.base = Long.parseLong(line.substring(13).trim());
            } else if (cur != null && line.startsWith("bss_size:")) {
                cur.bssSize = Integer.parseInt(line.substring(9).trim());
            }
        }
        return list;
    }
}
