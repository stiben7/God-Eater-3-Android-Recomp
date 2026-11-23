package io.github.iamwilmar.mhriseport;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

// The settings menu's Saves section: the port's save (<data>/save/, the Switch's files) out to a zip, a save from a zip
// or a folder in (Switch's as they are, Steam's converted), either one converted to the other platform into a zip, and
// the backups each import leaves. SaveData does the files themselves.
//
//   <data>/save_backups/<date>/  the save as it was before each import or restore (the last 10)
//   <data>/save_steam/           the last Steam save brought in, decrypted (<name>.steam), to fill when going back to Steam
//   mhr.ini steam_id, steam_curve  that save's Steam ID and the curve its key is on (SaveData.Citrus), to encrypt with
final class SaveFiles {
    private SaveFiles() {}

    static final String[] MAIN = {SaveData.SLOTS[0], SaveData.SLOTS[1], SaveData.SLOTS[2], SaveData.SYSTEM};
    private static final int KEEP_BACKUPS = 10;

    static File saveDir(File data) {
        return new File(data, "save");
    }

    static File backupsDir(File data) {
        return new File(data, "save_backups");
    }

    static File steamDir(File data) {
        return new File(data, "save_steam");
    }

    // ---- what was picked ------------------------------------------------------------------------------------------------

    // The save files found in a zip or a folder, and the Steam IDs it gives away: an SS1_* file has the account's number,
    // a Goldberg/GSE emulator's configs.user.ini has account_steamid, and Steam's own folder is userdata/<number>/.
    static final class Incoming {
        final Map<String, byte[]> files = new LinkedHashMap<>();
        private final Map<String, Integer> depth = new LinkedHashMap<>();
        final Map<String, String> paths = new LinkedHashMap<>();  // where in the zip or folder each one was
        final List<Long> ids = new ArrayList<>();

        void offer(String path, byte[] data) {
            String p = path.replace('\\', '/');
            String name = p.substring(p.lastIndexOf('/') + 1);
            int d = p.split("/").length;
            Matcher m = USERDATA.matcher(p);
            if (m.find()) id(SaveData.steamId(m.group(1)));
            if (name.startsWith("SS1_") && data.length > 0x18 && SaveData.le32(data, 8) == SaveData.FLAG_HAS_ID)
                id(SaveData.steamId(Long.toString(SaveData.le64(data, 0x10))));
            if (name.equalsIgnoreCase("configs.user.ini")) {
                Matcher a = GSE_ID.matcher(new String(data, StandardCharsets.UTF_8));
                if (a.find()) id(SaveData.steamId(a.group(1)));
            }
            String key = null;
            for (String main : MAIN)
                if (name.equalsIgnoreCase(main)) key = main;
            if (ss1(name) && name.toLowerCase(Locale.ROOT).endsWith(".bin")) key = name;
            // The one nearest the top, if there are several (a backup folder inside, say)
            if (key != null && (!depth.containsKey(key) || d < depth.get(key))) {
                files.put(key, data);
                depth.put(key, d);
                paths.put(key, p);
            }
        }

        void id(long id) {
            if (id != 0 && !ids.contains(id)) ids.add(id);
        }

        static boolean wanted(String name) {
            for (String main : MAIN)
                if (name.equalsIgnoreCase(main)) return true;
            return name.startsWith("SS1_") || name.equalsIgnoreCase("configs.user.ini");
        }
    }

    // The SS1_* files: the photos taken in the game (a 960x960 JPEG in each of 002-009, its thumbnail in 302-309, the list
    // of them in 001), plain on the Switch, with the account's number on Steam
    static boolean ss1(String name) {
        return name.startsWith("SS1_");
    }

    private static final Pattern USERDATA = Pattern.compile("(?i)userdata/(\\d{1,10})/1446780/");
    private static final Pattern GSE_ID = Pattern.compile("(?m)^\\s*account_steamid\\s*=\\s*(\\d+)");

