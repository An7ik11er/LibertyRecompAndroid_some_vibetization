package com.libertyrecomp;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Dialog for configuring game engine cvars and gameplay tweaks from args.txt:
 * traffic, ped density, shadows, queued frames, FPS cap, sync modes, etc.
 * Uses checkboxes for booleans and SeekBars for numeric parameters.
 */
final class ArgsTweaksDialog {
    private static final String TAG = "LibertyArgsTweaks";

    // Managed cvar keys from args.txt
    private static final String KEY_TRAFFIC_DENSITY = "gta4_traffic_density_scale";
    private static final String KEY_PARKED_DENSITY = "gta4_parked_car_density_scale";
    private static final String KEY_PED_DENSITY = "gta4_ped_density_scale";
    private static final String KEY_SCENARIO_PED_DENSITY = "gta4_scenario_ped_density_scale";

    private static final String KEY_SHADOW_DIST = "gta4_shadow_distance_scale";
    private static final String KEY_SHADOW_MIN_INDICES = "gta4_native_shadow_min_indices";
    private static final String KEY_DYNAMIC_DRAW = "gta4_dynamic_draw_distance";
    private static final String KEY_DYNAMIC_DRAW_MIN = "gta4_dynamic_draw_distance_min";

    private static final String KEY_FPS_GUARD = "gta4_fps_guard";
    private static final String KEY_MAX_QUEUED_FRAMES = "gta4_native_max_queued_frames";
    private static final String KEY_FRAME_COMPLETE_SIGNAL = "gta4_native_frame_complete_signal";
    private static final String KEY_ASYNC_CONSTANTS = "gta4_native_async_constant_release";
    private static final String KEY_VULKAN_MAILBOX = "vulkan_allow_present_mode_mailbox";
    private static final String KEY_VULKAN_FIFO = "vulkan_prefer_present_mode_fifo";
    private static final String KEY_VULKAN_PREROTATE = "vulkan_prerotate";
    private static final String KEY_JOB_HELP = "job_help";

    private static final String KEY_AUDIO_MAXQ = "audio_maxqframes";
    private static final String KEY_PERF_HINT = "android_perf_hint";
    private static final String KEY_PIN_HOT_THREADS = "android_pin_hot_threads";

    // Defaults matching default_args.txt
    private static final Map<String, String> DEFAULTS = new LinkedHashMap<>();
    static {
        DEFAULTS.put(KEY_TRAFFIC_DENSITY, "0.4");
        DEFAULTS.put(KEY_PARKED_DENSITY, "0.4");
        DEFAULTS.put(KEY_PED_DENSITY, "0.4");
        DEFAULTS.put(KEY_SCENARIO_PED_DENSITY, "0.4");

        DEFAULTS.put(KEY_SHADOW_DIST, "1.0");
        DEFAULTS.put(KEY_SHADOW_MIN_INDICES, "150");
        DEFAULTS.put(KEY_DYNAMIC_DRAW, "false");
        DEFAULTS.put(KEY_DYNAMIC_DRAW_MIN, "0.80");

        DEFAULTS.put(KEY_FPS_GUARD, "0");
        DEFAULTS.put(KEY_MAX_QUEUED_FRAMES, "2");
        DEFAULTS.put(KEY_FRAME_COMPLETE_SIGNAL, "1");
        DEFAULTS.put(KEY_ASYNC_CONSTANTS, "true");
        DEFAULTS.put(KEY_VULKAN_MAILBOX, "true");
        DEFAULTS.put(KEY_VULKAN_FIFO, "false");
        DEFAULTS.put(KEY_VULKAN_PREROTATE, "1");
        DEFAULTS.put(KEY_JOB_HELP, "true");

        DEFAULTS.put(KEY_AUDIO_MAXQ, "64");
        DEFAULTS.put(KEY_PERF_HINT, "false");
        DEFAULTS.put(KEY_PIN_HOT_THREADS, "false");
    }

    private final Context ctx;
    private final Runnable onSaved;
    private final Map<String, String> currentValues = new LinkedHashMap<>();

    interface IntConsumer {
        void accept(int val);
    }

    interface BoolConsumer {
        void accept(boolean val);
    }

    ArgsTweaksDialog(Context context, Runnable onSaved) {
        this.ctx = context;
        this.onSaved = onSaved;
        loadValues();
    }

