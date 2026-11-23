package io.github.iamwilmar.mhriseport;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// The mods folder, as Eden lays out a game's add-ons: <data>/mods/<name>/romfs/... (files that replace the game's,
// fs.cpp) and <data>/mods/<name>/cheats/<build id>.txt (Atmosphere cheats, cheats.cpp). A folder whose name starts with
// '#' is turned off. What the settings menu's Mods section reads and changes.
final class ModFiles {
    private ModFiles() {}

    static final class Mod {
        String name;      // without the '#'
        File dir;
        boolean on;
        boolean romfs, cheats;
    }

    static final class Cheat {
        String name;
        boolean master;
        String key;  // its switch in mhr.ini (cheat_<hash>), the same one cheats.cpp reads
    }

    static File modsDir(File data) {
        return new File(data, "mods");
    }

    static List<Mod> list(File data) {
        List<Mod> out = new ArrayList<>();
        File[] dirs = modsDir(data).listFiles(File::isDirectory);
        if (dirs == null) return out;
        Arrays.sort(dirs, (a, b) -> bare(a.getName()).compareToIgnoreCase(bare(b.getName())));
        for (File d : dirs) {
            if (d.getName().startsWith(".")) continue;
            Mod m = new Mod();
            m.dir = d;
            m.on = !d.getName().startsWith("#");
            m.name = bare(d.getName());
            m.romfs = new File(d, "romfs").isDirectory();
            m.cheats = new File(d, "cheats").isDirectory();
            out.add(m);
        }
        return out;
    }

    private static String bare(String name) {
        return name.startsWith("#") ? name.substring(1) : name;
    }

    // On or off: the folder gets or loses its '#'
    static boolean setOn(Mod m, boolean on) {
        File to = new File(m.dir.getParentFile(), (on ? "" : "#") + m.name);
        if (to.equals(m.dir)) return true;
        if (to.exists() || !m.dir.renameTo(to)) return false;
        m.dir = to;
        m.on = on;
        return true;
    }

    static boolean delete(Mod m) {
        return deleteTree(m.dir);
    }

