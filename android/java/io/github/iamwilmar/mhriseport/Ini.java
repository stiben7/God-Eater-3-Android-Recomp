package io.github.iamwilmar.mhriseport;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// <data>/mhr.ini: every choice of the settings menu, one "name = value" per line, instead of a file per switch. The
// runtime reads the same file (runtime/src/nnsdk/ini.h) and still takes the old empty files for switches the ini doesn't
// name, so a folder from before keeps its settings; changing a setting here moves it into the ini and drops its file.
final class Ini {
    static final String NAME = "mhr.ini";
    private static final String HEADER = "# God Eater 3 port settings, written by the settings menu. name = value; 1 is on.";

    private final File folder;
    private final LinkedHashMap<String, String> values = new LinkedHashMap<>();

    Ini(File folder) {
        this.folder = folder;
        load();
        if (folder != null) migrateDriverEnv();
    }

    // Again before each change: the in-game panel also writes the file (the FPS counter).
    private void load() {
        values.clear();
        if (folder == null) return;
        try {
            for (String line : Files.readAllLines(new File(folder, NAME).toPath(), StandardCharsets.UTF_8)) {
                String l = line.trim();
                int eq = l.indexOf('=');
                if (l.isEmpty() || l.startsWith("#") || l.startsWith(";") || l.startsWith("[") || eq <= 0) continue;
                values.put(l.substring(0, eq).trim(), l.substring(eq + 1).trim());
            }
        } catch (IOException ignored) {
            // (not there yet)
        }
    }

    String get(String name) {
        return values.get(name);
    }

    // On in the ini, or (when the ini doesn't name it) the old empty file is there.
    boolean on(String name) {
        String v = values.get(name);
        if (v == null) return folder != null && new File(folder, name).exists();
        if (v.equals("on") || v.equals("true") || v.equals("yes")) return true;
        try {
            return Float.parseFloat(v) != 0f;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    // null or empty removes the line. Drops the old file of that name too, so the two can't disagree.
    void set(String name, String value) {
        load();
        if (value == null || value.isEmpty()) values.remove(name);
        else values.put(name, value);
        if (folder != null) new File(folder, name).delete();
        save();
    }

    void setOn(String name, boolean on) {
        set(name, on ? "1" : null);
    }

    // Everything back to the defaults: the ini goes, and so do the old empty files of the given switches.
    void reset(String[] oldFiles) {
        values.clear();
        if (folder == null) return;
        new File(folder, NAME).delete();
        for (String name : oldFiles) new File(folder, name).delete();
    }

    // The driver variables (Debug → Driver environment) as NAME=value, from their "env.NAME" lines.
    List<String> env() {
        List<String> out = new ArrayList<>();
        for (Map.Entry<String, String> e : values.entrySet())
            if (e.getKey().startsWith("env.")) out.add(e.getKey().substring(4) + "=" + e.getValue());
        return out;
    }

    void setEnv(List<String> list) {
        load();
        values.keySet().removeIf(k -> k.startsWith("env."));
        for (String l : list) {
            int eq = l.indexOf('=');
            if (eq > 0) values.put("env." + l.substring(0, eq).trim(), l.substring(eq + 1).trim());
        }
        save();
    }

    // driver_env.txt from earlier builds: its variables go into the ini.
    private void migrateDriverEnv() {
        File old = new File(folder, "driver_env.txt");
        if (!old.isFile()) return;
        try {
            List<String> list = env();
            for (String line : Files.readAllLines(old.toPath(), StandardCharsets.UTF_8)) {
                String l = line.trim();
                if (!l.isEmpty() && !l.startsWith("#") && l.indexOf('=') > 0) list.add(l);
            }
            setEnv(list);
            old.delete();
        } catch (IOException ignored) {
            // (unreadable: left where it is)
        }
    }

    private void save() {
        if (folder == null) return;
        StringBuilder out = new StringBuilder(HEADER).append('\n');
        for (Map.Entry<String, String> e : values.entrySet()) out.append(e.getKey()).append(" = ").append(e.getValue()).append('\n');
        File tmp = new File(folder, NAME + ".tmp");
        try {
            Files.write(tmp.toPath(), out.toString().getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(new File(folder, NAME))) tmp.delete();
        } catch (IOException ignored) {
            // (storage refused: the row shows the old value next time)
        }
    }
}
