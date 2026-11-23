package io.github.iamwilmar.mhriseport;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.Settings;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

// The start screen. The first time it asks for the files permission and two folders:
//   - the game folder, where the user copied the game files (ExeFS and RomFS already extracted on their own console, and
//     any DLC). DumpScanner checks what's there; the app only reads it, so it can be anywhere, even read-only.
//   - the data folder, the port's own: saves, the shader cache, log.txt, the settings switches. It can be the game folder
//     itself (how older versions worked) or any other. Where the game files are goes in its rutas.txt (rutas.h).
// files/game_folder.txt and files/carpeta.txt remember them; the game (GameActivity, a NativeActivity: libnier.so) reads
// the data folder from carpeta.txt. Without game_folder.txt (an install from before), the game folder is the data folder.
// After that, straight to the game. The app decrypts nothing and takes no NSP/XCI.
// Looks like the in-game settings menu: dark wine, bronze plates, gold headings. Every page is the same frame: a title
// and a line under it, the page itself (scrolls), a status line and the buttons at the bottom, above the system's bar.
// With a controller: the d-pad moves the focus and A presses (Android turns A into DPAD_CENTER).
public final class SetupActivity extends Activity {

    private static final String GAME_FILE = "game_folder.txt", DATA_FILE = "carpeta.txt";

    private static final int WINE = 0xFF26120E, PANEL = 0xFF1A0C09, ROW = 0xFF2C1614, LIT = 0xFF4A2A18;
    private static final int BRONZE = 0xFF3E2616, EDGE = SettingsMenu.EDGE, GOLD = SettingsMenu.GOLD;
    private static final int CREAM = SettingsMenu.CREAM, DIM = 0xFFB8A888, GOOD = 0xFF9CD38A, ALERT = 0xFFFF9678;

    private File browsing;      // the browser's folder; null: the list of storages
    private boolean forData;    // what the browser is choosing
    private boolean inBrowser;
    private boolean shown, waitingAccess;
    private DumpScanner.Result found;  // the game files of the game folder, once scanned