    private static boolean deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids) deleteTree(k);
        return f.delete();
    }

    // main's build ID (the first 8 bytes of the module ID in its NSO header, +0x40), the name cheat files go by.
    // null if the game's exefs isn't where rutas.txt says.
    static String buildId(File data) {
        DumpScanner.Result r = DumpScanner.load(data);
        File main = new File(r != null && r.exefs != null ? r.exefs : new File(data, "exefs").getPath(), "main");
        try (RandomAccessFile f = new RandomAccessFile(main, "r")) {
            byte[] id = new byte[8];
            f.seek(0x40);
            f.readFully(id);
            StringBuilder b = new StringBuilder();
            for (byte x : id) b.append(String.format(Locale.ROOT, "%02X", x & 0xFF));
            return b.toString();
        } catch (IOException e) {
            return null;
        }
    }

    // The cheat files of a mod for this build (the others are for another game version and don't run)
    static List<File> cheatFiles(Mod m, String buildId) {
        List<File> out = new ArrayList<>();
        File[] files = new File(m.dir, "cheats").listFiles();
        if (files == null || buildId == null) return out;
        Arrays.sort(files);
        for (File f : files) {
            String n = f.getName();
            if (n.toLowerCase(Locale.ROOT).endsWith(".txt") && n.length() >= 20 && n.substring(0, 16).equalsIgnoreCase(buildId))
                out.add(f);
        }
        return out;
    }

    // The mod's cheat files that are for another build
    static int otherBuilds(Mod m, String buildId) {
        File[] files = new File(m.dir, "cheats").listFiles((d, n) -> n.toLowerCase(Locale.ROOT).endsWith(".txt"));
        return files == null ? 0 : files.length - cheatFiles(m, buildId).size();
    }

    static List<Cheat> cheats(Mod m, File file) {
        List<Cheat> out = new ArrayList<>();
        try {
            for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
                String l = line.trim();
                if (l.isEmpty() || (l.charAt(0) != '[' && l.charAt(0) != '{')) continue;
                int end = l.indexOf(l.charAt(0) == '[' ? ']' : '}');
                Cheat c = new Cheat();
                c.name = (end < 0 ? l.substring(1) : l.substring(1, end)).trim();
                c.master = l.charAt(0) == '{';
                c.key = key(m.name, c.name);
                out.add(c);
            }
        } catch (IOException ignored) {
            // (unreadable: no cheats)
        }
        return out;
    }

    // FNV-1a of "mod/name" in UTF-8, as cheats.cpp does
    static String key(String mod, String cheat) {
        int h = 0x811c9dc5;
        for (byte b : (mod + "/" + cheat).getBytes(StandardCharsets.UTF_8)) {
            h ^= b & 0xFF;
            h *= 0x01000193;
        }
        return String.format(Locale.ROOT, "cheat_%08x", h);
    }

    // ---- importing ------------------------------------------------------------------------------------------------------

    static String displayName(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (RuntimeException ignored) {
            // (no name: the caller makes one up)
        }
        return null;
    }

    private static String folderName(String file, String fallback) {
        String n = file != null ? file : fallback;
        int dot = n.lastIndexOf('.');
        if (dot > 0) n = n.substring(0, dot);
        n = n.replaceAll("[\\\\/:*?\"<>|]", "_").trim();
        while (n.startsWith("#") || n.startsWith(".")) n = n.substring(1);
        return n.isEmpty() ? fallback : n;
    }

    private static File freeDir(File data, String name) {
        File d = new File(modsDir(data), name);
        for (int i = 2; d.exists() || new File(modsDir(data), "#" + d.getName()).exists(); i++)
            d = new File(modsDir(data), name + " (" + i + ")");
        return d;
    }

    // A mod's zip, as made for Eden or Yuzu: somewhere inside it a romfs, exefs or cheats folder (maybe under the title ID
    // or the mod's own folder), or just the game's own folders (natives/...), which then go into romfs. Returns what
    // went in, for the toast.
    static String importZip(ContentResolver cr, Uri uri, File data) throws IOException {
        List<String> names = new ArrayList<>();
        try (InputStream in = cr.openInputStream(uri); ZipInputStream z = new ZipInputStream(in)) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; )
                if (!e.isDirectory()) names.add(e.getName().replace('\\', '/'));
        }
        if (names.isEmpty()) throw new IOException("the zip is empty");
        // Where the mod starts: the shortest path before a romfs, exefs or cheats folder
        String prefix = null, into = "";
        for (String n : names) {
            String[] parts = n.split("/");
            for (int i = 0; i < parts.length - 1; i++) {
                String p = parts[i].toLowerCase(Locale.ROOT);
                if (p.equals("romfs") || p.equals("exefs") || p.equals("cheats")) {
                    String pre = String.join("/", Arrays.copyOfRange(parts, 0, i));
                    if (prefix == null || pre.length() < prefix.length()) prefix = pre;
                    break;
                }
            }
        }
        if (prefix == null) {
            for (String n : names) {
                int at = ("/" + n).toLowerCase(Locale.ROOT).indexOf("/natives/");
                if (at >= 0) {
                    String pre = n.substring(0, Math.max(0, at - 1));  // (at is in "/" + n: the part before "/natives/")
                    if (prefix == null || pre.length() < prefix.length()) prefix = pre;
                }
            }
            if (prefix == null) throw new IOException("no romfs, exefs or cheats folder in it (is it a Switch mod?)");
            into = "romfs/";
        }
        String start = prefix.isEmpty() ? "" : prefix + "/";
        File dir = freeDir(data, folderName(displayName(cr, uri), "Mod"));
        int files = 0;
        boolean exefs = false;
        String root = dir.getCanonicalPath() + File.separator;
        try (InputStream in = cr.openInputStream(uri); ZipInputStream z = new ZipInputStream(in)) {
            byte[] buf = new byte[1 << 16];
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                String n = e.getName().replace('\\', '/');
                if (e.isDirectory() || !n.startsWith(start)) continue;
                String rest = n.substring(start.length());
                // romfs/exefs/cheats in whatever case the zip has them
                int slash = rest.indexOf('/');
                if (into.isEmpty() && slash > 0) rest = rest.substring(0, slash).toLowerCase(Locale.ROOT) + rest.substring(slash);
                if (rest.startsWith("exefs/")) exefs = true;
                File out = new File(dir, into + rest);
                if (!out.getCanonicalPath().startsWith(root)) continue;  // (a "../" name: not outside the folder)
                File parent = out.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("can't write " + parent);
                try (OutputStream o = new FileOutputStream(out)) {
                    for (int k; (k = z.read(buf)) > 0; ) o.write(buf, 0, k);
                }
                files++;
            }
        }
        if (files == 0) throw new IOException("nothing to take from it");
        return "\"" + dir.getName() + "\": " + files + " file(s)" + (exefs ? " (its exefs patches aren't used by this port)" : "");
    }

    // One cheat file (.txt), into a mod of its own name: mods/<name>/cheats/<build id>.txt. A file named after another
    // build ID is for another version of the game, and isn't taken.
    static String importCheats(ContentResolver cr, Uri uri, File data) throws IOException {
        String buildId = buildId(data);
        if (buildId == null) throw new IOException("can't read the game's build ID (is the exefs still there?)");
        String file = displayName(cr, uri);
        String base = file != null ? file.replaceAll("(?i)\\.txt$", "") : "";
        if (base.matches("[0-9A-Fa-f]{16,64}") && !base.substring(0, 16).equalsIgnoreCase(buildId))
            throw new IOException("that file is for another game version (build " + base.substring(0, 16).toUpperCase(Locale.ROOT)
                    + "; this one is " + buildId + ")");
        byte[] text;
        try (InputStream in = cr.openInputStream(uri)) {
            if (in == null) throw new IOException("can't open it");
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int k; (k = in.read(buf)) > 0; ) b.write(buf, 0, k);
            text = b.toByteArray();
        }
        String s = new String(text, StandardCharsets.UTF_8);
        if (!s.contains("[") && !s.contains("{")) throw new IOException("no [cheat] in it");
        File dir = freeDir(data, folderName(base.matches("[0-9A-Fa-f]{16,64}") ? null : file, "Cheats"));
        File cheats = new File(dir, "cheats");
        if (!cheats.mkdirs()) throw new IOException("can't write " + cheats);
        Files.write(new File(cheats, buildId + ".txt").toPath(), text);
        return "\"" + dir.getName() + "\"";
    }
}