    static Incoming readZip(ContentResolver cr, Uri uri) throws IOException {
        Incoming in = new Incoming();
        try (InputStream s = cr.openInputStream(uri); ZipInputStream z = new ZipInputStream(s)) {
            if (s == null) throw new IOException("can't open it");
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                String n = e.getName().replace('\\', '/');
                if (!e.isDirectory() && Incoming.wanted(n.substring(n.lastIndexOf('/') + 1))) in.offer(n, readAll(z));
            }
        }
        return in;
    }

    static Incoming readFolder(ContentResolver cr, Uri tree) throws IOException {
        Incoming in = new Incoming();
        walk(cr, tree, DocumentsContract.getTreeDocumentId(tree), "", 0, in);
        return in;
    }

    private static void walk(ContentResolver cr, Uri tree, String doc, String path, int depth, Incoming in) throws IOException {
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, doc);
        List<String[]> found = new ArrayList<>();
        try (Cursor c = cr.query(kids, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (c != null && c.moveToNext()) found.add(new String[] {c.getString(0), c.getString(1), c.getString(2)});
        } catch (RuntimeException e) {
            throw new IOException("can't read that folder");
        }
        for (String[] k : found) {
            String p = path + k[1];
            if (DocumentsContract.Document.MIME_TYPE_DIR.equals(k[2])) {
                if (depth < 7) walk(cr, tree, k[0], p + "/", depth + 1, in);
            } else if (Incoming.wanted(k[1])) {
                try (InputStream s = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, k[0]))) {
                    if (s != null) in.offer(p, readAll(s));
                }
            }
        }
    }

    // ---- opening and converting -----------------------------------------------------------------------------------------

    static SaveData.Citrus citrus(Context c) throws IOException {
        try (InputStream in = c.getAssets().open("citrus_curves.bin")) {
            return new SaveData.Citrus(readAll(in));
        }
    }

    // Every main file it has, opened; a Steam one with the IDs it gave, the one remembered and the one typed in (0 if
    // none). Throws SaveData.Citrus.NeedId when its ID isn't among them.
    static Map<String, SaveData.Opened> open(Context c, Incoming in, File data, long typed) throws IOException {
        if (in.files.isEmpty()) throw new IOException("no save files in it (data00-1.bin, data001Slot.bin...)");
        Ini ini = new Ini(data);
        List<Long> ids = new ArrayList<>();
        if (typed != 0) ids.add(typed);
        for (long id : in.ids)
            if (!ids.contains(id)) ids.add(id);
        long known = SaveData.steamId(ini.get("steam_id"));
        if (known != 0 && !ids.contains(known)) ids.add(known);
        long[] tryIds = new long[ids.size()];
        for (int i = 0; i < tryIds.length; i++) tryIds[i] = ids.get(i);
        int curve = curve(ini);
        SaveData.Citrus citrus = null;
        Map<String, SaveData.Opened> out = new LinkedHashMap<>();
        for (Map.Entry<String, byte[]> f : in.files.entrySet()) {
            byte[] d = f.getValue();
            if (citrus == null && d.length > 12 && SaveData.le32(d, 8) == SaveData.FLAG_CITRUS) citrus = citrus(c);
            SaveData.Opened o = SaveData.open(f.getKey(), d, tryIds, citrus, curve);
            if (o.curve >= 0) {
                // Remember which ID opened it, and its curve: what encrypts a save for that account later
                curve = o.curve;
                tryIds = new long[] {o.steamId};
                ini.set("steam_id", Long.toString(o.steamId));
                ini.set("steam_curve", Integer.toString(curve));
            }
            out.put(f.getKey(), o);
        }
        return out;
    }

    private static int curve(Ini ini) {
        try {
            int v = Integer.parseInt(ini.get("steam_curve").trim());
            return v >= 0 && v < 128 ? v : -1;
        } catch (RuntimeException e) {
            return -1;
        }
    }

    // The layout a converted file is filled into: the port's own save's file, or another slot's (same layout), or one in
    // the backups if the save folder has none the Switch's way (another release's files copied in by hand, say)
    private static List<SaveData.Root> current(File data, String name) {
        List<File> dirs = new ArrayList<>();
        dirs.add(saveDir(data));
        dirs.addAll(backups(data));
        for (File dir : dirs) {
            List<SaveData.Root> r = switchRoots(new File(dir, name), name);
            for (int i = 0; r == null && SaveData.slot(name) && i < SaveData.SLOTS.length; i++)
                r = switchRoots(new File(dir, SaveData.SLOTS[i]), name);
            if (r != null) return r;
        }
        return null;
    }

    private static List<SaveData.Root> switchRoots(File f, String name) {
        if (!f.isFile()) return null;
        try {
            SaveData.Opened o = SaveData.open(name, Files.readAllBytes(f.toPath()), new long[0], null, -1);
            return o.steam || o.foreign ? null : o.roots;
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    private static List<SaveData.Root> steamTemplate(File data, String name) {
        File f = new File(steamDir(data), name + ".steam");
        for (int i = 0; !f.isFile() && SaveData.slot(name) && i < SaveData.SLOTS.length; i++)
            f = new File(steamDir(data), SaveData.SLOTS[i] + ".steam");
        try {
            return SaveData.parse(Files.readAllBytes(f.toPath()), SaveData.STEAM_BASE);
        } catch (IOException e) {
            return null;
        }
    }

    // A Steam file as the Switch's, filled into the port's own save (its layout) when there is one
    private static byte[] toSwitch(File data, SaveData.Opened o, StringBuilder notes) throws IOException {
        if (ss1(o.name)) return SaveData.switchFile(o.name, SaveData.write(o.roots, SaveData.SWITCH_BASE));
        List<SaveData.Root> template = current(data, o.name);
        // Another release's file left as it is would crash the game: it needs 1.16's layout to fill
        if (template == null && o.foreign)
            throw new IOException("a save from another release needs one of this version to fill. Start the game, make a "
                    + "hunter and save once, then import it again");
        String none = " No Switch save was here to take the layout from, so only what Steam alone has was left out.";
        if (template == null && notes.indexOf(none) < 0) notes.append(none);
        SaveData.Counts n = new SaveData.Counts();
        return SaveData.switchFile(o.name, SaveData.write(SaveData.fill(template, o.roots, o.name, true, n), SaveData.SWITCH_BASE));
    }

    // A Switch file as Steam's: filled into the last Steam save brought in, and encrypted for its account if that's known
    private static byte[] toSteam(Context c, File data, SaveData.Opened o, boolean[] encrypted, StringBuilder notes)
            throws IOException {
        Ini ini = new Ini(data);
        long id = SaveData.steamId(ini.get("steam_id"));
        int curve = curve(ini);
        if (ss1(o.name)) {
            // (photos: nothing in them differs between the two, so no filling; and no encryption on Steam either)
            if (id != 0 && curve >= 0) return SaveData.steamSs1File(SaveData.write(o.roots, SaveData.STEAM_SS1_BASE), id);
            return SaveData.plainFile(SaveData.write(o.roots, SaveData.SWITCH_BASE));
        }
        List<SaveData.Root> template = steamTemplate(data, o.name);
        List<SaveData.Root> roots = SaveData.fill(template, o.roots, o.name, false, new SaveData.Counts());
        if (id != 0 && curve >= 0) {
            encrypted[0] = true;
            return SaveData.steamFile(SaveData.write(roots, SaveData.STEAM_BASE), id, citrus(c), curve);
        }
        encrypted[0] = false;
        return SaveData.plainFile(SaveData.write(roots, SaveData.SWITCH_BASE));  // (what a save editor reads and encrypts)
    }

    private static String steamNotes(boolean encrypted, boolean template) {
        String s = encrypted ? " Encrypted for the Steam account set in Saves."
                : " Not encrypted: set the Steam account first (Saves → Set from a Steam save), or encrypt these with a save "
                        + "editor such as ree-save-editor.";
        if (!template) s += " Without a Steam save of yours to fill, a few things only the PC version has are missing.";
        return s;
    }

    // Kept to fill when a save goes back to Steam: what only the PC version has comes from here
    private static void keepSteam(File data, SaveData.Opened o) throws IOException {
        if (ss1(o.name)) return;
        File dir = steamDir(data);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("can't write " + dir);
        writeFile(new File(dir, o.name + ".steam"), SaveData.write(o.roots, SaveData.STEAM_BASE));
    }

    // The Steam account saves are exported for: the one of the last Steam save opened here, if its key is known
    static String steamAccount(File data) {
        Ini ini = new Ini(data);
        long id = SaveData.steamId(ini.get("steam_id"));
        if (id == 0 || curve(ini) < 0) return null;
        boolean slots = false;
        for (String n : SaveData.SLOTS) slots |= new File(steamDir(data), n + ".steam").isFile();
        boolean system = new File(steamDir(data), SaveData.SYSTEM + ".steam").isFile();
        return id + (slots && system ? "" : " (from a save without " + (slots ? SaveData.SYSTEM : "a hunter's file") + ")");
    }

    // Only learns the account from a Steam save (its key, and its layout to fill), without importing anything: for a
    // save started in the port that's going to a Steam account it has never seen
    static String learnSteam(Context c, Incoming in, File data, long typed) throws IOException {
        Map<String, SaveData.Opened> opened = open(c, in, data, typed);
        List<String> kept = new ArrayList<>();
        for (SaveData.Opened o : opened.values()) {
            if (!o.steam) continue;
            keepSteam(data, o);
            kept.add(o.name);
        }
        if (kept.isEmpty()) throw new IOException("that's a Switch save; pick one from the Steam account");
        if (steamAccount(data) == null) throw new IOException("it isn't encrypted, so it doesn't give the account's key; pick the save "
                + "as Steam wrote it");
        return "Steam account set from that save (" + String.join(", ", kept) + "). Export for Steam now encrypts for it.";
    }

    // ---- the menu's actions ---------------------------------------------------------------------------------------------

    // Puts the picked save in place of the port's (backed up first). Returns the message for the menu.
    static String install(Context c, Incoming in, File data, long typed) throws IOException {
        Map<String, SaveData.Opened> opened = open(c, in, data, typed);
        Map<String, byte[]> out = new LinkedHashMap<>();
        StringBuilder notes = new StringBuilder();
        boolean steam = false, foreign = foreign(opened);
        for (SaveData.Opened o : opened.values()) {
            if (foreign && ss1(o.name)) continue;  // (see foreign())
            if (!o.steam && !o.foreign) {
                out.put(o.name, in.files.get(o.name));  // (the Switch's file as it is)
                continue;
            }
            out.put(o.name, toSwitch(data, o, notes));
            if (o.steam) {
                steam = true;
                keepSteam(data, o);
            }
        }
        String backup = backup(data);
        File dir = saveDir(data);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("can't write " + dir);
        for (Map.Entry<String, byte[]> f : out.entrySet()) writeFile(new File(dir, f.getKey()), f.getValue());
        // The old save's photos (SS1_*) would be listed by nothing in the new one: out, they're in the backup. So do its
        // hunters when the new one has its own list of them (data00-1.bin): a slot file the list doesn't match, or one
        // from another release, crashes the game or shows another hunter
        boolean list = out.containsKey(SaveData.SYSTEM);
        File[] old = dir.listFiles((x, n) -> !out.containsKey(n) && (ss1(n) || list && SaveData.slot(n)));
        if (old != null)
            for (File f : old) f.delete();
        // The game's own copy of the last committed save (fs.cpp) is of the old one now; the next save makes it again
        new File(new File(data, "save.bak"), ".pendiente").delete();
        List<String> from = new ArrayList<>();
        int extra = 0;
        for (String n : out.keySet()) {
            if (ss1(n)) extra++;
            else from.add(in.paths.containsKey(n) ? in.paths.get(n) : n);
        }
        if (extra > 0) from.add(extra + " SS1_* file(s)");
        return (steam ? "Steam save converted and imported" : foreign ? "Save from another release converted and imported"
                : "Save imported") + " (" + String.join(", ", from) + ")."
                + (backup != null ? " The one before is in Saves → Restore a backup (" + backup + ")." : "") + notes;
    }

    // Plain main files: another release's save (the Netflix one, the save editors'). Its SS1_* photo list has another
    // length and no photos with it, so it's left out (the game makes its own).
    private static boolean foreign(Map<String, SaveData.Opened> opened) {
        for (SaveData.Opened o : opened.values())
            if (o.foreign) return true;
        return false;
    }

    static final class Zip {
        byte[] bytes;
        String name, message;
    }

    // The port's save as a zip, for the Switch, Eden or Yuzu (or another copy of the port)
    static Zip exportSwitch(File data) throws IOException {
        File[] files = saveDir(data).listFiles(File::isFile);
        if (files == null || files.length == 0) throw new IOException("there's no save yet");
        Arrays.sort(files);
        Map<String, byte[]> out = new LinkedHashMap<>();
        for (File f : files) out.put(f.getName(), Files.readAllBytes(f.toPath()));
        Zip z = new Zip();
        z.bytes = zip(out, "");
        z.name = "MHRise save " + stamp() + ".zip";
        z.message = "Save exported: " + String.join(", ", out.keySet()) + ".";
        return z;
    }

    // The port's save as Steam's, in win64_save/ (what goes in ...\1446780\remote\win64_save)
    static Zip exportSteam(Context c, File data) throws IOException {
        Map<String, byte[]> out = new LinkedHashMap<>();
        boolean[] encrypted = {false};
        boolean template = true;
        StringBuilder notes = new StringBuilder();
        List<String> names = new ArrayList<>(Arrays.asList(MAIN));
        String[] photos = saveDir(data).list((d, n) -> ss1(n) && n.toLowerCase(Locale.ROOT).endsWith(".bin"));
        if (photos != null) {
            Arrays.sort(photos);
            names.addAll(Arrays.asList(photos));
        }
        for (String name : names) {
            File f = new File(saveDir(data), name);
            if (!f.isFile()) continue;
            SaveData.Opened o = SaveData.open(name, Files.readAllBytes(f.toPath()), new long[0], null, -1);
            if (!ss1(name)) template &= steamTemplate(data, name) != null;
            out.put(name, toSteam(c, data, o, encrypted, notes));
        }
        if (out.isEmpty()) throw new IOException("there's no save yet");
        Zip z = new Zip();
        z.bytes = zip(out, "win64_save/");
        z.name = "MHRise Steam save " + stamp() + ".zip";
        z.message = "Steam save exported." + steamNotes(encrypted[0], template);
        return z;
    }

    // The picked save as the other platform's, into a zip; the port's own save isn't touched
    static Zip convert(Context c, Incoming in, File data, long typed) throws IOException {
        Map<String, SaveData.Opened> opened = open(c, in, data, typed);
        boolean steam = false, foreign = foreign(opened);
        for (SaveData.Opened o : opened.values()) steam |= o.steam || o.foreign;  // (to the Switch's, from either)
        Map<String, byte[]> out = new LinkedHashMap<>();
        StringBuilder notes = new StringBuilder();
        boolean[] encrypted = {false};
        boolean template = true;
        for (SaveData.Opened o : opened.values()) {
            if (foreign && ss1(o.name)) continue;
            if (steam) {
                out.put(o.name, o.steam || o.foreign ? toSwitch(data, o, notes) : in.files.get(o.name));
                if (o.steam) keepSteam(data, o);
            } else {
                if (!ss1(o.name)) template &= steamTemplate(data, o.name) != null;
                out.put(o.name, toSteam(c, data, o, encrypted, notes));
            }
        }
        Zip z = new Zip();
        z.bytes = zip(out, steam ? "" : "win64_save/");
        z.name = (foreign ? "MHRise Switch save " : steam ? "MHRise save from Steam " : "MHRise Steam save ") + stamp() + ".zip";
        z.message = steam ? "Converted to the Switch's save." + notes : "Converted to a Steam save." + steamNotes(encrypted[0], template);
        return z;
    }

    // ---- backups --------------------------------------------------------------------------------------------------------

    static List<File> backups(File data) {
        List<File> out = new ArrayList<>();
        File[] dirs = backupsDir(data).listFiles(File::isDirectory);
        if (dirs == null) return out;
        Arrays.sort(dirs, (a, b) -> b.getName().compareTo(a.getName()));  // (newest first: the names are dates)
        out.addAll(Arrays.asList(dirs));
        return out;
    }

    // The save as it is now into a folder of its own; null if there's none. Only the last few are kept.
    static String backup(File data) throws IOException {
        File[] files = saveDir(data).listFiles(File::isFile);
        if (files == null || files.length == 0) return null;
        String name = stamp();
        File dir = new File(backupsDir(data), name);
        for (int i = 2; dir.exists(); i++) dir = new File(backupsDir(data), name + " (" + i + ")");
        if (!dir.mkdirs()) throw new IOException("can't write " + dir);
        for (File f : files) Files.copy(f.toPath(), new File(dir, f.getName()).toPath());
        List<File> all = backups(data);
        for (int i = KEEP_BACKUPS; i < all.size(); i++) Drivers.deleteTree(all.get(i));
        return dir.getName();
    }

    static String restore(File data, File backup) throws IOException {
        File[] files = backup.listFiles(File::isFile);
        if (files == null || files.length == 0) throw new IOException("that backup is empty");
        String before = backup(data);
        File dir = saveDir(data);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("can't write " + dir);
        for (File f : files) writeFile(new File(dir, f.getName()), Files.readAllBytes(f.toPath()));
        new File(new File(data, "save.bak"), ".pendiente").delete();
        return "Save from " + backup.getName() + " restored." + (before != null ? " The one before is backed up as " + before + "." : "");
    }

    // What the menu says about the save that's here
    static String describe(File data) {
        File[] files = saveDir(data).listFiles(File::isFile);
        if (files == null || files.length == 0) return "No save here yet.";
        long newest = 0;
        for (File f : files) newest = Math.max(newest, f.lastModified());
        return "Save: " + files.length + " file(s), last written "
                + new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(new Date(newest)) + ".";
    }

    // What an import or restore said, for the next start (the game closes right after it, see GameActivity.saveOver)
    static File pendingMessage(File data) {
        return new File(data, "save_imported.txt");
    }

    // ---- files ----------------------------------------------------------------------------------------------------------

    static void write(ContentResolver cr, Uri uri, byte[] bytes) throws IOException {
        try (OutputStream o = cr.openOutputStream(uri, "wt")) {
            if (o == null) throw new IOException("can't write there");
            o.write(bytes);
        }
    }

    // Written aside and renamed: never half a save file
    private static void writeFile(File f, byte[] bytes) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        try (FileOutputStream o = new FileOutputStream(tmp)) {
            o.write(bytes);
            o.getFD().sync();
        }
        if (!tmp.renameTo(f)) {
            tmp.delete();
            throw new IOException("can't write " + f.getName());
        }
    }

    private static byte[] zip(Map<String, byte[]> files, String folder) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(b)) {
            for (Map.Entry<String, byte[]> f : files.entrySet()) {
                z.putNextEntry(new ZipEntry(folder + f.getKey()));
                z.write(f.getValue());
                z.closeEntry();
            }
        }
        return b.toByteArray();
    }

    private static String stamp() {
        return new SimpleDateFormat("yyyy-MM-dd HH-mm-ss", Locale.ROOT).format(new Date());
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) b.write(buf, 0, n);
        return b.toByteArray();
    }
}
