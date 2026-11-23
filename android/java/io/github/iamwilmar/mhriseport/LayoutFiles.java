package io.github.iamwilmar.mhriseport;

import android.content.ContentResolver;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

// The touch layout as a file to keep or share (Settings → Touch controls → Export / Import layout): a .zip with
// layout.json (Controls.toJson) and images/<CONTROL>.png, the pictures given to buttons. And the pictures themselves:
// whatever image the player picks is made a PNG of at most 256 pixels a side in the app's own files.
final class LayoutFiles {
    private LayoutFiles() {}

    static final int MAX_SIDE = 256;
    private static final int MAX_ENTRY = 4 << 20;  // (a picture or the json: more than this isn't ours)

    // An image from the system's picker, made small enough for a button
    static Bitmap readImage(ContentResolver cr, Uri uri) throws IOException {
        BitmapFactory.Options probe = new BitmapFactory.Options();
        probe.inJustDecodeBounds = true;
        try (InputStream in = cr.openInputStream(uri)) {
            BitmapFactory.decodeStream(in, null, probe);
        }
        if (probe.outWidth <= 0 || probe.outHeight <= 0) throw new IOException("not an image");
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inSampleSize = 1;
        while (Math.max(probe.outWidth, probe.outHeight) / (o.inSampleSize * 2) >= MAX_SIDE) o.inSampleSize *= 2;
        Bitmap b;
        try (InputStream in = cr.openInputStream(uri)) {
            b = BitmapFactory.decodeStream(in, null, o);
        }
        if (b == null) throw new IOException("not an image");
        int side = Math.max(b.getWidth(), b.getHeight());
        if (side > MAX_SIDE) {
            float k = (float) MAX_SIDE / side;
            Bitmap small = Bitmap.createScaledBitmap(b, Math.max(1, Math.round(b.getWidth() * k)),
                    Math.max(1, Math.round(b.getHeight() * k)), true);
            if (small != b) b.recycle();
            b = small;
        }
        return b;
    }

    static void export(Controls cfg, ContentResolver cr, Uri uri) throws IOException {
        OutputStream os = cr.openOutputStream(uri, "w");
        if (os == null) throw new IOException("can't write there");
        try (ZipOutputStream zip = new ZipOutputStream(os)) {
            zip.putNextEntry(new ZipEntry("layout.json"));
            try {
                zip.write(cfg.toJson().toString(2).getBytes(StandardCharsets.UTF_8));
            } catch (org.json.JSONException e) {
                throw new IOException(e.getMessage());
            }
            zip.closeEntry();
            for (Controls.Ctl c : Controls.Ctl.values()) {
                if (!cfg.image[c.ordinal()]) continue;
                zip.putNextEntry(new ZipEntry("images/" + c.name() + ".png"));
                zip.write(Files.readAllBytes(cfg.imageFile(c).toPath()));
                zip.closeEntry();
            }
        }
    }

    // Replaces the layout with the file's. Nothing changes if the file isn't a layout (the json is read first, the
    // pictures only once it was).
    static void importFrom(Controls cfg, ContentResolver cr, Uri uri) throws IOException {
        String json = null;
        java.util.Map<String, byte[]> pictures = new java.util.HashMap<>();
        InputStream is = cr.openInputStream(uri);
        if (is == null) throw new IOException("can't read it");
        try (ZipInputStream zip = new ZipInputStream(is)) {
            for (ZipEntry e; (e = zip.getNextEntry()) != null;) {
                if (e.isDirectory()) continue;
                String name = e.getName();
                if (name.equals("layout.json")) {
                    json = new String(read(zip), StandardCharsets.UTF_8);
                } else if (name.startsWith("images/") && name.endsWith(".png")) {
                    String ctl = name.substring(7, name.length() - 4);
                    try {
                        Controls.Ctl.valueOf(ctl);  // (only our controls' names: nothing else is written)
                    } catch (IllegalArgumentException unknown) {
                        continue;
                    }
                    byte[] data = read(zip);
                    BitmapFactory.Options o = new BitmapFactory.Options();
                    o.inJustDecodeBounds = true;
                    BitmapFactory.decodeByteArray(data, 0, data.length, o);
                    if (o.outWidth > 0 && o.outHeight > 0) pictures.put(ctl, data);
                }
            }
        }
        if (json == null) throw new IOException("no layout.json in it: not a layout file");
        try {
            cfg.fromJson(new org.json.JSONObject(json));
        } catch (org.json.JSONException e) {
            throw new IOException("not a layout file (" + e.getMessage() + ")");
        }
        File dir = cfg.imagesDir();
        dir.mkdirs();
        for (Controls.Ctl c : Controls.Ctl.values()) cfg.imageFile(c).delete();
        for (java.util.Map.Entry<String, byte[]> p : pictures.entrySet())
            try (FileOutputStream out = new FileOutputStream(new File(dir, p.getKey() + ".png"))) {
                out.write(p.getValue());
            }
        cfg.rescanImages();
    }

    private static byte[] read(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        for (int n; (n = in.read(buf)) > 0;) {
            b.write(buf, 0, n);
            if (b.size() > MAX_ENTRY) throw new IOException("a file in it is too big");
        }
        return b.toByteArray();
    }
}
