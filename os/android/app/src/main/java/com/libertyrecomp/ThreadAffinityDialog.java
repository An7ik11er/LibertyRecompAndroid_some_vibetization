package com.libertyrecomp;

import android.app.AlertDialog;
import android.app.Dialog;
import android.util.Log;
import android.view.ViewGroup;
import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * "CPU thread placement" dialog: automatic (android_thread_pinning.cpp rules) or manual
 * placement of the game's known threads on the device's cores, per thread or per group.
 *
 * Launch contract with native code (android_thread_pinning.cpp):
 *   environment variable LIBERTY_THREAD_AFFINITY, set by LibertyActivity before libmain.so loads:
 *     auto
 *     manual;<thread id>:<hex cpu mask>;<thread id>:<hex cpu mask>...
 *   It wins over the android_thread_affinity cvar (args.txt / native.toml), which is used only
 *   when the game is started without the launcher.
 * Threads not listed keep their automatic placement. Thread ids below MUST match kIds
 * in android_thread_pinning.cpp, and the auto kinds / nice values MUST match its kLanes and
 * kClusters tables.
 *
 * The automatic layout itself is the android_thread_layout cvar (lanes, clusters, priority,
 * off), read here from LibertyRecomp/native.toml and args.txt (args.txt wins, as in the
 * runtime), so the dialog shows what the native side will actually do.
 */
final class ThreadAffinityDialog {
    static final String PREFS = "performance_test";          // same file as the other perf settings
    static final String KEY_AUTO = "affinity_auto";
    static final String KEY_MANUAL = "affinity_manual";      // JSON {"g.audio":15,"t.recorder":112}
    static final String EXTRA_SPEC = "perf_thread_affinity"; // Intent extra Launcher -> LibertyActivity
    static final String ARG_PREFIX = "--android_thread_affinity=";
    static final String ENV_SPEC = "LIBERTY_THREAD_AFFINITY";       // LibertyActivity -> native

    private static final int WARN_COLOR = 0xFFF2C744;        // yellow for "you can, but you should not"
    private static final int LITTLE_COLOR = 0xFF6FA8DC;
    private static final int BIG_COLOR = 0xFF93C47D;
    private static final int PRIME_COLOR = 0xFFE06666;

    // ------------------------------------------------------------------ topology

    /** Same detection as DetectTopology() in android_thread_pinning.cpp. */
    static final class Topology {
        int count;
        int little, big, prime;
        int[] maxKhz;
        boolean fallback;

        int all() { return count >= 32 ? -1 : (1 << count) - 1; }
        int workers() { return little != 0 ? little : (little | big); }
        int littleBig() { return little | big; }
        int bigPrime() { return big | prime; }
        /** Highest big core (next to prime): the recorder's second core in "lanes". */
        int topBig() { return big == 0 ? 0 : Integer.highestOneBit(big); }
        /** Big cores without the top one: audio lives here in "lanes". */
        int bigLow() {
            int rest = big & ~topBig();
            return rest != 0 ? rest : big;
        }

        String kindOf(int cpu) {
            int bit = 1 << cpu;
            if ((prime & bit) != 0) return "P";
            if ((big & bit) != 0) return "B";
            return "L";
        }
    }

    static Topology detectTopology() {
        Topology t = new Topology();
        int possible = Runtime.getRuntime().availableProcessors();
        String range = readFirstLine(new File("/sys/devices/system/cpu/possible")); // "0-7"
        if (range != null) {
            try {
                int dash = range.lastIndexOf('-');
                possible = Integer.parseInt(range.substring(dash + 1).trim()) + 1;
            } catch (RuntimeException ignored) { }
        }
        t.count = Math.max(1, Math.min(possible, 16));
        t.maxKhz = new int[t.count];
        int[] cap = new int[t.count];
        int maxCap = 0, minCap = Integer.MAX_VALUE;
        boolean ok = false;
        for (int cpu = 0; cpu < t.count; cpu++) {
            String base = "/sys/devices/system/cpu/cpu" + cpu;
            t.maxKhz[cpu] = parseInt(readFirstLine(new File(base + "/cpufreq/cpuinfo_max_freq")));
            int c = parseInt(readFirstLine(new File(base + "/cpu_capacity")));
            if (c <= 0) c = t.maxKhz[cpu];
            cap[cpu] = c;
            if (c > 0) {
                ok = true;
                maxCap = Math.max(maxCap, c);
                minCap = Math.min(minCap, c);
            }
        }
        if (ok) {
            for (int cpu = 0; cpu < t.count; cpu++) {
                if (cap[cpu] <= 0) continue;
                if (cap[cpu] == maxCap) t.prime |= 1 << cpu;
                else if (cap[cpu] <= minCap + 100) t.little |= 1 << cpu;
                else t.big |= 1 << cpu;
            }
        }
        if (t.little != 0 && t.prime != 0 && t.big == 0) {
            int moveToBig = Integer.bitCount(t.little) / 2;
            for (int cpu = t.count - 1; cpu >= 0 && moveToBig > 0; cpu--) {
                if ((t.little & (1 << cpu)) != 0) {
                    t.little &= ~(1 << cpu);
                    t.big |= 1 << cpu;
                    moveToBig--;
                }
            }
        }
        if (t.little == 0 || t.big == 0 || t.prime == 0) {   // generic fallback, as in native
            t.fallback = true;
            int n = t.count, half = n / 2;
            t.prime = 1 << (n - 1);
            t.big = 0;
            for (int i = half; i < n - 1; i++) t.big |= 1 << i;
            t.little = 0;
            for (int i = 0; i < half; i++) t.little |= 1 << i;
        }
        return t;
    }

