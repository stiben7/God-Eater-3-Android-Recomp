package io.github.iamwilmar.mhriseport;

import android.app.Dialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.IntConsumer;
import java.util.function.IntSupplier;


// Settings menu drawn over the game, styled after the game's own menus: dark wine panels with a bronze border, gold
// titles over the red and green rule, and a "?" on each row that expands its help text. Opened from the gear button
// of the touch controls. Sections on the left, the selected section's rows on the right.
// Control settings are saved and applied right away (GameActivity.refresh). Display, Graphics, Performance and Debug
// are the game's switches: empty files in the game folder (runtime/src/nnsdk/nvn/flags.h) that the game checks once a
// second. The ones only read at start-up say so in their help.
final class SettingsMenu {
    static final int PANEL = 0xF0201010, ROW = 0xFF2C1614, EDGE = 0xFFC49646, GOLD = 0xFFE2B65C, CREAM = 0xFFF0E4C8,
            DIM = 0xFFB09C84, RED = 0xFFB0221E, GREEN = 0xFF508E36;

    private static int section = 0;  // last one opened (for the life of the process)

    private final GameActivity activity;
    private final Controls cfg;
    private final Dialog dialog;
    private final LinearLayout list, sections;
    private final ScrollView scroll;
    private final TextView sub;
    private final File folder;  // the data folder (files/carpeta.txt), where mhr.ini lives
    private final Ini ini;
    private final float scale;  // menu size (Display > Menu size): text and spacing, 0.8 by default
    static final String PREFS = "menu", SCALE_KEY = "escala";
    static final int SCALE_DEFAULT = 80;

    SettingsMenu(GameActivity activity, Controls cfg) {
        this.activity = activity;
        this.cfg = cfg;
        this.scale = menuScale(activity) / 100f;
        dialog = new Dialog(activity);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);

        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackground(plate(PANEL, 2, 0));

