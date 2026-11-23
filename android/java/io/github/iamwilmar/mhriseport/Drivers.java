package io.github.iamwilmar.mhriseport;

import android.content.Context;
import android.net.Uri;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// The GPU drivers and Lossless.dll on disk, shared by the first-start screen (DriverActivity) and the settings menu's GPU
// driver and Frame generation sections. Each driver is unpacked into files/drivers/<folder>/ (the linker won't load
// libraries from the SD card) and the chosen one is written to files/drivers/elegido, which the game reads at start-up
// (runtime/src/nnsdk/nvn/driver.h). If it can't draw with it, the game goes back to the system's and leaves
// files/drivers/fallo. Lossless.dll goes to files/Lossless.dll (lsfg/dll.cpp looks there first).
final class Drivers {
    private Drivers() {}

    static final class Driver {
        File dir;
        String name, detail;
    }

    static File root(Context c) {
        return new File(c.getFilesDir(), "drivers");
    }

    static File losslessDll(Context c) {
        return new File(c.getFilesDir(), "Lossless.dll");
    }

    static List<Driver> list(Context c) {
        List<Driver> out = new ArrayList<>();
        File[] folders = root(c).listFiles(File::isDirectory);
        if (folders == null) return out;
        Arrays.sort(folders, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        for (File dir : folders) {
            List<String> info = lines(new File(dir, "driver.txt"));
            if (info.size() < 2 || !new File(dir, info.get(1)).isFile()) continue;
            Driver d = new Driver();
            d.dir = dir;
            d.name = info.get(0);
            d.detail = info.size() > 2 ? info.get(2) : "";
            out.add(d);
        }
        return out;
    }

    // The chosen driver's folder name, or null for the system's
    static String chosen(Context c) {
        List<String> lines = lines(new File(root(c), "elegido"));
        if (lines.size() < 2 || !new File(new File(root(c), lines.get(0)), lines.get(1)).isFile()) return null;
        return lines.get(0);
    }

    // null: the system's
    static void choose(Context c, File dir) throws IOException {
        File chosen = new File(root(c), "elegido");
        new File(root(c), "probando").delete();  // (whatever was left from trying another one)
        if (dir == null) {
            chosen.delete();
            return;
        }
        List<String> info = lines(new File(dir, "driver.txt"));
        if (info.size() < 2) throw new IOException("that driver's folder is incomplete");
        try (FileWriter w = new FileWriter(chosen)) {
            w.write(dir.getName() + "\n" + info.get(1) + "\n");
        }
    }

    static void remove(Context c, File dir) throws IOException {
        if (dir.getName().equals(chosen(c))) choose(c, null);
        deleteTree(dir);
    }

    // Why the last driver didn't work (the game wrote it when it went back to the system's), once: null if nothing
    static String takeFailure(Context c) {
        File failed = new File(root(c), "fallo");
        if (!failed.isFile()) return null;
        List<String> l = lines(failed);
        failed.delete();
        return l.isEmpty() ? "" : l.get(0);
    }

    // The zip's meta.json and .so libraries (without their folders) into a new folder; driver.txt keeps what's shown
    // and the library's name. Returns the name shown.
    static String unpack(Context c, Uri zip) throws IOException {
        File dir = new File(root(c), "nuevo");
        deleteTree(dir);
        if (!dir.mkdirs()) throw new IOException("couldn't create the folder");
        JSONObject meta = null;
        List<String> libraries = new ArrayList<>();
        try (InputStream in = c.getContentResolver().openInputStream(zip); ZipInputStream z = new ZipInputStream(in)) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                if (e.isDirectory()) continue;
                String name = e.getName().substring(e.getName().lastIndexOf('/') + 1);
                if (name.equalsIgnoreCase("meta.json")) {
                    try {
                        meta = new JSONObject(new String(readAll(z), "UTF-8"));
                    } catch (JSONException bad) {
                        throw new IOException("meta.json can't be read");
                    }
                } else if (name.endsWith(".so") && !name.contains("..")) {
                    try (OutputStream out = new FileOutputStream(new File(dir, name))) {
                        copy(z, out);
                    }
                    libraries.add(name);
                }
            }
        } catch (IOException e) {
            deleteTree(dir);
            throw e;
        }
        if (libraries.isEmpty()) {
            deleteTree(dir);
            throw new IOException("the zip has no .so library");
        }
        String library = meta != null ? meta.optString("libraryName", "") : "";
        if (library.isEmpty() || !libraries.contains(library)) library = libraries.get(0);
        library = notTheSystems(dir, library);
        String name = meta != null ? meta.optString("name", "").trim() : "";
        String version = meta != null ? meta.optString("driverVersion", "").trim() : "";
        String vendor = meta != null ? meta.optString("vendor", "").trim() : "";
        if (name.isEmpty()) name = library;
        String detail = (vendor + " " + version).trim();