    // ------------------------------------------------------------------ known threads

    static final int K_LITTLE = 0, K_BIG = 1, K_PRIME = 2, K_BIG_PRIME = 3, K_WORKERS = 4, K_LITTLE_BIG = 5,
            K_RECORDER_LANE = 6, K_AUDIO_LANE = 7, K_JOBS_LANE = 8, K_NONE = 9;

    /** Same as AutoMask() in android_thread_pinning.cpp; 0 = not pinned (Android decides). */
    static int autoMask(int kind, Topology t) {
        switch (kind) {
            case K_LITTLE: return t.little;
            case K_BIG: return t.big;
            case K_PRIME: return t.prime;
            case K_BIG_PRIME: return t.bigPrime();
            case K_LITTLE_BIG: return t.littleBig();
            case K_RECORDER_LANE: return t.prime | t.topBig();
            case K_AUDIO_LANE: return t.bigLow();
            case K_JOBS_LANE: return t.little | t.bigLow();
            case K_NONE: return 0;
            default: return t.workers();
        }
    }

    // ------------------------------------------------------------------ automatic layout

    /** android_thread_layout values (android_thread_pinning.cpp). */
    static final int L_LANES = 0, L_CLUSTERS = 1, L_PRIORITY = 2, L_OFF = 3, L_LANES2 = 4;
    static final String[] LAYOUT_NAMES = {"lanes", "clusters", "priority", "off", "lanes2"};
    static final String LAYOUT_ARG = "--android_thread_layout=";
    static final int KEEP = Integer.MIN_VALUE;   // nice left as it is

    static int parseLayout(String value) {
        String v = value.trim().replace("\"", "").toLowerCase(java.util.Locale.ROOT);
        if (v.equals("lanes2")) return L_LANES2;
        if (v.equals("clusters")) return L_CLUSTERS;
        if (v.equals("priority") || v.equals("nice")) return L_PRIORITY;
        if (v.equals("off") || v.equals("none")) return L_OFF;
        return L_LANES;
    }

    /** The layout the game will start with: native.toml, then args.txt (the last line wins). */
    static int readLayout(Context context) {
        File external = context.getExternalFilesDir(null);
        File root = external != null ? external : context.getFilesDir();
        int layout = L_LANES;
        for (String line : readLines(new File(new File(root, "LibertyRecomp"), "native.toml"))) {
            String t = line.trim();
            if (!t.startsWith("android_thread_layout")) continue;
            int eq = t.indexOf('=');
            if (eq > 0 && t.substring(0, eq).trim().equals("android_thread_layout"))
                layout = parseLayout(t.substring(eq + 1));
        }
        for (String line : readLines(new File(root, "args.txt"))) {
            String t = line.trim();
            if (t.startsWith(LAYOUT_ARG)) layout = parseLayout(t.substring(LAYOUT_ARG.length()));
        }
        return layout;
    }