        // top bar: "SETTINGS ◆ <section>" and the X, with the red and green rule under it
        LinearLayout top = new LinearLayout(activity);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(18), dp(10), dp(10), dp(10));
        TextView title = text("SETTINGS", 22, GOLD, true);
        title.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        sub = text("", 17, CREAM, false);
        TextView close = text("✕", 22, CREAM, false);
        close.setPadding(dp(14), dp(4), dp(14), dp(4));
        close.setBackground(plate(0xFF1A0C09, 1, 2));
        close.setOnClickListener(v -> dialog.dismiss());
        top.addView(title);
        top.addView(sub, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        top.addView(close);
        root.addView(top);
        root.addView(rule(RED, 2));
        root.addView(rule(GREEN, 1));

        folder = gameFolder(activity);
        ini = new Ini(folder);
        gpu = GpuInfo.read(folder);
        LinearLayout body = new LinearLayout(activity);
        sections = new LinearLayout(activity);
        sections.setOrientation(LinearLayout.VERTICAL);
        sections.setPadding(dp(10), dp(8), dp(6), dp(10));
        ScrollView side = new ScrollView(activity);
        side.addView(sections);
        body.addView(side, new LinearLayout.LayoutParams(dp(176), ViewGroup.LayoutParams.MATCH_PARENT));
        View divider = new View(activity);
        divider.setBackgroundColor(EDGE);
        body.addView(divider, new LinearLayout.LayoutParams(dp(1), ViewGroup.LayoutParams.MATCH_PARENT));
        list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(12), dp(6), dp(12), dp(12));
        scroll = new ScrollView(activity);
        scroll.addView(list);
        body.addView(scroll, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
        root.addView(body, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        build();

        dialog.setContentView(root);
        Window w = dialog.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = activity.getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(940), (int) (m.widthPixels * 0.94f)), (int) (m.heightPixels * 0.92f));
        }
    }

    void show() { dialog.show(); }

    private void changed() { activity.refresh(); }

    // ---- sections -------------------------------------------------------------------------------------------------------

    private String[] sectionNames() {
        return new String[] {"Display", "Graphics", "GPU driver", "Frame generation", "Performance",
                "Touch controls", "Combos", "Controller",
                "Multiplayer", "Saves", "Mods & cheats", "Debug"};
    }

    private void build() {
        String[] names = sectionNames();
        sections.removeAllViews();
        for (int i = 0; i < names.length; i++) {
            final int k = i;
            boolean on = k == section;
            TextView b = text(names[k], 15, on ? GOLD : CREAM, on);
            b.setPadding(dp(12), dp(11), dp(8), dp(11));
            b.setBackground(plate(on ? 0xFF3E2616 : ROW, on ? 2 : 1, 2));
            b.setOnClickListener(v -> {
                section = k;
                build();
                scroll.scrollTo(0, 0);
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(4);
            sections.addView(b, lp);
        }
        sub.setText("  ◆  " + names[section]);
        list.removeAllViews();
        switch (section) {
            case 0: buildDisplay(); break;
            case 1: buildGraphics(); break;
            case 2: buildDriver(); break;
            case 3: buildFrameGen(); break;
            case 4: buildPerformance(); break;
            case 5: buildTouch(); break;
            case 6: buildCombos(); break;
            case 7: buildController(); break;
            case 8: buildMultiplayer(); break;
            case 9: buildSaves(); break;
            case 10: buildMods(); break;
            default: buildDebug(); break;
        }
    }

    private void buildDisplay() {
        header("Menu");
        number("Menu size", "The size of this menu's text and buttons.", 60, 130, 5, " %", () -> menuScale(activity), v -> {
                    try {
                        activity.getSharedPreferences(PREFS, android.content.Context.MODE_PRIVATE).edit().putInt(SCALE_KEY, v).apply();
                    } catch (RuntimeException ignored) {
                        // (no preferences: keep the current size)
                    }
                    dialog.dismiss();
                    new SettingsMenu(activity, cfg).show();  // (sizes are fixed at build time, so reopen with the new one)
                });
        action(resetArmed ? "Tap again to restore the defaults" : "Restore default settings",
                "Puts every setting of this menu back as it comes: graphics, performance, debug and the driver "
                        + "variables. Your touch layout, combos and button mapping stay. Some settings need a restart.",
                () -> {
                    if (!resetArmed) {
                        resetArmed = true;
                        build();
                        return;
                    }
                    resetArmed = false;
                    ini.reset(SWITCHES);
                    build();
                });
        header("Resolution");
        note("The game's 3D resolution. On the handheld console it's 960x540 inside a 720p picture, which is then scaled "
                        + "to the screen. Higher: sharper, and more work for the GPU.");
        exclusive("Base resolution",
                "540p is the game's own. Lower: blurrier, much less GPU work, for weak phones. 720p draws the whole scene "
                        + "at 1280x720, with no scaling inside the game. In docked mode the same steps apply to the "
                        + "game's docked resolution. Applies at once.",
                new String[] {"540p (960x540)", "240p (427x240)", "360p (640x360)", "432p (768x432)", "480p (854x480)", "576p (1024x576)",
                        "648p (1152x648)", "720p (1280x720)"},
                new String[] {null, "resolution_240", "resolution_360", "resolution_432", "resolution_480", "resolution_576",
                        "resolution_648", "resolution_720"});
        exclusive("Aspect ratio",
                "The shape of the picture on your screen, like Eden's setting. 16:9 is the game's own, with black bars at "
                        + "the sides on a long phone. The others stretch the game's picture to that shape; Stretch fills the "
                        + "whole screen. Applies at once.",
                new String[] {"Default (16:9)", "Force 4:3", "Force 21:9", "Force 16:10", "Stretch to screen"},
                new String[] {null, "aspect_4_3", "aspect_21_9", "aspect_16_10", "aspect_stretch"});
        flag("Expand into the cutout",
                "Use the strip beside the camera cutout too, as in Eden and Azahar. Off: Android keeps the picture clear "
                        + "of it. With a wider aspect ratio or Stretch, On shows the picture right to the edge. Takes "
                        + "effect on the next start.",
                "cutout", false);
        flag("Show Android's bars",
                "Keep the status and navigation bars on screen while playing. Off: they're hidden, and a swipe in from "
                        + "the edge brings them back for a moment. Takes effect the next time you come back to the game.",
                "system_bars", false);
        toggle("Draw controls with the GPU",
                "The touch controls drawn by the GPU, as Android usually does. Off: drawn by the CPU, which keeps them "
                        + "away from the phone's OpenGL driver; Mali's crashed there while hunting. Off by default on MediaTek "
                        + "and Exynos. Takes effect the next time you start the game.",
                () -> GameActivity.touchOnGpu(ini) ? 1 : 0, v -> ini.set("touch_gpu", v != 0 ? "1" : "0"));
        flag("Docked mode",
                "The game thinks it's on the dock: its TV picture (1080p output, about twice the pixels of handheld) and "
                        + "your controls as player 1's Pro Controller. Much more work for the GPU: for strong phones. Takes "
                        + "effect on the next start.",
                "dock_mode", false);
        flag("Dynamic resolution",
                "If the GPU can't finish a frame in time, it lowers the resolution in steps (down to 73 % of the base) "
                        + "and raises it again when there's room. Off: always the base.",
                "no_dynamic_resolution", true);
        flag("FSR upscaling",
                "AMD FSR 1 takes the 720p picture to the screen sharper than a plain scale. Off: linear scaling, a "
                        + "little cheaper.",
                "no_fsr", true);

        header("Picture");
        final String[] caps = {null, "fps_20", "fps_25", "fps_40", "fps_45", "60fps"};
        choice("FPS limit",
                "The game measures how long each frame takes, so between 20 and 60 fps it runs at its normal speed: "
                        + "fewer fps save GPU, heat and battery with no slow motion. 30 is the game's own (loading screens "
                        + "always ask for 60). 40 is even on 120 Hz screens; on a 60 Hz one, 40 and 45 judder a little. "
                        + "Applies at once.",
                new String[] {"30 (the game's)", "20", "25", "40", "45", "60"}, () -> {
                    for (int i = caps.length - 1; i > 0; i--)
                        if (has(caps[i])) return i;
                    return 0;
                }, v -> {
                    for (int i = 1; i < caps.length; i++) put(caps[i], i == v);
                });
        flag("FPS counter", "The number in the corner.", "no_fps",
                true);

        header("Performance overlay");
        note("More lines under the FPS counter, each on its own. With any of them on, the counter becomes a small box "
                + "that also shows how many of the fps are real when frame generation is on. All apply at once.");
        flag("Frame time", "How long the game's frames take (ms), and the 1 % low: the fps of its slowest frames, "
                + "which is what you feel as stutter.", "no_hud_frametime", true);
        flag("CPU usage", "How much of the phone's CPU the game uses, all cores together.", "no_hud_cpu", true);
        flag("GPU usage", "How busy the GPU is. Only where the phone lets apps read it (many Adreno phones do; most "
                + "others show N/A).", "no_hud_gpu", true);
        flag("Memory", "The game's own RAM, then the phone's used / total.", "no_hud_ram", true);
        flag("Heat", "How close the phone is to slowing itself down from heat (Android's own measure, Android 12 and "
                + "later): at 100 % it throttles hard. Yellow from 80 %, red from 95 %.", "no_hud_heat", true);
        flag("Shaders building", "Pipelines being made in the background right now. Above 0, a new effect or area is "
                + "being prepared: short hitches then are that.", "no_hud_shaders", true);
    }

    private void buildGraphics() {
        header("Effects");
        flag("Shadows",
                "Those of the hunters, monsters and scenery. Off, the shadow maps aren't drawn: less work for a weak "
                        + "GPU. Applies at once.",
                "no_shadows", true);
        note(gameStatus());
        exclusive("Shadow quality",
                "The size of the shadow maps, as a share of the game's own. Lower: blurrier shadow edges and a lot less "
                        + "GPU work in one of the heaviest passes. Applies at once.",
                new String[] {"Game's", "Half", "Quarter"}, new String[] {null, "shadow_half", "shadow_quarter"});
        flag("Dynamic shadows",
                "Shadows of things that move (hunters, monsters, swaying plants), like the PC's option. Off: only the "
                        + "scenery's shadows, a lot less shadow work. Applies at once.",
                "no_dynamic_shadows", true);
        flag("Foliage sway",
                "Grass and leaves moving in the wind, like the PC's option. Off: they stand still, a bit less vertex "
                        + "work. Applies at once.",
                "no_foliage_sway", true);
        exclusive("Effects resolution (experimental)",
                "The size of the buffer the game draws particles, smoke and other see-through effects into, as steps "
                        + "below the game's own (which is already reduced). Lower: blurrier effects, less GPU work in "
                        + "big fights. Applies at once.",
                new String[] {"Game's", "One step lower", "Two steps lower"},
                new String[] {null, "effects_lower", "effects_lowest"});
        flag("Ambient occlusion (experimental)",
                "The soft darkening where things meet (the game's geometry AO). Off: flatter look, less GPU work. "
                        + "Applies at once.",
                "no_ao", true);
        flag("Fast depth test",
                "The colour pass skips hidden pixels earlier (it uses the depth test the GPU can reject fast with), "
                        + "which can give a few more FPS, mostly on Adreno. Grass and leaves keep the game's own test so "
                        + "they don't turn into solid cards. Applies at once.",
                "fast_depth", false);
        header("Detail");
        flag("Occlusion results",
                "Lets the game see which small things its GPU test found visible, so endemic life, wirebugs and some "
                        + "lights get drawn. Off: they stay invisible (you can still gather them). Costs almost nothing. "
                        + "Applies at once.",
                "no_readback_copies", true);
        if (gpu.noBc()) {
            flag("Textures as ASTC (Mali)",
                    "On GPUs that can't read the game's BC textures (Mali), keeps them compressed as ASTC instead of 4-8 "
                            + "times bigger: less memory traffic every frame. The first time a texture loads it's encoded on the "
                            + "CPU (slower loading and some stutter), then it comes from a cache in the app's storage. Restart the "
                            + "game to apply.",
                    "bc_astc", false);
            action("Clear the ASTC cache",
                    "Deletes the encoded textures (" + astcCacheSize() + ") from the app's storage. They're encoded again as "
                            + "they load. Uninstalling the app deletes them too.",
                    () -> {
                        clearAstcCache();
                        build();
                    });
        }
        exclusive("Model detail",
                "How soon models switch to their simpler versions with distance (the PC version's mesh quality). Lower: "
                        + "less work for the GPU, simpler models a little closer. Applies at once.",
                new String[] {"Game's", "Low", "Lowest", "Minimum", "High"},
                new String[] {null, "model_detail_low", "model_detail_lowest", "model_detail_min", "model_detail_high"});
        exclusive("Texture streaming",
                "How many of the largest texture sizes the game skips loading. Fewer: blurrier textures up close, less "
                        + "memory and bandwidth. Applies as textures load.",
                new String[] {"Game's", "One size smaller", "Two sizes smaller"},
                new String[] {null, "texture_bias_1", "texture_bias_2"});
        header("Textures");
        {
            final String[] files = {null, "bindless_arrays", "no_bindless_arrays"};
            choice("Texture arrays (bindless)",
                    "How shaders read many of the game's textures. Auto: off with Qualcomm's driver on an Adreno 7xx "
                            + "or 8xx, which can't compile many of those shaders (things come out black); on elsewhere. "
                            + "Turn them off if your driver draws things black. Takes effect on the next start.",
                    new String[] {"Auto", "On", "Off"}, () -> {
                        for (int i = files.length - 1; i > 0; i--)
                            if (has(files[i])) return i;
                        return 0;
                    }, v -> {
                        for (int i = 1; i < files.length; i++) put(files[i], i == v);
                    });
        }
        flag("Anisotropic filtering",
                "Sharper floors and walls at an angle in the distance. Off: a little cheaper for a weak GPU.",
                "no_af", true);
        if (gpu.eds || gpu.vids) {  // (only where the driver has them; known after the game's first start)
            header("Extensions");
            if (gpu.eds)
                flag("Extended dynamic state",
                        "Cull mode, depth test and stencil set per draw instead of built into each pipeline: far fewer "
                                + "pipelines to build, so fewer hitches when new effects or areas show up. Off by default: "
                                + "some drivers have bugs with it. Takes effect on the next start.",
                        "dynamic_state", false);
            if (gpu.vids)
                flag("Vertex input dynamic state",
                        "The vertex layout set per draw too: fewer pipelines again (a mesh with another layout shares "
                                + "one). Off by default, for the same reason. Takes effect on the next start.",
                        "vertex_input_state", false);
        }
    }

    // ---- GPU driver and frame generation (Drivers: the same files as the first-start screen, DriverActivity) ----------

    private static String sessionDriver;  // the driver the game started with (it can't change until the next start)
    private static boolean sessionDriverRead;

    private void buildDriver() {
        if (!sessionDriverRead) {
            sessionDriverRead = true;
            sessionDriver = Drivers.chosen(activity);
        }
        String failed = Drivers.takeFailure(activity);
        if (failed != null) note("The last driver didn't work (" + failed + "): the system's is in use.");
        header("Driver");
        note("The phone's own driver, or one you add from a .zip (meta.json and the .so, as for AdrenoTools: Turnip and "
                + "the like). Adreno (Snapdragon) only, at your own risk. A change takes effect the next time the game "
                + "starts; if the game can't draw with the new one, it goes back to the system's by itself.");
        List<Drivers.Driver> drivers = Drivers.list(activity);
        String chosen = Drivers.chosen(activity);
        String[] names = new String[drivers.size() + 1];
        names[0] = "System driver";
        int current = 0;
        for (int i = 0; i < drivers.size(); i++) {
            Drivers.Driver d = drivers.get(i);
            names[i + 1] = d.name + (d.detail.isEmpty() ? "" : " (" + d.detail + ")");
            if (d.dir.getName().equals(chosen)) current = i + 1;
        }
        pick("Driver in use", "The one the game uses from its next start.", names, current, v -> {
            try {
                Drivers.choose(activity, v == 0 ? null : drivers.get(v - 1).dir);
            } catch (IOException e) {
                android.widget.Toast.makeText(activity, "Couldn't save it: " + e.getMessage(), android.widget.Toast.LENGTH_LONG)
                        .show();
            }
        });
        action("Add a driver (.zip)", "Pick a driver's zip. It's added to the list; choose it above to use it.", () -> {
            dialog.dismiss();
            activity.importDriver();
        });
        if (!drivers.isEmpty()) {
            String[] own = Arrays.copyOfRange(names, 1, names.length);
            action("Remove a driver", "Deletes an added driver. If it's the one in use, the system's is used instead.",
                    () -> dropDown("Remove a driver", own, -1, i -> dropDown("Delete " + drivers.get(i).name + "?",
                            new String[] {"Keep it", "Delete it"}, -1, k -> {
                                if (k == 1) {
                                    try {
                                        Drivers.remove(activity, drivers.get(i).dir);
                                    } catch (IOException ignored) {
                                        // (left as it was)
                                    }
                                }
                                build();
                            })));
        }
        boolean changed = chosen == null ? sessionDriver != null : !chosen.equals(sessionDriver);
        if (changed) {
            note("The game keeps the driver it started with. Close it and open the app again to use the new one.");
            action("Close the game", "Closes the app now, so the next start uses the chosen driver.", () -> {
                activity.finishAffinity();
                android.os.Process.killProcess(android.os.Process.myPid());
            });
        }
    }

    private void buildFrameGen() {
        if (folder == null) {
            note("Pick the game folder first.");
            return;
        }
        File dll = Drivers.losslessDll(activity);
        File builtin = new File(folder, "fg_builtin");
        boolean own = !dll.isFile() || builtin.exists();
        header("Frame generation");
        note("Shows about twice the game's fps (30 → 60) by making a picture between each two of the game's. With your own "
                + "Lossless.dll (from Lossless Scaling on Steam, Windows: in the program's folder; not included with the "
                + "app) it can use Lossless Scaling's; otherwise the port's own.");
        toggle("Frame generation", "On or off while you play (the same as the in-game panel's row).",
                () -> has("lsfg") ? 1 : 0, v -> ini.set("lsfg", v != 0 ? "1" : "0"));
        if (dll.isFile()) {
            String[] kinds = {"Lossless Scaling", "Built-in"};
            String size = String.format(java.util.Locale.ROOT, "%.1f MB", dll.length() / 1048576.0);
            pick("Generator", "Lossless Scaling: sharper, heavier on the GPU (Lossless.dll, " + size + "). Built-in: lighter, "
                    + "runs on any GPU. Takes effect the next time the game starts.", kinds, own ? 1 : 0, k -> {
                if (k == 0) Drivers.deleteTree(builtin);
                else builtin.mkdirs();
            });
        }
        exclusive("Target",
                "Off: one generated picture between each two real ones (30 → 60). With a target, as many as it takes to "
                        + "reach it, like Lossless Scaling's adaptive mode: at 20 real fps two (60), on a 120 Hz screen up "
                        + "to three. Never above the screen's refresh rate, and each extra picture costs about as much GPU "
                        + "as the first, so on a busy GPU it can slow the real frames down. The built-in generator does "
                        + "this more cheaply than Lossless Scaling's. Applies at once.",
                new String[] {"Off (2x)", "60 fps", "90 fps", "120 fps"},
                new String[] {null, "fg_target_60", "fg_target_90", "fg_target_120"});
        File half = new File(folder, "lsfg_flow_50"), full = new File(folder, "lsfg_flow_100");
        int level = full.exists() ? 2 : half.exists() ? 1 : 0;
        String[] presets = own ? new String[] {"Light (motion at 1/4)", "Balanced (motion at 1/3)", "Sharp (motion at 1/2)"}
                               : new String[] {"Light (flow 25 %)", "Balanced (flow 50 %)", "Sharp (flow 100 %)"};
        pick("Quality Preset", "How finely " + (own ? "the generator" : "Lossless Scaling") + " follows motion. Higher is "
                + "sharper with less smearing, and heavier on the GPU (each step about twice the work). Applies at once.",
                presets, level, k -> {
                    Drivers.deleteTree(half);
                    Drivers.deleteTree(full);
                    if (k == 1) half.mkdirs();
                    if (k == 2) full.mkdirs();
                });
        action(dll.isFile() ? "Replace Lossless.dll" : "Import Lossless.dll",
                "Pick Lossless.dll from your copy of Lossless Scaling. Takes effect the next time the game starts.", () -> {
                    dialog.dismiss();
                    activity.importLosslessDll();
                });
        if (dll.isFile())
            action("Remove Lossless.dll", "Deletes the imported copy; frame generation goes on with the built-in one.", () -> {
                dll.delete();
                build();
            });
    }

    // A row whose value opens a drop-down, whatever the number of options (choice() flips two on a tap)
    private void pick(String title, String help, String[] options, int current, IntConsumer set) {
        String shown = options[current];
        int paren = shown.indexOf(" (");
        row(title, (paren > 0 ? shown.substring(0, paren) : shown) + "  ▾", help, x -> dropDown(title, options, current, k -> {
            set.accept(k);
            changed();
            build();
        }), false);
    }

    private void buildPerformance() {
        header("GPU work");
        flag("Skip empty GPU barriers",
                "About half of the GPU's full stops in a frame come right after another one with nothing in between, and "
                        + "still make the GPU wait and flush. On: those are skipped (less waiting, a little less heat). Off "
                        + "by default: if graphics glitch with it on, turn it off. Applies at once.",
                "tight_barriers", false);
        exclusive("Pipeline worker threads",
                "Background threads that build the GPU pipelines for new effects and areas (like Eden's setting). More "
                        + "build them sooner, so fewer objects pop in late, on a phone with cores to spare; 1 keeps the heat "
                        + "and battery down. Takes effect on the next start.",
                new String[] {"2 (default)", "1", "3", "4"},
                new String[] {null, "pipeline_threads_1", "pipeline_threads_3", "pipeline_threads_4"});
        flag("Asynchronous presentation",
                "Puts each finished frame on screen from a thread of its own (like Eden's setting), so the renderer starts "
                        + "the next frame instead of waiting for the screen. Can give a few fps back on a busy phone. Off by "
                        + "default: if the picture stutters or tears with it, turn it off. Applies at once.",
                "async_present", false);
        flag("GPU texture unswizzle",
                "Switch textures are stored in a tiled order, and each one is put back in plain rows before the GPU gets "
                        + "it. On: the GPU does that instead of the CPU, which then only copies each texture as it is, so "
                        + "loading an area or a monster stutters less (like Eden's option). Off by default until it's been "
                        + "tried on more phones. Applies at once.",
                "gpu_unswizzle", false);
        if (gpu.adreno()) {
            header("GPU");
            flag("GPU boost (Adreno)",
                    "Keeps the Adreno at its top clock while the game is open (libadrenotools' turbo, no root needed), "
                            + "so it doesn't slow down between frames. Hotter and more battery; the phone still throttles "
                            + "when it gets too hot. Turned off when you leave the game. Applies at once.",
                    "adreno_turbo", false);
        }
        header("CPU");
        flag("Performance hints",
                "Tells Android how long each frame should take and how much work it took, so it raises the clocks in "
                        + "time instead of letting them drop on every wait (Android 13 or newer). Usually a steadier pace.",
                "no_adpf", true);
        flag("Game threads on the fast cores",
                "The three threads that build each frame go on the fast cores (if the phone has at least three). Off: "
                        + "Android places them.",
                "no_affinity", true);
        header("Memory and shaders");
        flag("Low memory",
                "Smaller caches, as on a phone with less than 5 GB (it's already like that there). Takes effect on the "
                        + "next start.",
                "low_memory", false);
        flag("Shader warm-up",
                "At start it builds the pipelines seen in earlier runs, so they don't drop frames when they appear. "
                        + "Takes effect on the next start.",
                "no_warmup", true);
        flag("Async shaders",
                "A shader the game hasn't used before is translated in the background, and what needs it shows a moment "
                        + "late the first time. Off: the game stops until it's translated (a short freeze).",
                "no_async_shaders", true);
        flag("Pipelines on one thread",
                "No background threads or warm-up: more stutter, but some drivers crash building several at once "
                        + "(PowerVR is like that by itself). Takes effect on the next start.",
                "single_thread_pipelines", false);
    }

    // Local Play over the internet (runtime/src/nnsdk/ldn.cpp): hubs created in the game's Local Play are listed on the
    // hub servers, and the game's hub search shows theirs. Saved as plain text files in the game folder: hub_servers (one
    // per line, "#" in front for one switched off, "off" for none), hub_key, hub_hosts and hub_port. Read when a hub is
    // created or searched for, so changes apply without a restart. Not the MHGU hub servers: their owners asked for
    // MH Rise hubs to stay off them.
    private static final String[] DEFAULT_SERVERS = {"meoli.viewdns.net"};
    private static final String RETIRED_SERVER = "morerokk-mhgurecomp-ldnserver.duckdns.org";

    private List<String> lines(String name) {
        List<String> out = new ArrayList<>();
        if (folder == null) return out;
        try {
            for (String l : Files.readAllLines(new File(folder, name).toPath())) {
                l = l.trim();
                if (!l.isEmpty()) out.add(l);
            }
        } catch (IOException ignored) {
            // (not there yet)
        }
        return out;
    }

    private void writeLines(String name, List<String> list) {
        if (folder == null) return;
        try {
            Files.write(new File(folder, name).toPath(), (String.join("\n", list) + "\n").getBytes());
        } catch (IOException ignored) {
            // (storage refused: the row shows the old value)
        }
    }

    private List<String> servers() {
        List<String> l = lines("hub_servers");
        if (l.isEmpty()) return new ArrayList<>(Arrays.asList(DEFAULT_SERVERS));
        if (l.size() == 1 && l.get(0).equals("off")) return new ArrayList<>();
        l.removeIf(x -> x.replace("#", "").trim().startsWith(RETIRED_SERVER));  // (an older list saved with it)
        return l;
    }

    private void ask(String title, String guide, String initial, int max, java.util.function.Consumer<String> done) {
        new TextPrompt(activity).show(title, guide, initial, "", 0, max, false, false, text -> {
            if (text != null) done.accept(text.trim());
            build();
        });
    }

    private void buildMultiplayer() {
        note("Local Play over the internet: hubs you create in the game's Local Play are listed on the hub servers below, "
                + "and searching for hubs shows the ones there. Only MH Rise players are listed (not MHGU's). No port "
                + "forwarding needed.");
        header("Matchmaking");
        List<String> key = lines("hub_key");
        row("Matchmaking key", key.isEmpty() ? "None" : "••••",
                "Players who enter the same key only see each other's hubs. Empty: public, seen by everyone without a key. "
                        + "Only a hash of it is sent.",
                v -> ask("Matchmaking key", "Empty for public hubs", key.isEmpty() ? "" : key.get(0), 64, text -> {
                    if (text.isEmpty()) new File(folder, "hub_key").delete();
                    else writeLines("hub_key", Arrays.asList(text));
                }), false);

        header("Hub servers");
        List<String> list = servers();
        if (list.isEmpty()) note("No hub servers: only the search hosts below.");
        else note("Tap a server to switch it on or off, or to remove it. Hubs are listed on, and searched for in, every "
                + "server that's on.");
        for (int i = 0; i < list.size(); i++) {
            final int k = i;
            final boolean on = !list.get(i).startsWith("#");
            final String name = on ? list.get(i) : list.get(i).substring(1).trim();
            row(name, (on ? "On" : "Off") + "  ▾", "On: hubs are listed and searched for here. Off: kept in the list, "
                    + "not used.", v -> dropDown(name, new String[] {"On", "Off", "Remove"}, on ? 0 : 1, pick -> {
                        List<String> l = servers();
                        if (k >= l.size()) return;
                        if (pick == 2) l.remove(k);
                        else l.set(k, pick == 0 ? name : "#" + name);
                        writeLines("hub_servers", l.isEmpty() ? Arrays.asList("off") : l);
                        build();
                    }), false);
        }
        action("Add hub server", "A hub server's address: name or IP, and :port if it isn't 39510.",
                () -> ask("Add hub server", "host or host:port", "", 128, text -> {
                    if (text.isEmpty() || text.startsWith("#")) return;
                    List<String> l = servers();
                    if (!l.contains(text) && !l.contains("#" + text)) l.add(text);
                    writeLines("hub_servers", l);
                }));
        action("Restore the default server", "Back to the list it comes with: " + String.join(", ", DEFAULT_SERVERS) + ".",
                () -> {
                    new File(folder, "hub_servers").delete();
                    build();
                });

        header("Direct connections");
        List<String> hosts = lines("hub_hosts");
        for (int i = 0; i < hosts.size(); i++) {
            final int k = i;
            row(hosts.get(i), "Remove", "Tap to remove this host.", v -> {
                List<String> l = lines("hub_hosts");
                if (k < l.size()) l.remove(k);
                writeLines("hub_hosts", l);
                build();
            }, false);
        }
        action("Add search host", "Another phone's address (IP, or IP:port) to look for its hub directly, on the same "
                + "Wi-Fi or with its port forwarded.", () -> ask("Add search host", "IP or IP:port", "", 128, text -> {
                    if (text.isEmpty()) return;
                    List<String> l = lines("hub_hosts");
                    if (!l.contains(text)) l.add(text);
                    writeLines("hub_hosts", l);
                }));
        List<String> port = lines("hub_port");
        String portNow = port.isEmpty() ? "39510" : port.get(0);
        row("Port", portNow.equals("0") ? "Off" : portNow,
                "The TCP port a hub you create listens on for direct connections (0: off). Hub servers don't need it.",
                v -> ask("Port", "1-65535, or 0 for off", portNow, 5, text -> {
                    try {
                        int p = Integer.parseInt(text);
                        if (p >= 0 && p <= 65535) writeLines("hub_port", Arrays.asList(Integer.toString(p)));
                    } catch (NumberFormatException ignored) {
                        // (not a number: unchanged)
                    }
                }), false);
    }

    // Switches for testing drivers and collecting logs. Same files as always (see the options table in the README), just
    // with a name here. Most of them are only read at start-up.
    private void buildDebug() {
        String next = " Takes effect on the next start.";
        note("For testing drivers and sending logs. If the game looks right, leave all of this as it is.");
        header("Log");
        flag("Detailed log",
                "Writes much more to log.txt (every draw's state, textures…). Slower: only for a report.",
                "verbose_log", false);
        flag("GPU breadcrumbs",
                "Logs which pass the GPU stops at when the driver drops work (black screen). Slower." + next,
                "breadcrumbs", false);
        action("Profile one GPU frame",
                "Writes how long each pass of one frame takes to the log (\"perfil GPU\" lines). Use it where it's slow.",
                () -> {
                    trigger("profile_gpu", "");
                    dialog.dismiss();
                });
        flag("Performance stats",
                "Every 10 seconds, where the port's CPU time goes (\"perfil\" lines in the log), without the rest of "
                        + "the detailed log. Turn it on, play a minute, and send the report.",
                "perf_stats", false);
        header("Bug report");
        note("For testers: stand where the problem shows, tap Capture this frame, wait a few seconds, then Save the "
                + "report and send the zip from your Download folder." + (reportStatus != null ? "\n" + reportStatus : ""));
        action("Capture this frame",
                "Saves the next frame's pictures (every render target, depth and shadow maps) and a trace of its draws "
                        + "to the log. The game stutters for a moment. Old captures are cleared first.",
                () -> {
                    BugReport.capture(folder);
                    reportStatus = "Frame captured. Wait a few seconds before saving the report.";
                    dialog.dismiss();
                });
        action("Save the report",
                "Puts log.txt, your settings and the captured frame into Download/mhrise-report.zip, to send.",
                this::saveReport);
        flag("Capture button",
                "A small round button over the game, so a frame can be captured right where the problem shows without "
                        + "opening this menu. Tap it to capture, hold it to save the report, drag it out of the way.",
                CaptureButton.SWITCH, false);
        header("Shaders");
        flag("Remove unused shader functions",
                "Strips shader code that's never called before the driver sees it. Qualcomm's driver (512.863) "
                        + "rejected the character pipelines because of it (they came out black)." + next,
                "keep_dead_functions", true);
        toggle("Integer vertex attributes",
                "Integer vertex data (bone indices…) reaches the shader as integers, as on the console. Without "
                        + "it, characters stretch into spikes on Mali GPUs. Off by default on Adreno, which doesn't need "
                        + "it (with Turnip it made characters flash)." + next,
                () -> (gpu.adreno() ? has("int_attribs") : !has("no_int_attribs")) ? 1 : 0, v -> {
                    put("int_attribs", gpu.adreno() && v != 0);
                    put("no_int_attribs", !gpu.adreno() && v == 0);
                });
        flag("spirv-opt on switch shaders",
                "Runs shaders that contain a switch (BRX) through the optimizer; some drivers miscompile them." + next,
                "no_spirv_opt_brx", true);
        flag("Retry rejected pipelines",
                "When the driver rejects a pipeline, its shaders are translated again with safer options.",
                "no_safe_translation", true);
        exclusive("GPU occlusion culling",
                "The game hides what its GPU test says is behind something else. Auto: off on Mali, where that test's "
                        + "compute shaders don't run right on 16-lane subgroups and grass and trees popped in and out; on "
                        + "elsewhere. Off: the game culls by the camera alone, a bit more to draw. Applies at once.",
                new String[] {"Auto", "On", "Off"}, new String[] {null, "gpu_occlusion", "no_gpu_occlusion"});
        if (gpu.mali())
            exclusive("Exact draw counts (old drivers)",
                    "On drivers without draw-indirect-count (ARM's on a Dimensity 720), the vertex shader drops the "
                            + "game's GPU-counted draws past their count, which removes the stretched triangles. Auto: on "
                            + "where the driver lacks draw-indirect-count, unused elsewhere." + next,
                    new String[] {"Auto", "On", "Off"}, new String[] {null, "count_check", "no_count_check"});
        if (gpu.mali())
            exclusive("Subgroups under 32",
                    "Subgroup operations on GPUs whose subgroups are under 32 (Mali: 16). Auto: on with PanVK and with "
                            + "ARM's driver on GPUs without draw-indirect-count (Dimensity 720), where the ground goes "
                            + "missing without them; off with newer ARM drivers, where they stalled the GPU. Quad swaps for the game's derivatives stay on "
                            + "either way." + next,
                    new String[] {"Auto", "On", "Off"}, new String[] {null, "short_subgroups", "no_short_subgroups"});
        header("Driver workarounds");
        if (gpu.adreno()) {
            note("Render pass shortcuts: for flicker that only shows without Turnip's system memory rendering. Turn "
                    + "on one switch at a time to find which one it is. Each costs a little speed. Applies at once.");
            flag("No clears in the pass",
                    "A clear the game asks for is done on its own, not folded into the start of the next render pass.",
                    "no_deferred_clear", false);
            flag("Save every image",
                    "Everything a render pass draws is written back, even what the port thinks isn't read again.",
                    "no_store_ops", false);
            flag("Full render area",
                    "Each render pass covers the whole image, not just the part that's drawn.",
                    "no_area", false);
            flag("Qualcomm: return after a switch",
                    "On Qualcomm's driver, a return on a switch's impossible path instead of \"unreachable\" (without "
                            + "it the hunter's face wasn't drawn)." + next,
                    "no_return_qualcomm", true);
            flag("Qualcomm: emulated shuffle",
                    "On Qualcomm's driver, shuffle with a dynamic index is emulated, as in Eden." + next,
                    "no_shuffle_qualcomm", true);
        }
        if (gpu.mali()) {
            flag("Mali: return after a switch",
                    "The same on Mali GPUs (ARM's driver and PanVK)." + next,
                    "no_return_mali", true);
            flag("Mali: safe buffer reads",
                    "A read past the end of a buffer gives 0 instead of a GPU fault, which on Mali throws away the "
                            + "frame's work (black or magenta screens that fix themselves when you leave and come back). "
                            + "Costs very little." + next,
                    "no_robust", true);
        }

        header("Driver environment");
        note("Settings for the GPU driver itself, as NAME=value, set before it starts (on the next start). Mesa's drivers "
                + "read them: Turnip (TU_DEBUG, FD_DEV_FEATURES) and PanVK (PANVK_DEBUG). The phone's own drivers "
                + "ignore them.");
        if (turnip(activity)) {  // (Turnip's own variables, only while a Turnip driver is the one chosen)
            envToken("Turnip: no LRZ", "TU_DEBUG=nolrz. Turns off Turnip's low-resolution depth test, which culls whole "
                    + "blocks of the screen. Try it if blocks of the picture flash black.", "TU_DEBUG", "nolrz");
            envToken("Turnip: system memory rendering", "TU_DEBUG=sysmem. Draws straight to memory instead of in the "
                    + "GPU's on-chip tiles. Slower; for flicker or flashing on new Adreno GPUs.", "TU_DEBUG", "sysmem");
            envToken("Turnip: no UBWC", "TU_DEBUG=noubwc. No compression on any image.", "TU_DEBUG", "noubwc");
            envToken("Turnip: UBWC flag hint", "FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1. Reported to stop screen "
                    + "flicker with some Adreno 8xx Turnip builds.", "FD_DEV_FEATURES", "enable_tp_ubwc_flag_hint=1");
        } else {
            note("Turnip's switches show here while a Turnip driver is chosen (Graphics → GPU driver).");
        }
        List<String> env = ini.env();
        for (int i = 0; i < env.size(); i++) {
            final int k = i;
            row(env.get(i), "Remove", "Tap to remove this variable.", v -> {
                List<String> l = ini.env();
                if (k < l.size()) l.remove(k);
                ini.setEnv(l);
                build();
            }, false);
        }
        action("Add variable", "Any driver variable, as NAME=value.",
                () -> ask("Add variable", "NAME=value", "", 256, text -> {
                    int eq = text.indexOf('=');
                    if (eq <= 0) return;
                    setEnv(text.substring(0, eq).trim(), text.substring(eq + 1).trim());
                }));
    }

    // ---- driver environment ("env.NAME" lines of mhr.ini, read by vk.cpp at start-up) -----------------------------------

    // Is the driver chosen in DriverActivity a Turnip (Mesa's freedreno library, or a folder named for it)?
    static boolean turnip(android.content.Context c) {
        try {
            List<String> l = Files.readAllLines(new File(new File(c.getFilesDir(), "drivers"), "elegido").toPath());
            if (l.size() < 2) return false;
            String folder = l.get(0).toLowerCase(java.util.Locale.ROOT), library = l.get(1).toLowerCase(java.util.Locale.ROOT);
            return library.contains("freedreno") || folder.contains("turnip");
        } catch (IOException | RuntimeException e) {
            return false;  // (the system's driver)
        }
    }

    private String envValue(String name) {
        for (String l : ini.env())
            if (l.startsWith(name + "=")) return l.substring(name.length() + 1);
        return null;
    }

    private void setEnv(String name, String value) {
        List<String> l = ini.env();
        l.removeIf(x -> x.startsWith(name + "="));
        if (value != null && !value.isEmpty()) l.add(name + "=" + value);
        ini.setEnv(l);
    }

    // One option of a comma-separated variable (TU_DEBUG=nolrz,sysmem), or a whole NAME=value, as a toggle
    private void envToken(String title, String help, String name, String token) {
        final boolean list = !token.contains("=");
        toggle(title, help, () -> {
            String v = envValue(name);
            if (v == null) return 0;
            return (list ? Arrays.asList(v.split(",")).contains(token) : v.equals(token)) ? 1 : 0;
        }, on -> {
            String v = envValue(name);
            if (!list) {
                setEnv(name, on != 0 ? token : null);
            } else {
                List<String> parts = new ArrayList<>();
                if (v != null) for (String p : v.split(",")) if (!p.isEmpty() && !p.equals(token)) parts.add(p);
                if (on != 0) parts.add(token);
                setEnv(name, String.join(",", parts));
            }
            build();
        });
    }

    private void buildTouch() {
        header("Touch controls");
        toggle("On-screen controls",
                "The buttons and sticks over the game. Off, only the gear stays, to come back here.",
                () -> cfg.touchOn ? 1 : 0, v -> cfg.touchOn = v != 0);
        number("Opacity", "How much of the game shows through the controls.", 20, 100, 5, " %", () -> cfg.opacity, v -> cfg.opacity = v);
        number("Size", "All controls at once. Each one also has its own, in Edit layout.", 60, 160, 5, " %", () -> cfg.size,
                v -> cfg.size = v);
        choice("Vibration", "A short tap when a control is pressed.",
                new String[] {"Off", "Light", "Medium", "Strong"},
                () -> cfg.vibration, v -> {
                    cfg.vibration = v;
                    activity.tick();
                });
        toggle("Slide between buttons",
                "A finger moving from one button to another lets go of the first and presses the second, without lifting.",
                () -> cfg.slide ? 1 : 0, v -> cfg.slide = v != 0);
        toggle("Camera swipe",
                "Dragging a finger on the game, away from the controls, turns the camera, as in a phone game. A tap still "
                        + "goes to the game's touch screen. If you'd rather use the right stick, show it below and turn "
                        + "this off.",
                () -> cfg.cameraSwipe ? 1 : 0, v -> {
                    cfg.cameraSwipe = v != 0;
                    build();  // (the camera warning at the end of the page)
                });
        number("Swipe speed", "How far the camera turns for the same drag.", 25, 300, 25, " %", () -> cfg.swipeSpeed,
                v -> cfg.swipeSpeed = v);
        number("Stick dead zone",
                "How far a touch stick moves from its centre before it counts.", 0, 30, 1, " %",
                () -> cfg.touchDeadzone, v -> cfg.touchDeadzone = v);
        toggle("Tap a stick: L3 / R3",
                "A short tap on a stick, without moving it, presses it (L3 left, R3 right). Their own buttons are there too.",
                () -> cfg.stickPress ? 1 : 0, v -> cfg.stickPress = v != 0);
        action("Edit layout",
                "Drag each control where you want it. At the top: its size, hide or show it, and back to the original.",
                () -> {
                    dialog.dismiss();
                    activity.startEditing();
                });
        action("Export layout", "Saves the whole touch layout to a file: where each control is, its look and pictures, "
                + "and the combos with their controller buttons. To keep it, or give it to someone.", () -> {
                    dialog.dismiss();
                    activity.exportLayout();
                });
        action("Import layout", "Replaces the touch layout with one from an exported file (from this phone or another: "
                + "places follow the screen).", () -> {
                    dialog.dismiss();
                    activity.importLayout();
                });
        action("Reset layout",
                "The traditional layout, every control in its place and at its size.",
                () -> {
                    cfg.resetLayout();
                    changed();
                    build();
                });

        // (showing and hiding each control is in Edit layout: Hide / Show on the chosen one)
        if (!cfg.shown[Controls.Ctl.RSTICK.ordinal()] && !cfg.cameraSwipe)
            note("The right stick is hidden and camera swipe is off: nothing on the screen turns the camera. Show the stick "
                    + "in Edit layout, or turn camera swipe on.");
    }

    private void buildCombos() {
        header("Combos");
        note("A combo presses several buttons at once: with its button on the screen, or with one (or two) of the controller's. "
                + "The layout editor's Add and Copy as a new button make combos too, so every added button can be fired "
                + "from a controller.");
        for (int i = 0; i < Controls.COMBOS; i++) {
            final int k = i;
            String value = cfg.combo[k] == 0 ? "None"
                                             : Controls.names(cfg.combo[k]) + (cfg.comboPad[k] != 0
                                                       ? "  ·  pad: " + Controls.names(cfg.comboPad[k]) : "")
                                                       + (cfg.comboKey[k] != 0 ? "  ·  key: " + Controls.keyName(cfg.comboKey[k]) : "");
            row(cfg.comboName(k), value, "Choose the buttons it presses and, if you like, the controller button that does it.", v -> editCombo(k), false);
        }

    }

    private void buildController() {
        header("Controller");
        number("Dead zone", "How far a controller stick moves before it counts (both sticks).", 0, 40, 1, " %",
                () -> cfg.padDeadzone, v -> cfg.padDeadzone = v);
        number("Analog triggers",
                "How far an analog trigger travels before it presses ZL / ZR.", 10, 90, 5, " %",
                () -> cfg.trigger, v -> cfg.trigger = v);
        choice("Right stick",
                "Which axes the right stick is read from. By controller: Z/RZ on Android and Xbox pads, RX/RY (with the "
                        + "triggers on Z/RZ) on many generic ones. Change it if the camera won't move or moves on its own.",
                new String[] {"By controller", "Z / RZ", "RX / RY"}, () -> cfg.rightAxes,
                v -> cfg.rightAxes = v);

        header("Button mapping");
        note("Tap a Switch button, then press the controller button you want for it. Everything else keeps its usual "
                + "place. To swap two buttons, map each one to the other.");
        for (int i = 0; i < Controls.BUTTONS.length; i++) {
            final int b = i;
            String value = cfg.mapKey[i] == 0 ? "Default" : Controls.keyName(cfg.mapKey[i]);
            row(MAP_NAMES[i], value, "The controller button that presses " + MAP_NAMES[i] + " in the game.",
                    v -> captureKey(b), false);
        }
        row("Reset mapping", "", "Every controller button back to its usual place.", v -> {
            Arrays.fill(cfg.mapKey, 0);
            changed();
            build();
        }, false);

        header("Added buttons and combos");
        note("Any key of a controller or a keyboard can fire one of your added buttons or combos (Combos, and the layout "
                + "editor's Add): while it's held the game gets all of its buttons. Tap one, then press the key.");
        boolean any = false;
        for (int i = 0; i < Controls.COMBOS; i++) {
            if (cfg.combo[i] == 0) continue;
            any = true;
            final int k = i;
            final String name = cfg.comboName(k);
            row(name + "  (" + Controls.names(cfg.combo[k]) + ")", cfg.comboKey[k] == 0 ? "None" : Controls.keyName(cfg.comboKey[k]),
                    "The controller or keyboard key that presses " + Controls.names(cfg.combo[k]) + ".",
                    v -> captureKey(name, cfg.comboKey[k], key -> {
                        for (int j = 0; j < Controls.COMBOS; j++)
                            if (cfg.comboKey[j] == key) cfg.comboKey[j] = 0;  // (one key, one of them)
                        cfg.comboKey[k] = key;
                    }), false);
        }
        if (!any) note("None yet: make one in Combos, or with Add in Edit layout.");
    }

    private static final String[] MAP_NAMES = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+ (Plus)", "− (Minus)",
        "L3 (left stick press)", "R3 (right stick press)", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
        "Left stick up", "Left stick down", "Left stick left", "Left stick right", "Right stick up", "Right stick down",
        "Right stick left", "Right stick right"};

    // ---- button mapping -------------------------------------------------------------------------------------------------

    // Waits for one controller button (or an analog trigger, or a hat direction) and maps it to Controls.BUTTONS[b]
    private void captureKey(int b) {
        captureKey(MAP_NAMES[b], cfg.mapKey[b], key -> {
            for (int i = 0; i < cfg.mapKey.length; i++)
                if (cfg.mapKey[i] == key) cfg.mapKey[i] = 0;  // (one controller button, one Switch button)
            cfg.mapKey[b] = key;
        });
    }

    // The same for anything a key is mapped to: set gets the key pressed, or 0 for Default (none)
    private void captureKey(String what, int current, IntConsumer set) {
        final Dialog d = new Dialog(activity);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(12), dp(16), dp(14));
        box.setBackground(plate(PANEL, 2, 0));
        TextView title = text("Map " + what, 20, GOLD, true);
        title.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        box.addView(title);
        box.addView(rule(RED, 2));
        box.addView(rule(GREEN, 1));
        TextView ask = text("Press the controller button or keyboard key for " + what + ".", 16, CREAM, false);
        ask.setPadding(0, dp(12), 0, dp(4));
        box.addView(ask);
        box.addView(text(current == 0 ? "Now: none of its own" : "Now: " + Controls.keyName(current), 14, DIM, false));

        final boolean[] done = {false};
        final IntConsumer take = key -> {
            if (done[0]) return;
            done[0] = true;
            set.accept(key);
            d.dismiss();
            changed();
            build();
        };
        d.setOnKeyListener((dialog, keyCode, ev) -> {
            // (a controller's, or a physical keyboard's: not the on-screen one's)
            boolean pad = (ev.getSource() & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK
                    | InputDevice.SOURCE_DPAD | InputDevice.SOURCE_KEYBOARD)) != 0 && ev.getDevice() != null
                    && !ev.getDevice().isVirtual();
            if (keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) return false;
            if (!pad) return keyCode != KeyEvent.KEYCODE_BACK;  // (the back gesture still closes it)
            if (ev.getAction() == KeyEvent.ACTION_UP && ev.getRepeatCount() == 0) take.accept(keyCode);
            return true;  // (on release, so the press doesn't also reach the menu)
        });
        // Analog triggers and hats send motion, not keys: they're mapped as the keys they stand for
        box.setFocusable(true);
        box.setFocusableInTouchMode(true);
        box.setOnGenericMotionListener((v, ev) -> {
            if ((ev.getSource() & InputDevice.SOURCE_JOYSTICK) == 0) return false;
            float lt = Math.max(ev.getAxisValue(MotionEvent.AXIS_LTRIGGER), ev.getAxisValue(MotionEvent.AXIS_BRAKE));
            float rt = Math.max(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER), ev.getAxisValue(MotionEvent.AXIS_GAS));
            float hx = ev.getAxisValue(MotionEvent.AXIS_HAT_X), hy = ev.getAxisValue(MotionEvent.AXIS_HAT_Y);
            if (lt > 0.6f) take.accept(KeyEvent.KEYCODE_BUTTON_L2);
            else if (rt > 0.6f) take.accept(KeyEvent.KEYCODE_BUTTON_R2);
            else if (hy < -0.5f) take.accept(KeyEvent.KEYCODE_DPAD_UP);
            else if (hy > 0.5f) take.accept(KeyEvent.KEYCODE_DPAD_DOWN);
            else if (hx < -0.5f) take.accept(KeyEvent.KEYCODE_DPAD_LEFT);
            else if (hx > 0.5f) take.accept(KeyEvent.KEYCODE_DPAD_RIGHT);
            return true;
        });

        LinearLayout buttons = new LinearLayout(activity);
        buttons.setPadding(0, dp(14), 0, 0);
        TextView usual = button("Default", false);
        usual.setOnClickListener(v -> {
            done[0] = true;
            set.accept(0);
            d.dismiss();
            changed();
            build();
        });
        TextView cancel = button("Cancel", true);
        cancel.setOnClickListener(v -> {
            done[0] = true;
            d.dismiss();
        });
        LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0, dp(46), 1);
        half.setMargins(dp(4), 0, dp(4), 0);
        buttons.addView(usual, half);
        buttons.addView(cancel, new LinearLayout.LayoutParams(half));
        box.addView(buttons);

        d.setContentView(box);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = activity.getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(520), (int) (m.widthPixels * 0.7f)), ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        d.show();
        box.requestFocus();
    }

    // ---- the layout editor's Edit: one control -------------------------------------------------------------------------

    // Its size and opacity (sliders), its colour, edge and a picture; the shape too but for the d-pad's (a stick's is
    // its base's), an icon and a label for a button or the menu; and for a button tap to hold, and a combo's buttons
    // or a copy of a console button as a new one (a combo of that button alone, so a controller button can fire it too)
    void editControl(Controls.Ctl c) {
        final int k = c.ordinal();
        final Dialog d = new Dialog(activity);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(12), dp(16), dp(14));
        box.setBackground(plate(PANEL, 2, 0));
        TextView title = text(cfg.name(c), 20, GOLD, true);
        title.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        box.addView(title);
        box.addView(rule(RED, 2));
        box.addView(rule(GREEN, 1));

        final TextView sizeValue = text("", 16, GOLD, false);
        Runnable showSize = () -> sizeValue.setText(Math.round(cfg.place[k][2] * 100) + " %");
        showSize.run();
        box.addView(rowView("Size", sizeValue, "This control's size, on top of the size of all of them (Touch controls)."));
        SeekBar size = new SeekBar(activity);
        size.setMax(200);  // (50 % to 250 %)
        size.setProgress(Math.round(cfg.place[k][2] * 100) - 50);
        size.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int value, boolean user) {
                if (!user) return;
                cfg.place[k][2] = (value + 50) / 100f;
                showSize.run();
                changed();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        size.setPadding(dp(16), dp(10), dp(16), dp(10));
        goldBar(size);
        box.addView(size);

        final TextView alphaValue = text("", 16, GOLD, false);
        Runnable showAlpha = () -> alphaValue.setText(cfg.alpha[k] + " %");
        showAlpha.run();
        box.addView(rowView("Opacity", alphaValue, "This control's opacity, as a share of the opacity of all of them."));
        SeekBar alpha = new SeekBar(activity);
        alpha.setMax(90);  // (10 % to 100 %)
        alpha.setProgress(cfg.alpha[k] - 10);
        alpha.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int value, boolean user) {
                if (!user) return;
                cfg.alpha[k] = value + 10;
                showAlpha.run();
                changed();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        alpha.setPadding(dp(16), dp(10), dp(16), dp(10));
        goldBar(alpha);
        box.addView(alpha);

        final boolean button = Controls.isButton(c), stick = Controls.isStick(c), menu = c == Controls.Ctl.MENU;
        if (c != Controls.Ctl.DPAD)
            box.addView(pickRow("Shape", Controls.SHAPES, () -> cfg.shape[k], v -> cfg.shape[k] = v,
                    stick ? "The stick's base. Its own: the usual circle."
                          : "Its own: the usual look (a combo of one button looks like that button)."));
        if (button || menu)
            box.addView(pickRow("Icon", Controls.ICONS, () -> cfg.icon[k], v -> cfg.icon[k] = v,
                    menu ? "An icon instead of the gear." : "An icon instead of the button's letters."));
        final TextView edgeValue = text("", 16, GOLD, false);
        Runnable showEdge = () -> edgeValue.setText(cfg.outline[k] == 0 ? "None" : cfg.outline[k] + " %");
        showEdge.run();
        box.addView(rowView("Outline", edgeValue, "The width of its bronze edge (0: none)."));
        SeekBar edge = new SeekBar(activity);
        edge.setMax(300);
        edge.setProgress(cfg.outline[k]);
        edge.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int value, boolean user) {
                if (!user) return;
                cfg.outline[k] = value;
                showEdge.run();
                changed();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        edge.setPadding(dp(16), dp(10), dp(16), dp(10));
        goldBar(edge);
        box.addView(edge);
        box.addView(pickRow("Colour", Controls.COLORS, () -> cfg.color[k], v -> cfg.color[k] = v,
                stick ? "The stick's base and knob. Clear: only the edges, the game showing through."
                      : "Its plate. Clear: only its edge, the game showing through."));
        final TextView labelValue = text("", 16, GOLD, false);
        Runnable showLabel = () -> labelValue.setText(cfg.label[k] == null ? "Its own" : cfg.label[k]);
        showLabel.run();
        LinearLayout labelRow = rowView("Label", labelValue, "Your own text on it instead of " + (menu ? "the gear" : "its letters")
                + " (empty: its own).");
        labelRow.setOnClickListener(v -> new TextPrompt(activity).show("Label", "Empty for its own",
                cfg.label[k] == null ? "" : cfg.label[k], "", 0, 12, false, false, text -> {
                    if (text == null) return;
                    cfg.label[k] = text.trim().isEmpty() ? null : text.trim();
                    showLabel.run();
                    changed();
                }));
        if (button || menu) box.addView(labelRow);
        final TextView imageValue = text(cfg.image[k] ? "Yours  ▾" : "None  ▾", 16, GOLD, false);
        LinearLayout imageRow = rowView("Picture", imageValue,
                (stick ? "An image of yours as the stick's knob (it moves with your thumb)"
                       : "An image of yours instead of its drawing")
                        + ", kept with the layout (and in exported layouts). Pressed, it darkens.");
        imageRow.setOnClickListener(v -> dropDown("Picture", cfg.image[k] ? new String[] {"Choose another…", "Remove"}
                                                                            : new String[] {"Choose an image…"}, -1, i -> {
            if (i == 1) {
                cfg.clearImage(c);
                imageValue.setText("None  ▾");
                changed();
                return;
            }
            d.dismiss();
            activity.pickImage(c, () -> editControl(c));  // (the dialog again, with the picture)
        }));
        box.addView(imageRow);
        box.addView(pickRow("Picture shape", new String[] {stick ? "Cut round" : "Cut to the button", "The picture's own"},
                () -> cfg.fit[k] ? 1 : 0, v -> cfg.fit[k] = v != 0,
                stick ? "Cut round: the picture fills the knob, with its edge. The picture's own: only the picture, "
                        + "in its proportions (the stick still takes touches on its whole base)."
                      : "Cut to the control: the picture fills its shape, with its edge. The picture's own: only the "
                        + "picture, in its proportions, and only where it's drawn takes touches (a see-through "
                        + "background doesn't)."));
        if (button) {
            box.addView(pickRow("Tap to hold", new String[] {"Off", "On"}, () -> cfg.hold[k] ? 1 : 0, v -> cfg.hold[k] = v != 0,
                    "On: a tap presses it and keeps it pressed until the next tap (a small diamond shows it)."));
            if (c.combo()) {
                LinearLayout r = rowView("Buttons and controller button", text("›", 16, GOLD, false),
                        "What it presses, and the controller button that does the same.");
                r.setOnClickListener(v -> {
                    d.dismiss();
                    editCombo(c.comboIndex());
                });
                box.addView(r);
            } else {
                LinearLayout r = rowView("Copy as a new button", text("›", 16, GOLD, false),
                        "Adds another " + Controls.label(c) + " button (a combo of it alone: Settings → Combos, where a "
                                + "controller button can fire it too).");
                r.setOnClickListener(v -> {
                    int j = cfg.freeCombo();
                    if (j < 0) return;
                    Controls.Ctl copy = Controls.Ctl.values()[Controls.Ctl.COMBO1.ordinal() + j];
                    cfg.combo[j] = cfg.buttons(c);
                    cfg.comboPad[j] = 0;
                    float[] p = cfg.place[copy.ordinal()];
                    p[0] = Math.min(0.95f, cfg.place[k][0] + 0.06f);
                    p[1] = cfg.place[k][1];
                    p[2] = cfg.place[k][2];
                    cfg.shown[copy.ordinal()] = true;
                    cfg.shape[copy.ordinal()] = cfg.shape[k];
                    cfg.icon[copy.ordinal()] = cfg.icon[k];
                    cfg.hold[copy.ordinal()] = cfg.hold[k];
                    cfg.color[copy.ordinal()] = cfg.color[k];
                    cfg.alpha[copy.ordinal()] = cfg.alpha[k];
                    cfg.label[copy.ordinal()] = cfg.label[k];
                    cfg.outline[copy.ordinal()] = cfg.outline[k];
                    d.dismiss();
                    changed();
                });
                box.addView(r);
            }
        }

        TextView ok = button("Done", true);
        ok.setOnClickListener(v -> d.dismiss());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(46));
        lp.setMargins(dp(4), dp(10), dp(4), 0);
        box.addView(ok, lp);
        ScrollView sv = new ScrollView(activity);
        sv.addView(box);
        d.setContentView(sv);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = activity.getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(560), (int) (m.widthPixels * 0.8f)), ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        d.show();
    }

    private void goldBar(SeekBar bar) {
        bar.setProgressTintList(ColorStateList.valueOf(GOLD));
        bar.setProgressBackgroundTintList(ColorStateList.valueOf(EDGE));
        bar.setThumbTintList(ColorStateList.valueOf(GOLD));
    }

    // A row in a dialog: its value, and a tap opens the drop-down (two options: flips)
    private LinearLayout pickRow(String title, String[] options, IntSupplier get, IntConsumer set, String help) {
        final TextView value = text("", 16, GOLD, false);
        final boolean many = options.length > 2;
        Runnable show = () -> value.setText(options[get.getAsInt()] + (many ? "  ▾" : ""));
        show.run();
        LinearLayout r = rowView(title, value, help);
        r.setOnClickListener(v -> {
            if (!many) {
                set.accept(1 - get.getAsInt());
                show.run();
                changed();
                return;
            }
            dropDown(title, options, get.getAsInt(), i -> {
                set.accept(i);
                show.run();
                changed();
            });
        });
        return r;
    }

    // ---- combo editor ---------------------------------------------------------------------------------------------------

    void editCombo(int k) {
        final Dialog d = new Dialog(activity);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(12), dp(16), dp(14));
        box.setBackground(plate(PANEL, 2, 0));
        TextView title = text(cfg.comboName(k), 20, GOLD, true);
        title.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        box.addView(title);
        box.addView(rule(RED, 2));
        box.addView(rule(GREEN, 1));
        box.addView(text("The buttons it presses:", 14, DIM, false));

        // buttons in rows of eight: tap one to add or remove it
        final long[] mask = {cfg.combo[k]};
        final long[] padMask = {cfg.comboPad[k]};
        LinearLayout grid = new LinearLayout(activity);
        grid.setOrientation(LinearLayout.VERTICAL);
        LinearLayout line = null;
        final TextView[] chips = new TextView[Controls.BUTTONS.length];
        for (int i = 0; i < Controls.BUTTONS.length; i++) {
            if (i % 8 == 0) {
                line = new LinearLayout(activity);
                grid.addView(line);
            }
            final int b = i;
            TextView chip = text(Controls.NAMES[i], 16, CREAM, true);
            chip.setGravity(Gravity.CENTER);
            chip.setOnClickListener(v -> {
                mask[0] ^= Controls.BUTTONS[b];
                paintChip(chips[b], (mask[0] & Controls.BUTTONS[b]) != 0);
            });
            chips[i] = chip;
            paintChip(chip, (mask[0] & Controls.BUTTONS[i]) != 0);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(44), 1);
            lp.setMargins(dp(3), dp(3), dp(3), dp(3));
            line.addView(chip, lp);
        }
        box.addView(grid);

        // the controller button that fires it: one of Controls.PAD_TRIGGERS, picked from a drop-down
        final TextView padValue = text("", 16, GOLD, false);
        LinearLayout padRow = rowView("Controller button", padValue,
                "While it is held, the game gets the combo's buttons instead of it.");
        Runnable showPad = () -> padValue.setText((padMask[0] == 0 ? "None" : Controls.names(padMask[0])) + "  ▾");
        showPad.run();
        padRow.setOnClickListener(v -> {
            int at = 0;
            final String[] names = new String[Controls.PAD_TRIGGERS.length];
            for (int i = 0; i < names.length; i++) {
                if (Controls.PAD_TRIGGERS[i] == padMask[0]) at = i;
                names[i] = Controls.PAD_TRIGGERS[i] == 0 ? "None" : Controls.names(Controls.PAD_TRIGGERS[i]);
            }
            dropDown("Controller button", names, at, i -> {
                padMask[0] = Controls.PAD_TRIGGERS[i];
                showPad.run();
            });
        });
        box.addView(padRow);

        LinearLayout buttons = new LinearLayout(activity);
        buttons.setPadding(0, dp(10), 0, 0);
        TextView clear = button("Clear", false);
        clear.setOnClickListener(v -> {
            cfg.combo[k] = 0;
            cfg.comboPad[k] = 0;
            d.dismiss();
            changed();
            build();
        });
        TextView ok = button("Save", true);
        ok.setOnClickListener(v -> {
            cfg.combo[k] = mask[0];
            cfg.comboPad[k] = mask[0] == 0 ? 0 : padMask[0];
            d.dismiss();
            changed();
            build();
        });
        LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0, dp(46), 1);
        half.setMargins(dp(4), 0, dp(4), 0);
        buttons.addView(clear, half);
        buttons.addView(ok, new LinearLayout.LayoutParams(half));
        box.addView(buttons);

        d.setContentView(box);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = activity.getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(620), (int) (m.widthPixels * 0.85f)), ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        d.show();
    }

    private void paintChip(TextView chip, boolean on) {
        chip.setTextColor(on ? 0xFF281010 : CREAM);
        chip.setBackground(plate(on ? GOLD : ROW, 1, 4));
    }

    // ---- mods and cheats (ModFiles; the runtime's side is fs.cpp and cheats.cpp) ----------------------------------------

    // What the last export or convert did, shown at the top of Saves (GameActivity sets it)
    static String saveMessage;

    private void buildSaves() {
        if (folder == null) {
            note("Pick the game folder first.");
            return;
        }
        if (saveMessage != null) note(saveMessage);
        header("This save");
        note(SaveFiles.describe(folder) + " The port uses the Switch's save files, so a save exported from here works on the "
                + "Switch (with JKSV or Checkpoint), Eden and Yuzu as it is.");
        action("Export (Switch, Eden)", "Saves a zip of the save folder: data001Slot.bin, data00-1.bin.", () -> {
            dialog.dismiss();
            activity.exportSave(false);
        });
        action("Export for Steam", "Converts the save to the PC version's and zips it in win64_save/, to put in Steam's "
                + "...\\1446780\\remote\\win64_save (close the game first, and turn off Steam Cloud for it or it may bring the "
                + "old one back). It's encrypted for the Steam account below.", () -> {
            dialog.dismiss();
            activity.exportSave(true);
        });

        header("Steam account");
        String account = SaveFiles.steamAccount(folder);
        note(account != null ? "Saves for Steam are encrypted for " + account + "." : "None yet: a save for Steam comes "
                + "out unencrypted. The PC version encrypts its saves with a key that comes from the Steam ID, and the only way "
                + "to get that key is from a save the game encrypted for that account, so the port learns it from any Steam "
                + "save it opens. Played in the port first? Start the PC game once on that account, make a character and "
                + "save, then pick that save here.");
        action("Set from a Steam save", "Pick a save from that Steam account (a zip or its win64_save folder). Nothing is "
                + "imported: the port only learns the account's key and the PC version's layout from it.", () -> dropDown(
                "Set from a Steam save", new String[] {"From a .zip", "From a folder"}, -1, i -> {
                    dialog.dismiss();
                    activity.pickSave(i == 1, 2);
                }));

        header("Import");
        note("Takes the place of this save. The one here is backed up first. A Steam save is converted on the way in: it's "
                + "decrypted with your Steam ID (read from the save itself when it can be, asked for when not), then filled "
                + "into this save's layout. The game closes right after, so it can't write the old save back.");
        String[] from = {"From a .zip", "From a folder"};
        action("Import a save", "A zip or a folder with data00-1.bin and a data00#Slot.bin, from the Switch, Eden, Yuzu, "
                + "another copy of the port, or Steam (win64_save).", () -> dropDown("Import a save", from, -1, i -> {
                    dialog.dismiss();
                    activity.pickSave(i == 1, 0);
                }));
        List<File> backups = SaveFiles.backups(folder);
        if (!backups.isEmpty()) {
            String[] names = new String[backups.size()];
            for (int i = 0; i < names.length; i++) names[i] = backups.get(i).getName();
            action("Restore a backup", "The save as it was before an import or restore (the last 10 are kept).",
                    () -> dropDown("Restore a backup", names, -1, i -> dropDown("Restore the save from " + names[i] + "?",
                            new String[] {"Keep this one", "Restore it"}, -1, k -> {
                                if (k == 1) {
                                    try {
                                        String done = SaveFiles.restore(folder, backups.get(i));
                                        dialog.dismiss();
                                        activity.saveOver(done);
                                        return;
                                    } catch (IOException e) {
                                        saveMessage = "Couldn't restore it: " + e.getMessage();
                                    }
                                }
                                build();
                            })));
        }

        header("Convert");
        note("A save from one platform into a zip for the other, without touching this one. Steam to Switch fills this "
                + "save's layout (have one here first for the best result). Switch to Steam fills the Steam account's save "
                + "and is encrypted for it.");
        action("Convert a save", "Pick a Steam save to get the Switch's, or a Switch save to get Steam's; then where to "
                + "save the zip.", () -> dropDown("Convert a save", from, -1, i -> {
                    dialog.dismiss();
                    activity.pickSave(i == 1, 1);
                }));
    }

    private void buildMods() {
        if (folder == null) {
            note("Pick the game folder first.");
            return;
        }
        List<ModFiles.Mod> mods = ModFiles.list(folder);
        header("Mods");
        note("Each mod is a folder in " + ModFiles.modsDir(folder) + ", laid out as for Eden: romfs/ with the game files it "
                + "replaces, cheats/ with its cheat files. Adding a mod or turning one on or off applies the next time the "
                + "game starts.");
        action("Add a mod (.zip)",
                "Pick a mod's zip, as made for Eden or Yuzu. Its romfs (or its game folders, natives/...) and cheats go into "
                        + "a folder of their own under mods. Patches for exefs aren't used by this port.",
                () -> {
                    dialog.dismiss();
                    activity.importMod();
                });
        for (ModFiles.Mod m : mods) {
            String what = m.romfs && m.cheats ? "Game files and cheats" : m.romfs ? "Game files (romfs)"
                    : m.cheats ? "Cheats only" : "Nothing the port uses (no romfs or cheats folder)";
            toggle(m.name, what + ". In " + m.dir.getName() + ".", () -> m.on ? 1 : 0, v -> {
                if (!ModFiles.setOn(m, v != 0)) android.widget.Toast.makeText(activity, "Couldn't rename " + m.dir.getName(),
                        android.widget.Toast.LENGTH_LONG).show();
                build();
            });
        }
        if (!mods.isEmpty()) {
            String[] names = new String[mods.size()];
            for (int i = 0; i < names.length; i++) names[i] = mods.get(i).name;
            action("Remove a mod", "Deletes the mod's folder and everything in it.", () -> dropDown("Remove a mod", names, -1,
                    i -> dropDown("Delete " + names[i] + "?", new String[] {"Keep it", "Delete it"}, -1, k -> {
                        if (k == 1) ModFiles.delete(mods.get(i));
                        build();
                    })));
        }

        String buildId = ModFiles.buildId(folder);
        header("Cheats");
        note(buildId != null
                ? "Atmosphere cheats, as in Eden. This game's build ID is " + buildId + ", so a mod's cheat file is cheats/"
                        + buildId + ".txt. A cheat turned on starts working within a second; the master code of its file "
                        + "({...}) runs along with it."
                : "Can't read the game's build ID (the exefs isn't where the setup found it), so no cheats can be listed.");
        action("Add a cheat file (.txt)",
                "Pick a cheat .txt for this version of the game. It goes into a mod of its own, named after the file. A file "
                        + "named after another build ID is for another game version and isn't taken.",
                () -> {
                    dialog.dismiss();
                    activity.importCheats();
                });
        boolean any = false;
        for (ModFiles.Mod m : mods) {
            if (!m.on) continue;
            List<File> files = ModFiles.cheatFiles(m, buildId);
            for (File f : files) {
                List<ModFiles.Cheat> cheats = ModFiles.cheats(m, f);
                note("◆ " + m.name + (files.size() > 1 ? " · " + f.getName() : ""));
                for (ModFiles.Cheat c : cheats) {
                    if (c.master) continue;
                    any = true;
                    flag(c.name, "From " + m.name + ". On: it runs 12 times a second, as on the console.", c.key, false);
                }
            }
            int other = ModFiles.otherBuilds(m, buildId);
            if (other > 0) note(m.name + ": " + other + " cheat file(s) for another game version, not used.");
        }
        if (!any) note("No cheats yet. Add a cheat file, or a mod with a cheats folder.");
    }

    // ---- rows -----------------------------------------------------------------------------------------------------------

    private void header(String title) {
        TextView t = text(title, 18, GOLD, true);
        t.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        t.setPadding(dp(4), dp(16), 0, dp(4));
        list.addView(t);
        View line = rule(EDGE, 1);
        list.addView(line);
    }

    private void note(String s) {
        TextView t = text(s, 13, DIM, false);
        t.setPadding(dp(4), dp(6), dp(4), dp(2));
        list.addView(t);
    }

    // A row: title on the left, value and "?" on the right; a tap calls click(+1)
    private TextView row(String title, String value, String help, IntConsumer click, boolean steppers) {
        TextView v = text(value, 16, GOLD, false);
        LinearLayout r = rowView(title, v, help);
        r.setOnClickListener(x -> click.accept(1));
        list.addView(r);
        return v;
    }

    private LinearLayout rowView(String title, TextView value, String help) {
        LinearLayout outer = new LinearLayout(activity);
        outer.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams olp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        olp.setMargins(0, dp(4), 0, 0);
        outer.setLayoutParams(olp);
        outer.setBackground(plate(ROW, 1, 2));
        LinearLayout r = new LinearLayout(activity);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(14), dp(10), dp(8), dp(10));
        TextView t = text(title, 16, CREAM, false);
        r.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        value.setPadding(dp(8), 0, dp(8), 0);
        r.addView(value);
        final TextView helpText = text(help, 13, DIM, false);
        helpText.setPadding(dp(14), 0, dp(14), dp(10));
        helpText.setVisibility(View.GONE);
        TextView q = text("?", 15, GOLD, true);
        q.setGravity(Gravity.CENTER);
        GradientDrawable ring = new GradientDrawable();
        ring.setShape(GradientDrawable.OVAL);
        ring.setStroke(dp(1), EDGE);
        q.setBackground(ring);
        q.setOnClickListener(x -> helpText.setVisibility(helpText.getVisibility() == View.GONE ? View.VISIBLE : View.GONE));
        r.addView(q, new LinearLayout.LayoutParams(dp(30), dp(30)));
        outer.addView(r);
        outer.addView(helpText);
        return outer;
    }

    private void toggle(String title, String help, IntSupplier get, IntConsumer set) {
        String[] onOff = {"Off", "On"};
        choice(title, help, onOff, get, set);
    }

    // Two options (on / off): a tap flips it. More than two: a drop-down with the current one highlighted.
    private void choice(String title, String help, String[] options, IntSupplier get, IntConsumer set) {
        final TextView[] value = new TextView[1];
        final boolean list = options.length > 2;
        value[0] = row(title, options[get.getAsInt()] + (list ? "  ▾" : ""), help, step -> {
            if (!list) {
                set.accept((get.getAsInt() + step) % options.length);
                value[0].setText(options[get.getAsInt()]);
                changed();
                return;
            }
            dropDown(title, options, get.getAsInt(), i -> {
                set.accept(i);
                value[0].setText(options[get.getAsInt()] + "  ▾");
                changed();
            });
        }, false);
    }

    // The drop-down: a panel like the menu itself, with a title, the X and one row per option (current one in gold)
    private void dropDown(String title, String[] options, int current, IntConsumer pick) {
        final Dialog d = new Dialog(activity);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackground(plate(PANEL, 2, 0));
        LinearLayout top = new LinearLayout(activity);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(16), dp(8), dp(8), dp(8));
        TextView t = text("◆  " + title, 18, GOLD, true);
        t.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD));
        TextView close = text("✕", 20, CREAM, false);
        close.setPadding(dp(12), dp(2), dp(12), dp(2));
        close.setBackground(plate(0xFF1A0C09, 1, 2));
        close.setOnClickListener(v -> d.dismiss());
        top.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        top.addView(close);
        root.addView(top);
        root.addView(rule(RED, 2));
        root.addView(rule(GREEN, 1));
        LinearLayout items = new LinearLayout(activity);
        items.setOrientation(LinearLayout.VERTICAL);
        items.setPadding(dp(10), dp(6), dp(10), dp(10));
        for (int i = 0; i < options.length; i++) {
            final int k = i;
            final boolean on = i == current;
            LinearLayout r = new LinearLayout(activity);
            r.setGravity(Gravity.CENTER_VERTICAL);
            r.setPadding(dp(14), dp(12), dp(14), dp(12));
            r.setBackground(on ? gold() : plate(ROW, 1, 2));
            TextView name = text((on ? "◆  " : "") + options[i], 16, on ? 0xFF281010 : CREAM, on);
            r.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            if (on) r.addView(text("◆", 14, 0xFF281010, true));
            r.setOnClickListener(v -> {
                d.dismiss();
                if (k != current) pick.accept(k);
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(4);
            items.addView(r, lp);
        }
        ScrollView sv = new ScrollView(activity);
        sv.addView(items);
        root.addView(sv);
        d.setContentView(root);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = activity.getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(460), (int) (m.widthPixels * 0.6f)), ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        d.show();
    }

    // Selected drop-down row: gold with a glow, like the game's menus
    private GradientDrawable gold() {
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {0xFFD4A43C, 0xFF8E5A22});
        g.setStroke(dp(2), 0xFFF0D080);
        g.setCornerRadius(dp(2));
        return g;
    }

    // Saved menu size, in %
    static int menuScale(android.content.Context c) {
        try {
            return c.getSharedPreferences(PREFS, android.content.Context.MODE_PRIVATE).getInt(SCALE_KEY, SCALE_DEFAULT);
        } catch (RuntimeException e) {
            return SCALE_DEFAULT;
        }
    }

    // A number with − and +: tapping the row also steps up (and wraps to the minimum past the maximum)
    private void number(String title, String help, int min, int max, int step, String unit, IntSupplier get, IntConsumer set) {
        TextView value = text(get.getAsInt() + unit, 16, GOLD, false);
        LinearLayout r = rowView(title, value, help);
        LinearLayout line = (LinearLayout) r.getChildAt(0);
        TextView minus = smallButton("−"), plus = smallButton("+");
        line.addView(minus, 1, new LinearLayout.LayoutParams(dp(40), dp(36)));
        line.addView(plus, 3, new LinearLayout.LayoutParams(dp(40), dp(36)));
        IntConsumer by = d -> {
            set.accept(Math.max(min, Math.min(max, get.getAsInt() + d)));
            value.setText(get.getAsInt() + unit);
            changed();
        };
        minus.setOnClickListener(x -> by.accept(-step));
        plus.setOnClickListener(x -> by.accept(step));
        r.setOnClickListener(x -> {
            int next = get.getAsInt() + step;
            set.accept(next > max ? min : next);
            value.setText(get.getAsInt() + unit);
            changed();
        });
        list.addView(r);
    }

    // ---- game switches --------------------------------------------------------------------------------------------------

    static File gameFolder(GameActivity a) {
        try {
            String path = new String(Files.readAllBytes(new File(a.getFilesDir(), "carpeta.txt").toPath())).trim();
            return path.isEmpty() ? null : new File(path);
        } catch (IOException e) {
            return null;
        }
    }

    // ---- bug report -------------------------------------------------------------------------------------------------

    private static String reportStatus;

    private void trigger(String name, String content) {
        BugReport.trigger(folder, name, content);
    }

    private void saveReport() {
        reportStatus = "Saving the report…";
        build();
        BugReport.save(folder, done -> activity.runOnUiThread(() -> {
            reportStatus = done;
            build();
        }));
    }

    // The ASTC cache (runtime: nvn/astc.cpp): in the app's files folder, or the data folder with astc_cache_in_data_folder
    private File[] astcCacheDirs() {
        return new File[] {new File(activity.getFilesDir(), "astc_cache"), folder != null ? new File(folder, "astc_cache") : null};
    }

    private String astcCacheSize() {
        long bytes = 0;
        for (File dir : astcCacheDirs()) {
            File[] files = dir != null ? dir.listFiles() : null;
            if (files != null)
                for (File f : files) bytes += f.length();
        }
        return String.format(java.util.Locale.ROOT, "%.0f MB", bytes / 1048576.0);
    }

    private void clearAstcCache() {
        for (File dir : astcCacheDirs()) {
            File[] files = dir != null ? dir.listFiles() : null;
            if (files != null)
                for (File f : files) f.delete();
        }
    }

    private GpuInfo gpu;  // the options that apply to this GPU only (all of them before the game's first start)
    private boolean resetArmed;  // Restore default settings asks for a second tap

    // Every switch the menu has ever written as its own file, so Restore default settings clears old folders too.
    private static final String[] SWITCHES = {
        "60fps", "adreno_turbo", "cutout", "system_bars", "cool_down", "no_hud_heat", "no_tight_barriers", "tight_barriers", "int_attribs", "gpu_unswizzle", "async_present", "pipeline_threads_1", "pipeline_threads_3", "pipeline_threads_4", "fg_target_60", "fg_target_90", "fg_target_120", "dynamic_state", "vertex_input_state",
        "no_dynamic_state", "astc_cache_in_data_folder", "bc_astc", "aspect_16_10", "aspect_21_9", "aspect_4_3", "aspect_stretch", "bindless_arrays", "breadcrumbs",
        "capture_button", "count_check", "depth_gequal", "dock_mode", "effects_lower", "effects_lowest", "fast_depth", "fps_20", "fps_25", "fps_40",
        "fps_45", "gpu_occlusion", "hud_battery", "hud_cpu", "hud_frametime", "hud_gpu", "hud_ram", "hud_shaders", "no_hud_battery", "no_hud_cpu",
        "no_hud_frametime", "no_hud_gpu", "no_hud_ram", "no_hud_shaders", "lsfg_no_guard", "keep_dead_functions", "log_lines", "low_memory", "model_detail_high", "model_detail_low",
        "model_detail_lowest", "model_detail_min", "no_adpf", "no_af", "no_affinity", "no_ao", "no_async_shaders", "no_bindless_arrays", "no_bloom",
        "no_chromatic_aberration", "no_depth_of_field", "no_dynamic_resolution", "no_dynamic_shadows",
        "no_effect_ssr", "no_film_grain", "no_foliage_sway", "no_fps", "no_fsr", "no_fxaa", "no_god_rays",
        "no_gpu_occlusion", "no_int_attribs", "no_lens_flare", "no_motion_blur", "no_quad_derivatives", "no_readback_copies",
        "no_return_mali", "no_return_qualcomm", "no_robust", "no_count_check", "no_safe_translation", "no_shadows", "no_short_subgroups",
        "no_shuffle_qualcomm", "no_spirv_opt_brx", "no_area", "no_deferred_clear", "no_store_ops", "no_ubwc", "no_warmup", "perf_stats", "resolution_240", "resolution_360",
        "resolution_432", "resolution_480", "resolution_576", "resolution_648", "resolution_720", "shadow_cache",
        "shadow_half", "shadow_quarter", "short_subgroups", "single_thread_pipelines", "small_object_culling",
        "texture_bias_1", "texture_bias_2", "tv_mode", "ultrawide", "verbose_log", "wide_view",
    };

    private boolean has(String name) {
        return name != null && ini.on(name);
    }

    private void put(String name, boolean on) {
        ini.setOn(name, on);
    }

    // An on / off row backed by a switch in mhr.ini. inverted: the switch turns it off (the no_* ones).
    private void flag(String title, String help, String file, boolean inverted) {
        toggle(title, help, () -> has(file) != inverted ? 1 : 0, v -> put(file, (v != 0) != inverted));
    }

    // What the game is using right now (game_settings.txt, written by the game when a value changes). The menu shows it
    // when it opens: change a setting, close the menu and open it again to see it applied.
    private String gameStatus() {
        String shadow = null, model = null, texture = null;
        try {
            for (String line : Files.readAllLines(new File(folder, "game_settings.txt").toPath())) {
                String[] w = line.trim().split(" ");
                if (w[0].equals("shadow") && w.length >= 3)
                    shadow = w[1].equals(w[2]) ? "shadow maps " + w[1] + " (game's)" : "shadow maps " + w[1] + " (game's " + w[2] + ")";
                else if (w[0].equals("model") && w.length >= 2)
                    model = Math.abs(Float.parseFloat(w[1]) - 1f) < 0.001f ? "model detail game's"
                            : String.format(java.util.Locale.ROOT, "model detail x%.2f", Float.parseFloat(w[1]));
                else if (w[0].equals("texture") && w.length >= 3)
                    texture = w[1].equals(w[2]) ? "texture streaming game's"
                            : "textures " + (Integer.parseInt(w[1]) - Integer.parseInt(w[2])) + " size(s) smaller";
            }
        } catch (IOException | RuntimeException e) {
            return "In the game now: nothing yet (the game writes it while running; this game version may not be supported).";
        }
        StringBuilder b = new StringBuilder("In the game now: ");
        for (String part : new String[] {shadow, model, texture})
            if (part != null) b.append(b.length() > 17 ? " · " : "").append(part);
        if (b.length() == 17) return "In the game now: nothing yet (the game writes it once it's in town or a hunt).";
        return b.append(". Reopen the menu after a change to see it.").toString();
    }

    // A choice where each option (but the first, the default) is its own switch
    private void exclusive(String title, String help, String[] options, String[] files) {
        choice(title, help, options, () -> {
            for (int i = files.length - 1; i > 0; i--)
                if (has(files[i])) return i;
            return 0;
        }, v -> {
            for (int i = 1; i < files.length; i++) put(files[i], i == v);
        });
    }

    private void action(String title, String help, Runnable run) {
        row(title, "›", help, x -> run.run(), false);
    }

    // ---- pieces ---------------------------------------------------------------------------------------------------------

    private TextView smallButton(String s) {
        TextView b = text(s, 18, CREAM, true);
        b.setGravity(Gravity.CENTER);
        b.setBackground(plate(0xFF1A0C09, 1, 2));
        return b;
    }

    private TextView button(String s, boolean main) {
        TextView b = text(s, 16, main ? 0xFF281010 : CREAM, true);
        b.setGravity(Gravity.CENTER);
        b.setBackground(plate(main ? GOLD : 0xFF3E2616, 1, 4));
        return b;
    }

    private View rule(int color, int heightDp) {
        View v = new View(activity);
        v.setBackgroundColor(color);
        v.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(heightDp)));
        return v;
    }

    private GradientDrawable plate(int fill, int strokeDp, int radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(strokeDp), EDGE);
        d.setCornerRadius(dp(radiusDp));
        return d;
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(activity);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp * scale);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private int dp(int v) {  // (scaled by the menu size; 1 dp rules never vanish)
        float px = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v * scale, activity.getResources().getDisplayMetrics());
        return v > 0 ? Math.max(1, Math.round(px)) : Math.round(px);
    }
}