    private LinearLayout body, buttons;
    private TextView status;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!Environment.isExternalStorageManager()) {
            showPermission();
            shown = false;
            return;
        }
        if (shown) return;  // coming back from the background doesn't change the screen
        shown = true;
        File data = saved(DATA_FILE);
        DumpScanner.Result saved = data != null ? DumpScanner.load(data) : null;
        if (saved != null) {
            found = saved;
            // Both folders chosen and the files where they were: straight to the game, past this screen. It shows again
            // if something is missing (below), if the game's settings panel asked for it ("Change folder" leaves the file
            // elegir_carpeta in the data folder) or if the app is opened twice within 10 seconds, the way out when the
            // game doesn't get as far as starting.
            File ask = new File(data, "elegir_carpeta");
            File stamp = new File(getFilesDir(), "ultimo_arranque.txt");
            long last = 0;
            try {
                last = Long.parseLong(new String(Files.readAllBytes(stamp.toPath())).trim());
            } catch (IOException | NumberFormatException e) {
                // no earlier start
            }
            long now = System.currentTimeMillis();
            boolean again = now >= last && now - last < 10000;
            if (saved.ready() && !ask.exists() && !again) {
                try {
                    Files.write(stamp.toPath(), Long.toString(now).getBytes());
                } catch (IOException e) {
                    // without the stamp only the "twice in a row" way out is lost
                }
                startGame();
                return;
            }
            ask.delete();
            stamp.delete();
            showResult();
            return;
        }
        File game = gameFolder();
        if (game != null && game.isDirectory()) scan(game);  // something moved since last time
        else if (waitingAccess) showBrowser(null, false);  // (back from the permission screen with it on)
        else showWelcome();
        waitingAccess = false;
    }

    @Override
    @SuppressWarnings("deprecation")  // still delivered with targetSdk 33; a controller sends BACK with B
    public void onBackPressed() {
        if (inBrowser && browsing != null) showBrowser(parentOf(browsing), forData);
        else if (inBrowser && found != null && found.ready()) showResult();
        else super.onBackPressed();
    }

    // ---- the pages ------------------------------------------------------------------------------------------------------

    private void showPermission() {
        inBrowser = false;
        page("GAME FILES", "To read your game files the app needs access to all files. It only reads the game folder "
                + "and writes to the data folder you choose.");
        gameGuide();
        Button grant = action("Give permission", true, v -> {
            waitingAccess = true;
            say("Turn on access to all files and come back.", GOLD);
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (ActivityNotFoundException e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        });
        action("Exit", false, v -> finish());
        grant.requestFocus();
    }

    private void showWelcome() {
        inBrowser = false;
        page("GAME FILES", "Choose the folder that holds your copy of the game. It's read from there: nothing is copied.");
        gameGuide();
        Button pick = action("Choose game folder", true, v -> showBrowser(null, false));
        action("GPU driver", false, v -> startActivity(new Intent(this, DriverActivity.class)));
        action("Exit", false, v -> finish());
        pick.requestFocus();
    }

    // What the game folder has to look like, and what each part is
    private void gameGuide() {
        LinearLayout cols = columns();
        LinearLayout left = panel(cols, "THE FOLDER SHOULD LOOK LIKE THIS");
        left.addView(tree(new String[] {
            "Your game folder",
            "├─ exefs",
            "│    main, main.npdm",
            "│    rtld, sdk, subsdk0",
            "├─ romfs",
            "│    resource/",
            "└─ DLC (optional)",
            "     <title ID> romfs",
        }, 0, 1, 4, 7));
        LinearLayout right = column(cols, "WHAT EACH ONE MUST BE");
        right.addView(notes("• exefs: the one from the " + Game.VERSION + " update.\n"
                + "• romfs: the base game's, with the " + Game.VERSION + " update applied over it.\n"
                + (Game.DLC_MARKER != null
                        ? "• DLC: each one's romfs, in a folder named with its title ID, as your extraction tool made it.\n" : "")
                + "• Folder names can differ: the app looks for them anywhere inside the folder you pick.\n"
                + "• Your saves don't go here: you choose a data folder next."));
    }

    // ---- the folder browser ---------------------------------------------------------------------------------------------

    private void showBrowser(File dir, boolean data) {
        inBrowser = true;
        browsing = dir;
        forData = data;
        if (data) {
            page("DATA FOLDER", "Where the port keeps your saves, the shader cache, log.txt and its settings. "
                    + "Any folder you like.");
        } else {
            page("CHOOSE YOUR GAME FOLDER", "Open the folder with your game files and choose \"Use this folder\".");
        }
        LinearLayout where = panel(body, null);
        TextView path = text(dir == null ? "Storage" : dir.getPath(), 14, GOLD, true);
        path.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        where.addView(path);
        if (dir == null) {
            for (StorageVolume volume : getSystemService(StorageManager.class).getStorageVolumes()) {
                File d = volume.getDirectory();
                if (d != null) item(where, "💾   " + volume.getDescription(this), v -> showBrowser(d, data));
            }
        } else {
            item(where, "⬆   Up", v -> showBrowser(parentOf(dir), data));
            File[] subs = dir.listFiles(File::isDirectory);
            int n = 0;
            if (subs != null) {
                Arrays.sort(subs, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
                for (File sub : subs) {
                    if (sub.getName().startsWith(".")) continue;
                    item(where, "📁   " + sub.getName(), v -> showBrowser(sub, data));
                    n++;
                }
            }
            if (n == 0) where.addView(notes("(no folders in here)"));
        }
        Button first = null;
        if (dir != null) {
            first = action("Use this folder", true, v -> {
                if (data) useDataFolder(dir);
                else scan(dir);
            });
            if (data) action("New folder here", false, v -> {
                File fresh = new File(dir, "God Eater 3 Recomp");
                for (int k = 2; fresh.exists() && !fresh.isDirectory(); k++) fresh = new File(dir, "MH Rise port " + k);
                if (fresh.isDirectory() || fresh.mkdirs()) showBrowser(fresh, true);
                else say("Can't make a folder in " + dir.getPath() + ".", ALERT);
            });
        }
        boolean back = found != null && found.ready();
        action(back ? "Cancel" : "Exit", false, v -> {
            if (back) showResult();
            else finish();
        });
        if (where.getChildCount() > 1) where.getChildAt(1).requestFocus();
        else if (first != null) first.requestFocus();
    }

    private void showBusy(String title, String what) {
        inBrowser = false;
        page(title, what);
        LinearLayout p = panel(body, null);
        p.addView(new android.widget.ProgressBar(this));
    }

    // ---- the game folder ------------------------------------------------------------------------------------------------

    private void scan(File dir) {
        showBusy("LOOKING FOR YOUR GAME…", dir.getPath());
        new Thread(() -> {
            DumpScanner.Result r = DumpScanner.scan(dir);
            String error = null;
            File data = saved(DATA_FILE);
            if (r.ready()) {
                try {
                    write(GAME_FILE, dir);
                    // (an install from before kept everything in the game folder: that stays its data folder)
                    if (data != null && data.isDirectory()) DumpScanner.writePaths(data, r);
                } catch (IOException e) {
                    error = "Can't write to " + data + ": " + e.getMessage();
                }
            }
            String failure = error;
            runOnUiThread(() -> {
                found = r;
                if (r.ready() && failure == null && (data == null || !data.isDirectory())) {
                    askDataFolder(dir);
                    return;
                }
                showResult();
                if (failure != null) say(failure, ALERT);
            });
        }, "FindGame").start();
    }

    // The first time: the game files are fine, now where the port's own files go
    private void askDataFolder(File game) {
        inBrowser = false;
        page("SAVES AND SETTINGS", "Game found. Now choose where the port keeps its own files.");
        LinearLayout cols = columns();
        LinearLayout left = panel(cols, "THE DATA FOLDER WILL HAVE");
        left.addView(tree(new String[] {
            "Your data folder",
            "├─ save          your hunts",
            "├─ save.bak      the last good save",
            "├─ shaders       the shader cache",
            "├─ log.txt",
            "└─ settings      (from the in-game menu)",
        }, 0));
        LinearLayout right = column(cols, "WHY ITS OWN FOLDER");
        right.addView(notes("• The game files stay untouched, and can even be on a card you only read from.\n"
                + "• Back up or move your saves without the 10+ GB of game files.\n"
                + "• The game folder still works if you'd rather keep everything together."));
        say("✓  " + game.getPath(), GOOD);
        Button other = action("Choose data folder", true, v -> showBrowser(game.getParentFile(), true));
        action("Use the game folder", false, v -> useDataFolder(game));
        other.requestFocus();
    }

    // ---- the data folder ------------------------------------------------------------------------------------------------

    private void useDataFolder(File dir) {
        File old = saved(DATA_FILE);
        if (old != null && (sameFolder(old, dir) || !old.isDirectory())) old = null;
        showBusy(old != null ? "MOVING YOUR SAVES…" : "SETTING UP…", dir.getPath());
        final File from = old;
        final DumpScanner.Result r = found;
        new Thread(() -> {
            String error = null, note = null;
            try {
                if (r == null || !r.ready()) throw new IOException("the game folder isn't set up yet");
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("can't make the folder");
                if (from != null) note = copyPortFiles(from, dir, r);
                DumpScanner.writePaths(dir, r);
                write(DATA_FILE, dir);
            } catch (IOException e) {
                error = "Can't use " + dir + ": " + e.getMessage();
            }
            String failure = error, done = note;
            runOnUiThread(() -> {
                showResult();
                if (failure != null) say(failure, ALERT);
                else if (done != null) say(done, GOLD);
            });
        }, "DataFolder").start();
    }

    // The old data folder's contents, copied (not moved) to the new one: everything except the game files themselves.
    // A folder that already has saves keeps them: nothing is copied over them.
    private static String copyPortFiles(File from, File to, DumpScanner.Result r) throws IOException {
        if (new File(to, "save").isDirectory())
            return "This folder already has saves, so those are used. Nothing was copied from " + from.getPath() + ".";
        List<String> game = new ArrayList<>();
        game.add(r.exefs);
        game.add(r.romfs);
        game.addAll(r.dlc.values());
        File[] items = from.listFiles();
        if (items == null) return null;
        int copied = 0;
        for (File item : items) {
            String name = item.getName();
            if (name.equals("rutas.txt") || name.equals("rutas.txt.tmp") || name.equals("elegir_carpeta")) continue;
            if (holdsGame(item, game)) continue;
            copy(item, new File(to, name));
            copied++;
        }
        return copied == 0 ? null
                : "Your saves and settings were copied here. The old copy is still in " + from.getPath()
                        + ": delete it once the game runs fine.";
    }

    // Is this a game folder, or one with game files somewhere inside?
    private static boolean holdsGame(File item, List<String> game) {
        String path = canonical(item);
        for (String g : game) {
            if (g == null) continue;
            String gp = canonical(new File(g));
            if (gp.equals(path) || gp.startsWith(path + File.separator)) return true;
        }
        return false;
    }

    private static void copy(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("can't make " + to);
            File[] items = from.listFiles();
            if (items == null) return;
            for (File item : items) copy(item, new File(to, item.getName()));
        } else {
            Files.copy(from.toPath(), to.toPath(), StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.COPY_ATTRIBUTES);
        }
    }

    // ---- what's chosen --------------------------------------------------------------------------------------------------

    private void showResult() {
        inBrowser = false;
        browsing = null;
        DumpScanner.Result r = found != null ? found : new DumpScanner.Result();
        File game = gameFolder(), data = saved(DATA_FILE);
        boolean dataOk = data != null && data.isDirectory();
        boolean ready = r.ready() && dataOk;
        page(Game.NAME.toUpperCase(java.util.Locale.ROOT), ready ? "Everything's in place." : "Something's missing: see below.");
        LinearLayout cols = columns();

        LinearLayout left = panel(cols, "GAME FOLDER");
        left.addView(pathLine(game != null ? game.getPath() : "none"));
        left.addView(check(r.exefs != null, r.exefs != null ? "Game, version " + Game.VERSION
                : r.exefsProblem != null ? r.exefsProblem : "ExeFS not found (the folder with main and main.npdm)."));
        left.addView(check(r.romfs != null, r.romfs != null ? "Game RomFS"
                : r.romfsProblem != null ? r.romfsProblem : "RomFS not found."));
        if (Game.DLC_MARKER != null) {
            left.addView(r.dlc.isEmpty() ? line("·  No DLC (optional)", DIM) : check(true, r.dlc.size() + " DLC"));
            if (r.unknownDlc > 0)
                left.addView(line("·  " + r.unknownDlc + " DLC skipped: keep their folders named with their title ID, "
                        + "as your extraction tool made them.", DIM));
        }

        LinearLayout right = panel(cols, "DATA FOLDER");
        right.addView(pathLine(dataOk ? data.getPath() : "not chosen yet"));
        if (dataOk) {
            boolean same = game != null && sameFolder(game, data);
            right.addView(check(true, same ? "Saves, cache and log next to the game files" : "Saves, cache and log"));
            right.addView(line(new File(data, "save").isDirectory() ? "·  Has saves" : "·  No saves yet", DIM));
        } else {
            right.addView(check(false, "Choose where your saves go."));
        }

        Button first = null;
        if (ready) first = action("Start game", true, v -> startGame());
        Button changeGame = action(r.ready() ? "Game folder" : "Choose game folder", !r.ready(),
                v -> showBrowser(game != null && game.isDirectory() ? game : null, false));
        if (first == null && !r.ready()) first = changeGame;
        if (r.ready()) {
            Button changeData = action(dataOk ? "Data folder" : "Choose data folder", !dataOk,
                    v -> showBrowser(dataOk ? data : null, true));
            if (first == null) first = changeData;
        }
        action("GPU driver", false, v -> startActivity(new Intent(this, DriverActivity.class)));
        first.requestFocus();
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        overridePendingTransition(0, 0);
        finish();
    }

    // ---- files/ ---------------------------------------------------------------------------------------------------------

    private File saved(String name) {
        try {
            String path = new String(Files.readAllBytes(new File(getFilesDir(), name).toPath())).trim();
            return path.isEmpty() ? null : new File(path);
        } catch (IOException e) {
            return null;
        }
    }

    private File gameFolder() {
        File game = saved(GAME_FILE);
        return game != null ? game : saved(DATA_FILE);
    }

    private void write(String name, File dir) throws IOException {
        try (FileWriter w = new FileWriter(new File(getFilesDir(), name))) {
            w.write(dir.getPath() + "\n");
        }
    }

    private static String canonical(File f) {
        try {
            return f.getCanonicalPath();
        } catch (IOException e) {
            return f.getAbsolutePath();
        }
    }

    private static boolean sameFolder(File a, File b) {
        return canonical(a).equals(canonical(b));
    }

    // Up one folder; from a storage's root, back to the list of storages.
    private File parentOf(File dir) {
        if (dir == null) return null;
        for (StorageVolume volume : getSystemService(StorageManager.class).getStorageVolumes())
            if (dir.equals(volume.getDirectory())) return null;
        return dir.getParentFile();
    }

    // ---- the frame ------------------------------------------------------------------------------------------------------

    // A new page: title, the line under it, an empty body, the status line and an empty row of buttons
    private void page(String title, String intro) {
        int pad = dp(14);
        LinearLayout whole = new LinearLayout(this);
        whole.setOrientation(LinearLayout.VERTICAL);
        whole.setBackgroundColor(WINE);

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.VERTICAL);
        top.setPadding(pad * 2, pad, pad * 2, dp(8));
        TextView t = text(title, 20, GOLD, true);
        t.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        t.setGravity(Gravity.CENTER);
        top.addView(t);
        TextView i = text(intro, 14, CREAM, false);
        i.setGravity(Gravity.CENTER);
        i.setPadding(0, dp(4), 0, 0);
        top.addView(i);
        View rule = new View(this);
        rule.setBackgroundColor(EDGE);
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1);
        rlp.topMargin = dp(10);
        top.addView(rule, rlp);
        whole.addView(top);

        ScrollView scroll = new ScrollView(this);
        body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(pad * 2, 0, pad * 2, pad);
        scroll.addView(body);
        whole.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        status = text("", 14, GOLD, false);
        status.setGravity(Gravity.CENTER);
        status.setPadding(pad * 2, dp(6), pad * 2, dp(6));
        status.setVisibility(View.GONE);
        whole.addView(status);

        buttons = new LinearLayout(this);
        buttons.setGravity(Gravity.CENTER);
        buttons.setPadding(pad, 0, pad, dp(10));
        whole.addView(buttons);

        whole.setOnApplyWindowInsetsListener((v, insets) -> {  // (the buttons stay above the system's bar)
            v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                    insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(whole);
    }

    private void say(String s, int color) {
        status.setText(s);
        status.setTextColor(color);
        status.setVisibility(View.VISIBLE);
    }

    // A button at the bottom; the main one in gold
    private Button action(String label, boolean main, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(main ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        if (main) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(states(BRONZE, LIT, main ? 2 : 1, 6));
        b.setPadding(dp(18), 0, dp(18), 0);
        b.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(46));
        lp.setMargins(dp(6), 0, dp(6), 0);
        buttons.addView(b, lp);
        return b;
    }

    // Two columns side by side in the body (a phone is in landscape here)
    private LinearLayout columns() {
        LinearLayout cols = new LinearLayout(this);
        cols.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        body.addView(cols, lp);
        return cols;
    }

    // A dark plate with a gold heading (none: just the plate), in the body or in a column
    private LinearLayout panel(LinearLayout parent, String heading) {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setBackground(plate(PANEL, 2, 6));
        p.setPadding(dp(14), dp(10), dp(14), dp(10));
        if (heading != null) p.addView(text(heading, 13, GOLD, true));
        LinearLayout.LayoutParams lp = parent == body
                ? new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
                : new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1);
        if (parent == body) lp.topMargin = dp(10);
        else if (parent.getChildCount() > 0) lp.leftMargin = dp(12);
        parent.addView(p, lp);
        return p;
    }

    // A column without a plate (the notes next to a panel)
    private LinearLayout column(LinearLayout parent, String heading) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(18), dp(10), 0, 0);
        c.addView(text(heading, 13, GOLD, true));
        parent.addView(c, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return c;
    }

    // A folder in the browser: a row that lights up under the focus or the finger
    private void item(LinearLayout parent, String label, View.OnClickListener click) {
        TextView t = text(label, 15, CREAM, false);
        t.setPadding(dp(12), dp(10), dp(12), dp(10));
        t.setBackground(states(ROW, LIT, 1, 3));
        t.setFocusable(true);
        t.setClickable(true);
        t.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(4);
        parent.addView(t, lp);
    }

    // A folder tree: folders (the lines given) in gold, what's in them in cream
    private TextView tree(String[] lines, int... folders) {
        SpannableStringBuilder b = new SpannableStringBuilder();
        for (int i = 0; i < lines.length; i++) {
            int at = b.length();
            b.append(lines[i]).append(i + 1 < lines.length ? "\n" : "");
            boolean folder = false;
            for (int f : folders) folder |= f == i;
            b.setSpan(new ForegroundColorSpan(folder ? GOLD : CREAM), at, b.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        TextView t = new TextView(this);
        t.setText(b);
        t.setTypeface(Typeface.MONOSPACE);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        t.setPadding(0, dp(6), 0, 0);
        return t;
    }

    private TextView notes(String s) {
        TextView t = text(s, 14, CREAM, false);
        t.setPadding(0, dp(6), 0, 0);
        t.setLineSpacing(dp(2), 1);
        return t;
    }

    private TextView pathLine(String s) {
        TextView t = text(s, 13, DIM, false);
        t.setTypeface(Typeface.MONOSPACE);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }

    private TextView check(boolean ok, String s) {
        return line((ok ? "✓  " : "✗  ") + s, ok ? GOOD : ALERT);
    }

    private TextView line(String s, int color) {
        TextView t = text(s, 14, color, false);
        t.setPadding(0, dp(4), 0, 0);
        return t;
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private GradientDrawable plate(int fill, int strokeDp, int radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(strokeDp), EDGE);
        d.setCornerRadius(dp(radiusDp));
        return d;
    }

    // Plain, and lit with a gold edge while focused (a controller) or pressed
    private StateListDrawable states(int fill, int lit, int strokeDp, int radiusDp) {
        StateListDrawable s = new StateListDrawable();
        GradientDrawable on = plate(lit, strokeDp + 1, radiusDp);
        on.setStroke(dp(strokeDp + 1), GOLD);
        s.addState(new int[] {android.R.attr.state_pressed}, on);
        s.addState(new int[] {android.R.attr.state_focused}, on);
        s.addState(new int[0], plate(fill, strokeDp, radiusDp));
        return s;
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }
}