    static void writeLayout(Context context, int layout) {
        if (layout < 0 || layout >= LAYOUT_NAMES.length) return;
        File external = context.getExternalFilesDir(null);
        File root = external != null ? external : context.getFilesDir();
        File f = new File(root, "args.txt");
        List<String> lines = new ArrayList<>();
        if (f.exists()) {
            lines.addAll(readLines(f));
        } else {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(context.getAssets().open("default_args.txt")))) {
                String line;
                while ((line = r.readLine()) != null) lines.add(line);
            } catch (Exception ignored) {}
        }
        boolean replaced = false;
        String newLine = LAYOUT_ARG + LAYOUT_NAMES[layout];
        for (int i = 0; i < lines.size(); i++) {
            if (lines.get(i).trim().startsWith(LAYOUT_ARG)) {
                lines.set(i, newLine);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            lines.add(newLine);
        }
        try {
            if (f.exists() && !f.canWrite()) f.delete();
            try (BufferedWriter w = new BufferedWriter(new FileWriter(f))) {
                for (String l : lines) {
                    w.write(l);
                    w.newLine();
                }
            }
        } catch (IOException e) {
            Log.e("LibertyAffinity", "Failed to write layout to args.txt", e);
        }
    }

    static final class Th {
        final String id, label, hint;
        final int laneKind, laneNice, clusterKind, clusterNice;
        /** Kinds and nice values = kLanes / kClusters in android_thread_pinning.cpp. */
        Th(String id, String label, String hint, int laneKind, int laneNice, int clusterKind, int clusterNice) {
            this.id = id; this.label = label; this.hint = hint;
            this.laneKind = laneKind; this.laneNice = laneNice;
            this.clusterKind = clusterKind; this.clusterNice = clusterNice;
        }
        int kind(int layout) {
            switch (layout) {
                case L_LANES:
                    return laneKind;
                case L_LANES2:
                    if ("main_xthread".equals(id)) return K_BIG;
                    return laneKind;
                case L_CLUSTERS:
                    return clusterKind;
                default:
                    return K_NONE;     // priority / off: no masks
            }
        }
        int nice(int layout) {
            switch (layout) {
                case L_LANES:
                    return laneNice;
                case L_LANES2:
                    if ("render_guest".equals(id)) return -10;
                    if ("rage_jobs".equals(id) || "rage_physics".equals(id)) return -6;
                    return laneNice;
                case L_PRIORITY:
                    return laneNice;
                case L_CLUSTERS:
                    return clusterNice;
                default:
                    return KEEP;       // off: priorities untouched
            }
        }
    }

    static final class Group {
        final String id, label;
        final Th[] threads;
        Group(String id, String label, Th... threads) {
            this.id = id; this.label = label; this.threads = threads;
        }
    }

    /** Ids = kIds, kinds/nice = kLanes and kClusters in android_thread_pinning.cpp. */
    static final Group[] GROUPS = {
        new Group("render", "Render (main threads)",
            new Th("render_guest", "RenderGuest", "Game render thread, builds every frame",
                    K_BIG_PRIME, -8, K_BIG_PRIME, -6),
            new Th("recorder", "GtaRecorder", "Records the frame for Vulkan",
                    K_RECORDER_LANE, -10, K_BIG_PRIME, -8),
            new Th("render_work", "GtaRenderWork", "Prepares draw state for the recorder",
                    K_BIG, -8, K_BIG, -4),
            new Th("main_xthread", "Main XThread", "Game logic main thread",
                    K_BIG_PRIME, -8, K_BIG_PRIME, -6),
            new Th("sdl_thread", "SDLThread (main)", "SDL main thread: UI and frame presentation",
                    K_BIG, -4, K_BIG, -2),
            new Th("sdl_helpers", "SDLThread (children)",
                    "Threads started by SDLThread that kept its name (8-9 of them)",
                    K_LITTLE, KEEP, K_LITTLE, KEEP)),
        new Group("audio", "Audio",
            new Th("audio_guest", "AudioGuest", "Game audio mixer",
                    K_AUDIO_LANE, -16, K_BIG, -8),
            new Th("host_audio", "AudioTrack / SDLAudio", "Android audio output (set by AAudio, never pinned)",
                    K_NONE, KEEP, K_NONE, KEEP),
            new Th("xma_decoder", "XMA Decoder", "Decodes compressed sounds",
                    K_AUDIO_LANE, -16, K_WORKERS, -16),
            new Th("audio_worker", "Audio Worker", "Dispatches XAudio frame callbacks",
                    K_AUDIO_LANE, -16, K_WORKERS, -16)),
        new Group("world", "World streaming & file IO",
            new Th("rage_stream", "RageStream", "Loads the city around you",
                    K_LITTLE, 2, K_WORKERS, 0),
            new Th("rage_fileio", "RageFileIO", "Game file reads",
                    K_LITTLE, KEEP, K_LITTLE, KEEP),
            new Th("kernel_io", "Kernel IO / host tasks", "Emulated Xbox kernel workers",
                    K_LITTLE, -2, K_WORKERS, -2)),
        new Group("jobs", "Game jobs & physics",
            new Th("rage_jobs", "RageJobWork", "Animation, AI and other jobs",
                    K_JOBS_LANE, -2, K_LITTLE_BIG, -2),
            new Th("rage_physics", "RagePhysics", "Physics",
                    K_JOBS_LANE, -6, K_LITTLE_BIG, -2),
            new Th("rage_sim", "Rage_1BA088 (sim)", "Physics and vehicle simulation worker",
                    K_JOBS_LANE, -6, K_WORKERS, 0),
            new Th("rage_other", "Other Rage / XThread", "Remaining game threads",
                    K_LITTLE, 0, K_WORKERS, 0)),
        new Group("background", "Background",
            new Th("cmd_retirer", "GtaCmdRetirer", "Frees finished frame data",
                    K_LITTLE, 0, K_WORKERS, 0),
            new Th("recorder_helpers", "Shader compile helpers", "Child threads of the recorder",
                    K_LITTLE, 0, K_WORKERS, -2),
            new Th("sdl_activity", "SDLActivity", "Android UI thread",
                    K_LITTLE, KEEP, K_LITTLE, KEEP),
            new Th("background", "Timers / binder / misc", "System helper threads",
                    K_LITTLE, KEEP, K_LITTLE, KEEP)),
    };

    private static final String[] HEAVY_RENDER = {"render_guest", "recorder", "render_work", "main_xthread"};

    // ------------------------------------------------------------------ launch spec

    /** Called by LauncherActivity when it starts the game. */
    static String buildLaunchSpec(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (prefs.getBoolean(KEY_AUTO, true)) return "auto";
        Map<String, Integer> manual = readManual(prefs);
        if (manual.isEmpty()) return "auto";
        Topology topo = detectTopology();
        StringBuilder sb = new StringBuilder("manual");
        int entries = 0;
        for (Group g : GROUPS) {
            Integer groupMask = manual.get("g." + g.id);
            for (Th th : g.threads) {
                Integer m = groupMask != null ? groupMask : manual.get("t." + th.id);
                if (m == null) continue;
                int mask = m & topo.all();
                if (mask == 0) continue;            // nothing selected -> automatic placement
                sb.append(';').append(th.id).append(':').append(Integer.toHexString(mask));
                entries++;
            }
        }
        return entries == 0 ? "auto" : sb.toString();
    }

    /** Short line for the button in the performance dialog. */
    static String summary(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        int lIdx = readLayout(context);
        String layout = (lIdx >= 0 && lIdx < LAYOUT_NAMES.length) ? LAYOUT_NAMES[lIdx] : "lanes";
        if (prefs.getBoolean(KEY_AUTO, true)) return "CPU thread placement: Automatic (" + layout + ")";
        int n = readManual(prefs).size();
        return n == 0 ? "CPU thread placement: Automatic (" + layout + ")"
                : "CPU thread placement: Manual (" + n + "), rest " + layout;
    }

    static Map<String, Integer> readManual(SharedPreferences prefs) {
        Map<String, Integer> out = new HashMap<>();
        try {
            JSONObject o = new JSONObject(prefs.getString(KEY_MANUAL, "{}"));
            for (Iterator<String> it = o.keys(); it.hasNext(); ) {
                String k = it.next();
                out.put(k, o.getInt(k));
            }
       } catch (Exception e) {
    android.util.Log.e("LibertyRecomp", "affinity_manual JSON broken: "
            + prefs.getString(KEY_MANUAL, "{}"), e);
}
        return out;
    }

    // ------------------------------------------------------------------ dialog

    private final Context ctx;
    private final SharedPreferences prefs;
    private final Topology topo;
    private int layout;          // android_thread_layout the game will start with
    private final Runnable onSaved;
    private boolean auto;
    private final Map<String, Integer> manual;
    private boolean updating;

    private Switch autoSwitch;
    private TextView warnings;
    private final List<Row> rows = new ArrayList<>();

    private final class Row {
        final Group group;   // header row: th == null
        final Th th;
        final String key;
        CheckBox manualBox;
        TextView label;
        LinearLayout coreStrip;
        CheckBox[] cores;
        Row(Group group, Th th) {
            this.group = group;
            this.th = th;
            this.key = th == null ? "g." + group.id : "t." + th.id;
        }
    }

    ThreadAffinityDialog(Context context, Runnable onSaved) {
        this.ctx = context;
        this.onSaved = onSaved;
        this.prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        this.topo = detectTopology();
        this.layout = readLayout(context);
        this.auto = prefs.getBoolean(KEY_AUTO, true);
        this.manual = readManual(prefs);
    }

    void show() {
        final Dialog dialog = new Dialog(ctx);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);

        LinearLayout root = new LinearLayout(ctx);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(14), dp(18), dp(14));
        root.setBackground(rounded(color(R.color.picker_card), dp(10)));

        TextView title = text("CPU thread placement", 18, color(R.color.picker_text));
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);
        root.addView(text(topologyLine(), 12, color(R.color.picker_muted)), matchWrap(dp(2)));

        autoSwitch = new Switch(ctx);
        autoSwitch.setText("Automatic placement (recommended)");
        autoSwitch.setTextColor(color(R.color.picker_text));
        autoSwitch.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        autoSwitch.setPadding(dp(6), dp(8), dp(6), dp(8));
        autoSwitch.setBackground(focusBackground(0));
        autoSwitch.setFocusable(true);
        autoSwitch.setChecked(auto);
        autoSwitch.setOnCheckedChangeListener((b, checked) -> {
            if (updating) return;
            auto = checked;
            refresh();
        });
        root.addView(autoSwitch, matchWrap(dp(8)));
        root.addView(text("Manual: tick \"Manual\" on a group or a single thread and choose its cores. "
                + "Anything left unticked keeps the automatic placement.", 12, color(R.color.picker_muted)),
            matchWrap(dp(2)));

        LinearLayout list = new LinearLayout(ctx);
        list.setOrientation(LinearLayout.VERTICAL);
        list.addView(coreHeader(), matchWrap(dp(8)));
        for (Group g : GROUPS) {
            Row header = new Row(g, null);
            list.addView(buildRow(header), matchWrap(dp(10)));
            for (Th th : g.threads) {
                Row r = new Row(g, th);
                list.addView(buildRow(r), matchWrap(dp(2)));
            }
        }
        ScrollView scroll = new ScrollView(ctx);
        scroll.addView(list);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1f));

        warnings = text("", 12, WARN_COLOR);
        warnings.setVisibility(View.GONE);
        root.addView(warnings, matchWrap(dp(6)));

        LinearLayout buttons = new LinearLayout(ctx);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER_VERTICAL);

        Button layoutBtn = button(layoutButtonLabel(), false);
        layoutBtn.setPadding(dp(12), dp(0), dp(12), dp(0));
        layoutBtn.setOnClickListener(v -> {
            showLayoutPicker(() -> {
                layoutBtn.setText(layoutButtonLabel());
                refresh();
                if (onSaved != null) onSaved.run();
            });
        });
        LinearLayout.LayoutParams layoutBtnParams =
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44));
        buttons.addView(layoutBtn, layoutBtnParams);

        View spacer = new View(ctx);
        buttons.addView(spacer, new LinearLayout.LayoutParams(0, 0, 1f));

        Button reset = button(tr("btn_reset", "Reset"), false);
        Button cancel = button(tr("btn_cancel", "Cancel"), false);
        Button save = button(tr("btn_save", "Save"), true);
        LinearLayout.LayoutParams lp1 = new LinearLayout.LayoutParams(dp(110), dp(44));
        LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(dp(110), dp(44));
        lp2.leftMargin = dp(8);
        LinearLayout.LayoutParams lp3 = new LinearLayout.LayoutParams(dp(110), dp(44));
        lp3.leftMargin = dp(8);
        buttons.addView(reset, lp1);
        buttons.addView(cancel, lp2);
        buttons.addView(save, lp3);
        root.addView(buttons, matchWrap(dp(10)));

        reset.setOnClickListener(v -> {
            auto = true;
            manual.clear();
            refresh();
        });
        cancel.setOnClickListener(v -> dialog.dismiss());
        save.setOnClickListener(v -> {
            JSONObject o = new JSONObject();
            try {
                for (Map.Entry<String, Integer> e : manual.entrySet()) o.put(e.getKey(), e.getValue());
            } catch (Exception ignored) { }
            prefs.edit()
    .putBoolean(KEY_AUTO, auto)
    .putString(KEY_MANUAL, o.toString())  // JSONObject и так в одну строку
    .commit();  // сразу на диск, без гонки с Play
            dialog.dismiss();
            if (onSaved != null) onSaved.run();
        });

        dialog.setContentView(root);
        Window w = dialog.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(0));
            DisplayMetrics m = ctx.getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(760), (int) (m.widthPixels * 0.95f)), (int) (m.heightPixels * 0.95f));
        }
        refresh();
        dialog.show();
        autoSwitch.requestFocus();
    }

    private String tr(String key, String fallbackEn) {
        return LauncherI18n.tr(ctx, key, fallbackEn);
    }

    private String topologyLine() {
        StringBuilder sb = new StringBuilder();
        sb.append(topo.count).append(" cores: ");
        appendCluster(sb, "Little", topo.little);
        sb.append("  \u00B7  ");
        appendCluster(sb, "Big", topo.big);
        sb.append("  \u00B7  ");
        appendCluster(sb, "Prime", topo.prime);
        if (topo.fallback) sb.append("  (guessed: the system did not report core sizes)");
        return sb.toString();
    }

    private String layoutButtonLabel() {
        boolean isRu = LauncherI18n.isRu(ctx);
        String name = (layout >= 0 && layout < LAYOUT_NAMES.length) ? LAYOUT_NAMES[layout] : "lanes";
        return isRu ? ("Профиль раскладки CPU: " + name + " ▾")
                    : ("CPU Layout Profile: " + name + " ▾");
    }

    private void showLayoutPicker(Runnable onChange) {
        boolean isRu = LauncherI18n.isRu(ctx);
        final String[] items = isRu ? new String[] {
                "lanes (По умолчанию: рендер на Prime/Big, звук на Big)",
                "clusters (По кластерам: задачи по типам ядер)",
                "priority (Только nice: без масок ядер)",
                "off (Без масок: решает Android)",
                "lanes2 (Main на Big, RenderGuest nice -10, Jobs nice -6)"
        } : new String[] {
                "lanes (Default: Prime/Big render, Big audio)",
                "clusters (Per-cluster: tasks assigned by core type)",
                "priority (Nice only: no core masks)",
                "off (Off: Android scheduler decides)",
                "lanes2 (Main on Big, RenderGuest nice -10, Jobs nice -6)"
        };

        int checkedItem = (layout >= 0 && layout < items.length) ? layout : 0;

        new AlertDialog.Builder(ctx)
                .setTitle(isRu ? "Выберите профиль раскладки CPU" : "Select CPU Layout Profile")
                .setSingleChoiceItems(items, checkedItem, (dialog, which) -> {
                    layout = which;
                    writeLayout(ctx, layout);
                    if (onChange != null) onChange.run();
                    dialog.dismiss();
                })
                .setNegativeButton(tr("btn_cancel", "Cancel"), null)
                .show();
    }

    private String layoutLine() {
        boolean isRu = LauncherI18n.isRu(ctx);
        switch (layout) {
            case L_LANES2:
                return isRu ? "Авто-раскладка: lanes2 (Main на Big, RenderGuest nice -10, Jobs nice -6, Prime разгружен под рендер). "
                                + "Задаётся " + LAYOUT_ARG + " в args.txt."
                            : "Automatic layout: lanes2 (Main on Big, RenderGuest nice -10, Jobs nice -6). "
                                + "Set by " + LAYOUT_ARG + " in args.txt.";
            case L_CLUSTERS:
                return isRu ? "Авто-раскладка: clusters (по типам ядер: Big/Prime/Little). Задаётся "
                                + LAYOUT_ARG + " в args.txt / native.toml."
                            : "Automatic layout: clusters (previous per-cluster table). Set by "
                                + LAYOUT_ARG + " in args.txt / native.toml.";
            case L_PRIORITY:
                return isRu ? "Авто-раскладка: priority (только nice, маски сняты — Android сам распределяет потоки). Задаётся "
                                + LAYOUT_ARG + " в args.txt / native.toml."
                            : "Automatic layout: priority (nice values only, no core masks: Android picks the "
                                + "cores). Set by " + LAYOUT_ARG + " in args.txt / native.toml.";
            case L_OFF:
                return isRu ? "Авто-раскладка: off (полностью отключено, планировщик Android решает всё сам). Задаётся "
                                + LAYOUT_ARG + " в args.txt / native.toml."
                            : "Automatic layout: off (Android decides; only manual entries are applied, "
                                + "without nice). Set by " + LAYOUT_ARG + " in args.txt / native.toml.";
            default:
                return isRu ? "Авто-раскладка: lanes (по умолчанию: рендер на Prime/Big, звук на Big с nice -16). Задаётся "
                                + LAYOUT_ARG + " в args.txt."
                            : "Automatic layout: lanes (recorder on prime + top big, audio on the other big "
                                + "cores at nice -16). Change with " + LAYOUT_ARG + " in args.txt.";
        }
    }

    private void appendCluster(StringBuilder sb, String name, int mask) {
        sb.append(name).append(' ');
        int first = -1, last = -1, khz = 0;
        for (int cpu = 0; cpu < topo.count; cpu++) {
            if ((mask & (1 << cpu)) == 0) continue;
            if (first < 0) first = cpu;
            last = cpu;
            khz = Math.max(khz, topo.maxKhz[cpu]);
        }
        if (first < 0) { sb.append('-'); return; }
        sb.append(first == last ? String.valueOf(first) : first + "\u2013" + last);
        if (khz > 0) sb.append(String.format(java.util.Locale.US, " (%.2f GHz)", khz / 1e6));
    }

    private View coreHeader() {
        LinearLayout line = new LinearLayout(ctx);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.CENTER_VERTICAL);
        line.addView(new View(ctx), new LinearLayout.LayoutParams(dp(92), 1));
        line.addView(text("Thread", 12, color(R.color.picker_muted)), new LinearLayout.LayoutParams(0, -2, 1f));
        for (int cpu = 0; cpu < topo.count; cpu++) {
            String kind = topo.kindOf(cpu);
            TextView t = text(kind + cpu, 12, "P".equals(kind) ? PRIME_COLOR : "B".equals(kind) ? BIG_COLOR : LITTLE_COLOR);
            t.setGravity(Gravity.CENTER);
            t.setTypeface(Typeface.DEFAULT_BOLD);
            line.addView(t, new LinearLayout.LayoutParams(dp(40), -2));
        }
        return line;
    }

    private View buildRow(final Row r) {
        LinearLayout line = new LinearLayout(ctx);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.CENTER_VERTICAL);
        if (r.th == null) {
            line.setBackground(rounded(0x22FFFFFF, dp(6)));
            line.setPadding(0, dp(4), 0, dp(4));
        }

        r.manualBox = new CheckBox(ctx);
        r.manualBox.setText("Manual");
        r.manualBox.setTextColor(color(R.color.picker_text));
        r.manualBox.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        r.manualBox.setBackground(focusBackground(0));
        r.manualBox.setFocusable(true);
        r.manualBox.setOnCheckedChangeListener((b, checked) -> {
            if (updating) return;
            if (checked) {
                // start from what the thread/group gets right now, so ticking changes nothing yet
                // (a thread the layout leaves unpinned starts from every core)
                int mask = currentMask(r);
                manual.put(r.key, mask != 0 ? mask : topo.all());
            } else {
                manual.remove(r.key);
            }
            refresh();
        });
        line.addView(r.manualBox, new LinearLayout.LayoutParams(dp(92), -2));

        r.label = new TextView(ctx);
        r.label.setTextSize(TypedValue.COMPLEX_UNIT_SP, r.th == null ? 14 : 13);
        if (r.th == null) r.label.setTypeface(Typeface.DEFAULT_BOLD);
        r.label.setPadding(dp(4), 0, dp(4), 0);
        line.addView(r.label, new LinearLayout.LayoutParams(0, -2, 1f));

        r.coreStrip = new LinearLayout(ctx);
        r.coreStrip.setOrientation(LinearLayout.HORIZONTAL);
        r.cores = new CheckBox[topo.count];
        for (int cpu = 0; cpu < topo.count; cpu++) {
            final int bit = 1 << cpu;
            CheckBox c = new CheckBox(ctx);
            c.setBackground(focusBackground(0));
            c.setFocusable(true);
            c.setContentDescription("Core " + cpu);
            c.setOnCheckedChangeListener((b, checked) -> {
                if (updating) return;
                Integer m = manual.get(r.key);
                int mask = m == null ? 0 : m;
                mask = checked ? (mask | bit) : (mask & ~bit);
                manual.put(r.key, mask);
                refresh();
            });
            r.cores[cpu] = c;
            r.coreStrip.addView(c, new LinearLayout.LayoutParams(dp(40), dp(40)));
        }
        line.addView(r.coreStrip);
        rows.add(r);
        return line;
    }

    private boolean groupManual(Group g) {
        return !auto && manual.containsKey("g." + g.id);
    }

    /** Mask the native side will actually use for this thread (0 = nothing selected). */
    private int effectiveMask(Group g, Th th) {
        int automatic = autoMask(th.kind(layout), topo);
        if (auto) return automatic;
        Integer gm = manual.get("g." + g.id);
        if (gm != null) return gm;
        Integer tm = manual.get("t." + th.id);
        if (tm != null) return tm;
        return automatic;
    }

    /** "nice -16", "nice kept" — what the native side sets with the current layout. */
    private String niceText(Th th) {
        int n = th.nice(layout);
        return n == KEEP ? "nice kept" : "nice " + n;
    }

    private int currentMask(Row r) {
        if (r.th != null) return effectiveMask(r.group, r.th);
        int union = 0;
        for (Th th : r.group.threads) union |= effectiveMask(r.group, th);
        return union;
    }

    private void refresh() {
        updating = true;
        autoSwitch.setChecked(auto);
        Set<String> flagged = new LinkedHashSet<>();
        List<String> notes = auto ? new ArrayList<>() : evaluate(flagged);

        for (Row r : rows) {
            boolean gManual = groupManual(r.group);
            if (r.th == null) {
                r.manualBox.setChecked(gManual);
                setEnabled(r.manualBox, !auto);
                boolean warn = flagged.contains(r.key);
                r.label.setText(r.group.label + (gManual ? "  \u2014 whole group" : ""));
                r.label.setTextColor(warn ? WARN_COLOR : color(R.color.picker_text));
                r.coreStrip.setVisibility(gManual ? View.VISIBLE : View.INVISIBLE);
                int mask = gManual ? manual.get(r.key) : 0;
                for (int cpu = 0; cpu < topo.count; cpu++) {
                    r.cores[cpu].setChecked((mask & (1 << cpu)) != 0);
                    setEnabled(r.cores[cpu], gManual);
                }
            } else {
                boolean own = !auto && !gManual && manual.containsKey(r.key);
                r.manualBox.setChecked(!auto && (gManual || own));
                setEnabled(r.manualBox, !auto && !gManual);
                int mask = effectiveMask(r.group, r.th);
                String state = auto ? "auto" : gManual ? "group" : own ? "manual" : "auto";
                if (mask == 0 && "auto".equals(state)) {
                    boolean isRu = LauncherI18n.isRu(ctx);
                    state = isRu ? "авто: решает Android (без масок)" : "auto: Android decides (no masks)";
                }
                boolean warn = flagged.contains(r.key);
                r.label.setText(r.th.label + "  \u00B7  " + state + "  \u00B7  " + niceText(r.th)
                        + "\n" + r.th.hint);
                r.label.setTextColor(warn ? WARN_COLOR
                    : own ? color(R.color.picker_text) : color(R.color.picker_muted));
                for (int cpu = 0; cpu < topo.count; cpu++) {
                    r.cores[cpu].setChecked((mask & (1 << cpu)) != 0);
                    setEnabled(r.cores[cpu], own);
                }
            }
        }
        if (notes.isEmpty()) {
            warnings.setVisibility(View.GONE);
        } else {
            StringBuilder sb = new StringBuilder();
            for (String n : notes) sb.append(sb.length() == 0 ? "\u26A0 " : "\n\u26A0 ").append(n);
            warnings.setText(sb.toString());
            warnings.setVisibility(View.VISIBLE);
        }
        updating = false;
    }

    // ------------------------------------------------------------------ hidden recommendations
    // Never shown as a list. Each rule only produces a short note and marks the offending rows
    // yellow when the user's manual choice matches it. Nothing is blocked.

    private List<String> evaluate(Set<String> flagged) {
        List<String> notes = new ArrayList<>();
        Map<String, Integer> eff = new HashMap<>();
        Map<String, Row> rowOf = new HashMap<>();
        for (Row r : rows) if (r.th != null) {
            eff.put(r.th.id, effectiveMask(r.group, r.th));
            rowOf.put(r.th.id, r);
        }

        // R0: manual entry with no cores -> native keeps the automatic placement
        for (Map.Entry<String, Integer> e : manual.entrySet()) {
            if ((e.getValue() & topo.all()) == 0) {
                flagged.add(e.getKey());
                notes.add(nameOf(e.getKey()) + ": no cores selected, it will use automatic placement.");
            }
        }
        // R1: heavy render threads only on little cores
        List<String> onLittle = new ArrayList<>();
        for (String id : HEAVY_RENDER) {
            int m = eff.get(id);
            if (m != 0 && (m & ~topo.little) == 0) { onLittle.add(id); flag(flagged, rowOf.get(id)); }
        }
        if (!onLittle.isEmpty())
            notes.add(join(onLittle) + " on little cores only: expect a large FPS drop and stutter.");

        // R2: RenderGuest and GtaRecorder squeezed onto the same single core
        int rg = eff.get("render_guest"), rec = eff.get("recorder");
        if (Integer.bitCount(rg) == 1 && rg == rec) {
            flag(flagged, rowOf.get("render_guest"));
            flag(flagged, rowOf.get("recorder"));
            notes.add("RenderGuest and GtaRecorder share one core: they work as a pipeline and will wait for each other.");
        }
        // R3: the four heavy render threads on fewer than 3 cores in total
        int union = 0;
        for (String id : HEAVY_RENDER) union |= eff.get(id);
        if (union != 0 && Integer.bitCount(union) < 3) {
            for (String id : HEAVY_RENDER) flag(flagged, rowOf.get(id));
            notes.add("Main render threads squeezed onto " + Integer.bitCount(union)
                + " core(s): they need at least 3.");
        }
        // R4: game audio mixer only on little cores
        int ag = eff.get("audio_guest");
        if (ag != 0 && (ag & ~topo.little) == 0) {
            flag(flagged, rowOf.get("audio_guest"));
            notes.add("AudioGuest on little cores only: sound may crackle or drop out under load.");
        }
        // R5: presenter only on little cores
        int sdl = eff.get("sdl_thread");
        if (sdl != 0 && (sdl & ~topo.little) == 0) {
            flag(flagged, rowOf.get("sdl_thread"));
            notes.add("SDLThread on little cores only: finished frames can reach the screen late.");
        }
        // R6: background / world workers pushed onto the prime core only, next to RenderGuest
        if ((rg & topo.prime) != 0) {
            for (String id : new String[] {"rage_stream", "rage_jobs", "rage_physics", "rage_other",
                                           "cmd_retirer", "xma_decoder"}) {
                int m = eff.get(id);
                if (m != 0 && (m & ~topo.prime) == 0) {
                    flag(flagged, rowOf.get(id));
                    notes.add(rowOf.get(id).th.label + " only on the prime core: it takes time from RenderGuest.");
                }
            }
        }
        // R7: everything heavy on one cluster while another cluster is left empty
        int heavyAll = union | ag | eff.get("rage_jobs") | eff.get("rage_stream");
        if (heavyAll != 0 && (heavyAll & ~topo.big) == 0 && Integer.bitCount(topo.big) <= 3) {
            notes.add("All heavy threads on the big cores only: the prime and little cores stay idle.");
        }
        // group header rows inherit the mark of their members
        for (Row r : rows) if (r.th != null && flagged.contains(r.key) && groupManual(r.group))
            flagged.add("g." + r.group.id);
        return notes;
    }

    private static void flag(Set<String> flagged, Row r) {
        if (r != null) flagged.add(r.key);
    }

    private String nameOf(String key) {
        for (Group g : GROUPS) {
            if (key.equals("g." + g.id)) return g.label;
            for (Th th : g.threads) if (key.equals("t." + th.id)) return th.label;
        }
        return key;
    }

    private String join(List<String> ids) {
        StringBuilder sb = new StringBuilder();
        for (String id : ids) {
            if (sb.length() > 0) sb.append(", ");
            sb.append(nameOf("t." + id));
        }
        return sb.toString();
    }

    // ------------------------------------------------------------------ small UI helpers
    // Copies of LauncherActivity's private helpers so this class stays self-contained.

    private static void setEnabled(View v, boolean enabled) {
        v.setEnabled(enabled);
        v.setFocusable(enabled);
        v.setAlpha(enabled ? 1f : 0.45f);
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
            ctx.getResources().getDisplayMetrics()));
    }

    private int color(int id) {
        return ctx.getResources().getColor(id, ctx.getTheme());
    }

    private TextView text(String s, int sp, int color) {
        TextView t = new TextView(ctx);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    private LinearLayout.LayoutParams matchWrap(int topMargin) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = topMargin;
        return lp;
    }

    private Button button(String s, boolean primary) {
        Button b = new Button(ctx);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(primary ? 0xFF000000 : color(R.color.picker_text));
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(focusBackground(primary ? 0xFFE6E6E6 : 0xFF30363D));
        b.setFocusable(true);
        return b;
    }

    private static GradientDrawable rounded(int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius);
        return d;
    }

    private StateListDrawable focusBackground(int color) {
        GradientDrawable normal = rounded(color, dp(8));
        GradientDrawable focused = rounded(color, dp(8));
        focused.setStroke(dp(3), 0xFF3FA9F5);
        GradientDrawable pressed = rounded(color == 0 ? 0x33FFFFFF : color, dp(8));
        pressed.setStroke(dp(3), 0xFFFFFFFF);
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] {android.R.attr.state_pressed}, pressed);
        s.addState(new int[] {android.R.attr.state_focused}, focused);
        s.addState(new int[0], normal);
        return s;
    }

    private static List<String> readLines(File f) {
        List<String> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) out.add(line);
        } catch (Exception ignored) {
            // missing file: defaults
        }
        return out;
    }

    private static String readFirstLine(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            return r.readLine();
        } catch (Exception e) {
            return null;
        }
    }

    private static int parseInt(String s) {
        if (s == null) return 0;
        try { return Integer.parseInt(s.trim()); } catch (RuntimeException e) { return 0; }
    }
}