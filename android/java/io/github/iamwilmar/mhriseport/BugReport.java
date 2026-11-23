package io.github.iamwilmar.mhriseport;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Locale;
import java.util.function.Consumer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

// Frame captures and the report zip, shared by Debug → Bug report and the floating capture button.
final class BugReport {
    static final String DUMPS = "/storage/emulated/0/NieR-volcados";  // where the runtime saves captured frames
    static final String ZIP = "/storage/emulated/0/Download/mhrise-report.zip";

    private BugReport() {}

    // Clears the last capture and asks the runtime for the next frame (it deletes <data>/trazar once it's done).
    static void capture(File folder) {
        clearDir(new File(DUMPS));
        trigger(folder, "trazar", "completa");
    }

    // One-shot requests the runtime picks up and deletes (a capture, a GPU profile): plain files, not settings.
    static void trigger(File folder, String name, String content) {
        if (folder == null) return;
        try {
            Files.write(new File(folder, name).toPath(), content.getBytes());
        } catch (IOException ignored) {
            // (storage refused: nothing happens)
        }
    }

    // Zips log.txt, the settings and the captured frame on a thread of its own; done gets the line to show the user,
    // on that thread.
    static void save(File folder, Consumer<String> done) {
        new Thread(() -> {
            File out = new File(ZIP);
            String result;
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(out))) {
                for (String name : new String[] {"log.txt", Ini.NAME, "game_settings.txt", "rutas.txt"})
                    if (folder != null) add(zip, new File(folder, name), name);
                File[] dumps = new File(DUMPS).listFiles();
                if (dumps != null)
                    for (File f : dumps)
                        if (f.isFile()) add(zip, f, "frame/" + f.getName());
                result = String.format(Locale.ROOT, "Report saved: Download/mhrise-report.zip (%.1f MB).",
                        out.length() / 1048576.0);
            } catch (IOException e) {
                result = "Couldn't save the report: " + e.getMessage();
            }
            done.accept(result);
        }, "Report").start();
    }

    private static void add(ZipOutputStream zip, File f, String name) throws IOException {
        if (!f.isFile()) return;
        zip.putNextEntry(new ZipEntry(name));
        Files.copy(f.toPath(), zip);
        zip.closeEntry();
    }

    private static void clearDir(File dir) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            if (f.isDirectory()) clearDir(f);
            f.delete();
        }
    }
}