        String folder = name.replaceAll("[^A-Za-z0-9._-]+", "_");
        if (folder.isEmpty() || folder.equals("nuevo")) folder = "driver";
        File target = new File(root(c), folder);
        for (int n = 2; target.exists(); n++) target = new File(root(c), folder + "_" + n);
        try (FileWriter w = new FileWriter(new File(dir, "driver.txt"))) {
            w.write(name + "\n" + library + "\n" + detail + "\n");
        }
        if (!dir.renameTo(target)) {
            deleteTree(dir);
            throw new IOException("couldn't save it");
        }
        return name;
    }

    // The system already has its vulkan.adreno.so open (and the ones in /vendor/lib64/hw): a library of that same name
    // wouldn't get loaded, and the system's would run without a word. It gets a name of its own.
    private static String notTheSystems(File dir, String library) {
        boolean clash = library.equals("vulkan.adreno.so") || library.equals("libvulkan.so")
                || new File("/vendor/lib64/hw/" + library).exists() || new File("/system/lib64/hw/" + library).exists();
        if (!clash) return library;
        String own = "vulkan.propio.so";
        return new File(dir, library).renameTo(new File(dir, own)) ? own : library;
    }

    // The picked file as files/Lossless.dll, after checking it's a Windows DLL (starts with "MZ"). Frame generation is
    // turned on with it. Returns its size, for the message.
    static String importDll(Context c, Uri uri, File gameFolder) throws IOException {
        File tmp = new File(c.getFilesDir(), "Lossless.dll.new");
        long total = 0;
        try (InputStream in = c.getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) throw new IOException("couldn't open the file");
            byte[] buf = new byte[1 << 16];
            boolean first = true;
            for (int n; (n = in.read(buf)) > 0; ) {
                if (first && (n < 2 || buf[0] != 'M' || buf[1] != 'Z')) throw new IOException("that's not a .dll");
                first = false;
                out.write(buf, 0, n);
                total += n;
            }
        } catch (IOException e) {
            tmp.delete();
            throw e;
        }
        if (total < 1 << 20) {
            tmp.delete();
            throw new IOException("too small to be Lossless.dll");
        }
        File dll = losslessDll(c);
        dll.delete();
        if (!tmp.renameTo(dll)) throw new IOException("couldn't save it");
        if (gameFolder != null) new Ini(gameFolder).set("lsfg", "1");  // (on by default once it's there)
        return String.format(java.util.Locale.ROOT, "%.1f MB", total / 1048576.0);
    }

    // ---- files ----------------------------------------------------------------------------------------------------------

    static List<String> lines(File f) {
        try {
            return Files.readAllLines(f.toPath());
        } catch (IOException e) {
            return new ArrayList<>();
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        copy(in, b);
        return b.toByteArray();
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }

    static void deleteTree(File f) {
        File[] subs = f.listFiles();
        if (subs != null)
            for (File s : subs) deleteTree(s);
        f.delete();
    }
}