    static String summary(Context context) {
        return LauncherI18n.tr(context, "args_summary", "args.txt Tweaks & Settings");
    }

    private String tr(String key, String fallbackEn) {
        return LauncherI18n.tr(ctx, key, fallbackEn);
    }

    private File getArgsFile() {
        File external = ctx.getExternalFilesDir(null);
        File root = external != null ? external : ctx.getFilesDir();
        return new File(root, "args.txt");
    }

    private void loadValues() {
        currentValues.clear();
        currentValues.putAll(DEFAULTS);

        File f = getArgsFile();
        if (!f.exists()) {
            // First time: read bundled default_args.txt
            try (BufferedReader r = new BufferedReader(new InputStreamReader(ctx.getAssets().open("default_args.txt")))) {
                String line;
                while ((line = r.readLine()) != null) parseLine(line);
            } catch (Exception ignored) {}
            return;
        }

        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) parseLine(line);
        } catch (Exception ignored) {}
    }

    private void parseLine(String line) {
        String t = line.trim();
        if (t.startsWith("--")) {
            int eq = t.indexOf('=');
            if (eq > 2) {
                String key = t.substring(2, eq).trim();
                String val = t.substring(eq + 1).trim();
                if (DEFAULTS.containsKey(key)) {
                    currentValues.put(key, val);
                }
            }
        }
    }

    private void saveValues() {
        File f = getArgsFile();
        List<String> lines = new ArrayList<>();
        if (f.exists()) {
            try (BufferedReader r = new BufferedReader(new FileReader(f))) {
                String line;
                while ((line = r.readLine()) != null) lines.add(line);
            } catch (Exception ignored) {}
        }

        Set<String> handled = new HashSet<>();
        List<String> output = new ArrayList<>();

        for (String line : lines) {
            String t = line.trim();
            if (t.startsWith("--")) {
                int eq = t.indexOf('=');
                if (eq > 2) {
                    String key = t.substring(2, eq).trim();
                    if (currentValues.containsKey(key)) {
                        output.add("--" + key + "=" + currentValues.get(key));
                        handled.add(key);
                        continue;
                    }
                }
            }
            output.add(line);
        }

        // Insert unhandled keys right before the first comment after base settings or at top
        int insertPos = output.size();
        for (int i = 0; i < output.size(); ++i) {
            if (output.get(i).contains("All heavy profilers")) {
                insertPos = i;
                break;
            }
        }
        for (Map.Entry<String, String> entry : currentValues.entrySet()) {
            if (!handled.contains(entry.getKey())) {
                output.add(insertPos++, "--" + entry.getKey() + "=" + entry.getValue());
            }
        }

        try (BufferedWriter w = new BufferedWriter(new FileWriter(f))) {
            for (String l : output) {
                w.write(l);
                w.newLine();
            }
        } catch (IOException e) {
            Log.e(TAG, "Failed to write args.txt", e);
            Toast.makeText(ctx, "Error saving args.txt: " + e.getMessage(), Toast.LENGTH_SHORT).show();
            return;
        }

        Toast.makeText(ctx, tr("args_toast_saved", "args.txt settings saved"), Toast.LENGTH_SHORT).show();
    }

    private float getFloat(String key, float def) {
        String val = currentValues.get(key);
        if (val == null) return def;
        try {
            return Float.parseFloat(val);
        } catch (Exception e) {
            return def;
        }
    }

    private int getInt(String key, int def) {
        String val = currentValues.get(key);
        if (val == null) return def;
        try {
            return Integer.parseInt(val);
        } catch (Exception e) {
            return def;
        }
    }

    private boolean getBool(String key, boolean def) {
        String val = currentValues.get(key);
        if (val == null) return def;
        return Boolean.parseBoolean(val);
    }

    void show() {
        final Dialog dialog = new Dialog(ctx);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);

        LinearLayout root = new LinearLayout(ctx);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(14), dp(18), dp(14));
        root.setBackground(rounded(color(R.color.picker_card), dp(10)));

        TextView title = text(tr("args_tweaks_title", "args.txt Tweaks & Settings"), 18, color(R.color.picker_text));
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView subtitle = text(tr("args_tweaks_subtitle", "Graphics, world density, and frame queue settings from args.txt"), 12, color(R.color.picker_muted));
        root.addView(subtitle, matchWrap(dp(2)));

        LinearLayout list = new LinearLayout(ctx);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(0, 0, dp(4), 0);

        buildContent(list);

        ScrollView scroll = new ScrollView(ctx);
        scroll.addView(list);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout buttons = new LinearLayout(ctx);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.END);
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
            currentValues.clear();
            currentValues.putAll(DEFAULTS);
            list.removeAllViews();
            buildContent(list);
        });

        cancel.setOnClickListener(v -> dialog.dismiss());

        save.setOnClickListener(v -> {
            saveValues();
            dialog.dismiss();
            if (onSaved != null) onSaved.run();
        });

        dialog.setContentView(root);
        Window w = dialog.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = ctx.getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(760), (int) (m.widthPixels * 0.95f)), (int) (m.heightPixels * 0.95f));
        }
        dialog.show();
    }

    private void buildContent(LinearLayout list) {
        // Section 1: Densities
        list.addView(sectionHeader(tr("args_sec_density", "World Density & Traffic")));

        list.addView(buildSliderRow(
                tr("args_traffic_title", "Traffic Density (Ambient Vehicles)"),
                tr("args_traffic_note", "Fewer vehicles = higher FPS and less CPU load"),
                0, 100, 5, 0.01f, "",
                Math.round(getFloat(KEY_TRAFFIC_DENSITY, 0.4f) * 100),
                val -> currentValues.put(KEY_TRAFFIC_DENSITY, String.format(Locale.ROOT, "%.2f", val * 0.01f))
        ));

        list.addView(buildSliderRow(
                tr("args_parked_title", "Parked Car Density"),
                tr("args_parked_note", "Amount of cars parked on streets and parking lots"),
                0, 100, 5, 0.01f, "",
                Math.round(getFloat(KEY_PARKED_DENSITY, 0.4f) * 100),
                val -> currentValues.put(KEY_PARKED_DENSITY, String.format(Locale.ROOT, "%.2f", val * 0.01f))
        ));

        list.addView(buildSliderRow(
                tr("args_ped_title", "Pedestrian Density"),
                tr("args_ped_note", "Number of walking pedestrians on Liberty City streets"),
                0, 100, 5, 0.01f, "",
                Math.round(getFloat(KEY_PED_DENSITY, 0.4f) * 100),
                val -> currentValues.put(KEY_PED_DENSITY, String.format(Locale.ROOT, "%.2f", val * 0.01f))
        ));

        list.addView(buildSliderRow(
                tr("args_scenario_ped_title", "Scenario Pedestrian Density"),
                tr("args_scenario_ped_note", "Sitting, smoking, chatting and ambient scenario NPCs"),
                0, 100, 5, 0.01f, "",
                Math.round(getFloat(KEY_SCENARIO_PED_DENSITY, 0.4f) * 100),
                val -> currentValues.put(KEY_SCENARIO_PED_DENSITY, String.format(Locale.ROOT, "%.2f", val * 0.01f))
        ));

        // Section 2: Graphics / Shadows
        list.addView(sectionHeader(tr("args_sec_graphics", "Draw Distance & Shadows")));

        list.addView(buildSliderRow(
                tr("args_shadow_dist_title", "Shadow Draw Distance"),
                tr("args_shadow_dist_note", "Distance at which object shadows are visible"),
                0, 20, 1, 0.1f, "x",
                Math.round(getFloat(KEY_SHADOW_DIST, 1.0f) * 10),
                val -> currentValues.put(KEY_SHADOW_DIST, String.format(Locale.ROOT, "%.1f", val * 0.1f))
        ));

        list.addView(buildSliderRow(
                tr("args_shadow_indices_title", "Min Indices for Shadow Casting"),
                tr("args_shadow_indices_note", "Culls small objects (trash, posts) from shadow map; 0 = all shadows"),
                0, 500, 10, 1.0f, "",
                getInt(KEY_SHADOW_MIN_INDICES, 150),
                val -> currentValues.put(KEY_SHADOW_MIN_INDICES, String.valueOf(val))
        ));

        list.addView(buildCheckBoxRow(
                tr("args_dynamic_draw_title", "Dynamic Draw Distance"),
                tr("args_dynamic_draw_note", "Automatically reduces draw distance when FPS drops"),
                getBool(KEY_DYNAMIC_DRAW, false),
                val -> currentValues.put(KEY_DYNAMIC_DRAW, String.valueOf(val))
        ));

        list.addView(buildSliderRow(
                tr("args_dynamic_draw_min_title", "Dynamic Draw Distance Floor"),
                tr("args_dynamic_draw_min_note", "Lowest multiplier limit when dynamic draw distance is enabled"),
                10, 100, 5, 0.01f, "",
                Math.round(getFloat(KEY_DYNAMIC_DRAW_MIN, 0.80f) * 100),
                val -> currentValues.put(KEY_DYNAMIC_DRAW_MIN, String.format(Locale.ROOT, "%.2f", val * 0.01f))
        ));

        // Section 3: Queues & Framerate
        list.addView(sectionHeader(tr("args_sec_framerate", "Frame Queues & Sync")));

        list.addView(buildSliderRow(
                tr("args_queued_frames_title", "Max Queued Frames (GPU)"),
                tr("args_queued_frames_note", "1 = minimal input lag, 2-3 = smoother framerate"),
                1, 4, 1, 1.0f, "",
                getInt(KEY_MAX_QUEUED_FRAMES, 2),
                val -> currentValues.put(KEY_MAX_QUEUED_FRAMES, String.valueOf(val))
        ));

        list.addView(buildSliderRow(
                tr("args_frame_complete_title", "Frame Complete Signal"),
                tr("args_frame_complete_note", "Display sync trigger mode (0-2)"),
                0, 2, 1, 1.0f, "",
                getInt(KEY_FRAME_COMPLETE_SIGNAL, 1),
                val -> currentValues.put(KEY_FRAME_COMPLETE_SIGNAL, String.valueOf(val))
        ));

        list.addView(buildCheckBoxRow(
                tr("args_async_constant_title", "Async Constant Buffer Release"),
                tr("args_async_constant_note", "Reduces render thread stalls during state changes"),
                getBool(KEY_ASYNC_CONSTANTS, true),
                val -> currentValues.put(KEY_ASYNC_CONSTANTS, String.valueOf(val))
        ));

        list.addView(buildCheckBoxRow(
                tr("args_mailbox_title", "Mailbox Present Mode (Triple Buffering)"),
                tr("args_mailbox_note", "Tear-free presentation with lowest possible latency"),
                getBool(KEY_VULKAN_MAILBOX, true),
                val -> currentValues.put(KEY_VULKAN_MAILBOX, String.valueOf(val))
        ));

        list.addView(buildCheckBoxRow(
                tr("args_fifo_title", "Enforce FIFO Present Mode (VSync)"),
                tr("args_fifo_note", "Strict frame presentation lock to display refresh rate"),
                getBool(KEY_VULKAN_FIFO, false),
                val -> currentValues.put(KEY_VULKAN_FIFO, String.valueOf(val))
        ));

        list.addView(buildPrerotateRow(
                tr("args_prerotate_title", "Vulkan Pre-rotation (G2)"),
                tr("args_prerotate_note", "Creates swapchain in display's native orientation; bypasses SurfaceFlinger GPU composition"),
                getInt(KEY_VULKAN_PREROTATE, 1),
                val -> currentValues.put(KEY_VULKAN_PREROTATE, String.valueOf(val))
        ));

        list.addView(buildCheckBoxRow(
                tr("args_job_help_title", "Main Thread Work-Helping (C3)"),
                tr("args_job_help_note", "Main thread executes RAGE worker jobs while waiting on synchronization"),
                getBool(KEY_JOB_HELP, true),
                val -> currentValues.put(KEY_JOB_HELP, String.valueOf(val))
        ));

        // Section 4: Audio & Android System
        list.addView(sectionHeader(tr("args_sec_system", "Audio & System Options")));

        list.addView(buildSliderRow(
                tr("args_audio_qframes_title", "Audio Buffer Queue Size (Frames)"),
                tr("args_audio_qframes_note", "Lower = less audio latency; higher = crackle-free under heavy load"),
                16, 128, 8, 1.0f, "",
                getInt(KEY_AUDIO_MAXQ, 64),
                val -> currentValues.put(KEY_AUDIO_MAXQ, String.valueOf(val))
        ));

        list.addView(buildCheckBoxRow(
                tr("args_perf_hint_title", "Android Performance Hint (ADPF)"),
                tr("args_perf_hint_note", "Sends target frame timing to Android power HAL for CPU/GPU clocks"),
                getBool(KEY_PERF_HINT, false),
                val -> currentValues.put(KEY_PERF_HINT, String.valueOf(val))
        ));

        list.addView(buildCheckBoxRow(
                tr("args_pin_threads_title", "Pin Hot Threads"),
                tr("args_pin_threads_note", "Automatically pins heavy loops to performance cores"),
                getBool(KEY_PIN_HOT_THREADS, false),
                val -> currentValues.put(KEY_PIN_HOT_THREADS, String.valueOf(val))
        ));


    }

    private View sectionHeader(String title) {
        LinearLayout sec = new LinearLayout(ctx);
        sec.setOrientation(LinearLayout.VERTICAL);
        sec.setPadding(0, dp(14), 0, dp(4));

        TextView t = text(title, 14, 0xFF58A6FF);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        sec.addView(t);

        View divider = new View(ctx);
        divider.setBackgroundColor(0x3330363D);
        LinearLayout.LayoutParams dParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
        dParams.topMargin = dp(4);
        sec.addView(divider, dParams);
        return sec;
    }

    private View buildCheckBoxRow(String label, String note, boolean checked, BoolConsumer onChange) {
        CheckBox box = new CheckBox(ctx);
        android.text.SpannableStringBuilder text = new android.text.SpannableStringBuilder(label);
        text.append('\n');
        int start = text.length();
        text.append(note);
        text.setSpan(new android.text.style.RelativeSizeSpan(0.8f), start, text.length(), 0);
        text.setSpan(new android.text.style.ForegroundColorSpan(color(R.color.picker_muted)), start, text.length(), 0);
        box.setText(text);
        box.setTextColor(color(R.color.picker_text));
        box.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        box.setPadding(dp(6), dp(6), dp(6), dp(6));
        box.setBackground(focusBackground(Color.TRANSPARENT));
        box.setFocusable(true);
        box.setChecked(checked);
        box.setOnCheckedChangeListener((b, isChecked) -> onChange.accept(isChecked));

        LinearLayout.LayoutParams lp = matchWrap(dp(4));
        LinearLayout wrapper = new LinearLayout(ctx);
        wrapper.setOrientation(LinearLayout.VERTICAL);
        wrapper.addView(box, lp);
        return wrapper;
    }

    private View buildSliderRow(String title, String note, int min, int max, int step,
                                float scaleFactor, String unit, int currentValue,
                                IntConsumer onChange) {
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(dp(6), dp(6), dp(6), dp(6));
        row.setBackground(focusBackground(Color.TRANSPARENT));
        row.setFocusable(true);

        LinearLayout header = new LinearLayout(ctx);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);

        TextView titleView = text(title, 14, color(R.color.picker_text));
        titleView.setTypeface(Typeface.DEFAULT_BOLD);
        header.addView(titleView, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView valueView = text(formatValue(currentValue, scaleFactor, unit), 13, 0xFF58A6FF);
        valueView.setTypeface(Typeface.DEFAULT_BOLD);
        valueView.setGravity(Gravity.END);
        header.addView(valueView, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        row.addView(header, matchWrap(0));

        if (note != null && !note.isEmpty()) {
            TextView noteView = text(note, 11, color(R.color.picker_muted));
            row.addView(noteView, matchWrap(dp(2)));
        }

        SeekBar seekBar = new SeekBar(ctx);
        int steps = (max - min) / step;
        seekBar.setMax(steps);
        int currentProgress = Math.max(0, Math.min(steps, (currentValue - min) / step));
        seekBar.setProgress(currentProgress);
        seekBar.setFocusable(true);
        seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int val = min + progress * step;
                valueView.setText(formatValue(val, scaleFactor, unit));
                onChange.accept(val);
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });

        LinearLayout.LayoutParams sbParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sbParams.topMargin = dp(4);
        row.addView(seekBar, sbParams);

        return row;
    }

    private View buildFpsGuardRow(String title, String note, int currentValue, IntConsumer onChange) {
        int[] fpsSteps = {0, 15, 20, 24, 30, 40, 45, 60};
        int currentIdx = 0;
        for (int i = 0; i < fpsSteps.length; i++) {
            if (fpsSteps[i] == currentValue) {
                currentIdx = i;
                break;
            }
        }

        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(dp(6), dp(6), dp(6), dp(6));
        row.setBackground(focusBackground(Color.TRANSPARENT));
        row.setFocusable(true);

        LinearLayout header = new LinearLayout(ctx);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);

        TextView titleView = text(title, 14, color(R.color.picker_text));
        titleView.setTypeface(Typeface.DEFAULT_BOLD);
        header.addView(titleView, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView valueView = text(fpsLabel(currentValue), 13, 0xFF58A6FF);
        valueView.setTypeface(Typeface.DEFAULT_BOLD);
        valueView.setGravity(Gravity.END);
        header.addView(valueView, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        row.addView(header, matchWrap(0));

        if (note != null && !note.isEmpty()) {
            TextView noteView = text(note, 11, color(R.color.picker_muted));
            row.addView(noteView, matchWrap(dp(2)));
        }

        SeekBar seekBar = new SeekBar(ctx);
        seekBar.setMax(fpsSteps.length - 1);
        seekBar.setProgress(currentIdx);
        seekBar.setFocusable(true);
        seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int val = fpsSteps[progress];
                valueView.setText(fpsLabel(val));
                onChange.accept(val);
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });

        LinearLayout.LayoutParams sbParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sbParams.topMargin = dp(4);
        row.addView(seekBar, sbParams);

        return row;
    }

    private String fpsLabel(int val) {
        if (val <= 0) return tr("args_fps_guard_off", "0 (Выкл)");
        return val + " FPS";
    }

    private View buildPrerotateRow(String title, String note, int currentValue, IntConsumer onChange) {
        int[] steps = {0, 1, 2};
        int currentIdx = (currentValue >= 0 && currentValue <= 2) ? currentValue : 1;

        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(dp(6), dp(6), dp(6), dp(6));
        row.setBackground(focusBackground(Color.TRANSPARENT));
        row.setFocusable(true);

        LinearLayout header = new LinearLayout(ctx);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);

        TextView titleView = text(title, 14, color(R.color.picker_text));
        titleView.setTypeface(Typeface.DEFAULT_BOLD);
        header.addView(titleView, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView valueView = text(prerotateLabel(currentValue), 13, 0xFF58A6FF);
        valueView.setTypeface(Typeface.DEFAULT_BOLD);
        valueView.setGravity(Gravity.END);
        header.addView(valueView, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        row.addView(header, matchWrap(0));

        if (note != null && !note.isEmpty()) {
            TextView noteView = text(note, 11, color(R.color.picker_muted));
            row.addView(noteView, matchWrap(dp(2)));
        }

        SeekBar seekBar = new SeekBar(ctx);
        seekBar.setMax(steps.length - 1);
        seekBar.setProgress(currentIdx);
        seekBar.setFocusable(true);
        seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                int val = steps[progress];
                valueView.setText(prerotateLabel(val));
                onChange.accept(val);
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });

        LinearLayout.LayoutParams sbParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sbParams.topMargin = dp(4);
        row.addView(seekBar, sbParams);

        return row;
    }

    private String prerotateLabel(int val) {
        switch (val) {
            case 0: return tr("args_prerotate_0", "0 (Выкл / Legacy)");
            case 1: return tr("args_prerotate_1", "1 (Вкл / Рекомендуется)");
            case 2: return tr("args_prerotate_2", "2 (Реверс 180°)");
            default: return String.valueOf(val);
        }
    }

    private static String formatValue(int rawVal, float scaleFactor, String unit) {
        float f = rawVal * scaleFactor;
        if (scaleFactor == 1.0f) {
            return String.valueOf(rawVal) + unit;
        }
        if (scaleFactor == 0.1f) {
            return String.format(Locale.ROOT, "%.1f%s", f, unit);
        }
        return String.format(Locale.ROOT, "%.2f%s", f, unit);
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
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
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
        s.addState(new int[]{android.R.attr.state_pressed}, pressed);
        s.addState(new int[]{android.R.attr.state_focused}, focused);
        s.addState(new int[0], normal);
        return s;
    }
}
