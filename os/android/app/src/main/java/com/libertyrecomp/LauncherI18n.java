package com.libertyrecomp;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Localization helper for LauncherActivity and ThreadAffinityDialog.
 * Provides instant English <-> Russian switching with natural technical Anglicisms.
 */
final class LauncherI18n {
    private static final String PREF_NAME = "launcher_prefs";
    private static final String KEY_LANG = "launcher_lang";
    public static final String LANG_RU = "ru";
    public static final String LANG_EN = "en";

    private static String sCurrentLang = null;
    private static final Map<String, String> RU = new HashMap<>();

    static {
        // --- Status & General ---
        RU.put("btn_lang_name", "Русский");
        RU.put("status_installed", "Игра установлена. Нажмите Play.");
        RU.put("status_install_found", "Файлы игры найдены в папке install/. Нажмите «Установить» справа.");
        RU.put("status_not_installed", "Игра не установлена. Выберите образ диска и Title Update 8 справа и нажмите «Установить».");
        RU.put("status_last_launch_failed", "Сбой предыдущего запуска: ");

        // --- Crash card ---
        RU.put("crash_title", "Игра вылетела — сохранён отчёт о сбое");
        RU.put("crash_saved_prefix", "Сохранено в Android/data/");
        RU.put("crash_saved_suffix", "Нажмите «Поделиться отчётом», чтобы отправить его.");
        RU.put("crash_btn_share", "Поделиться отчётом");
        RU.put("crash_btn_dismiss", "Закрыть");
        RU.put("crash_toast_no_app", "Нет приложения для отправки. Отчёт сохранён в ");
        RU.put("crash_share_chooser", "Отправить отчёт о сбое");

        // --- Main Buttons & Footers ---
        RU.put("btn_play", "Play");
        RU.put("btn_play_debug", "Play (Debug-лог)");
        RU.put("btn_perf_settings", "Настройки производительности");
        RU.put("debug_note", "Debug-лог сохраняется в Android/data/%s/files/%s/. Прикрепите свежий файл при создании issue на GitHub.");
        RU.put("gamepad_hint", "A: выбор   B: назад   Start: запуск");
        RU.put("toast_game_busy", "Запуск...");
        RU.put("toast_install_running", "Выполняется установка игры.");
        RU.put("toast_install_first", "Сначала установите игру (панель «Игра» справа).");

        // --- Driver Card ---
        RU.put("driver_card_title", "Vulkan-драйвер");
        RU.put("driver_card_subtitle", "Применится при следующем запуске Play.");
        RU.put("driver_turnip_name", "Turnip 26.3.0-R6 (встроенный)");
        RU.put("driver_turnip_desc", "Рекомендован для Adreno 6xx/7xx");
        RU.put("driver_system_name", "Системный драйвер");
        RU.put("driver_system_desc", "Штатный Vulkan-драйвер устройства (требуется Vulkan 1.2)");
        RU.put("driver_imported_desc", "Импортированный драйвер");
        RU.put("btn_import_driver", "Импорт драйвера (.zip)");
        RU.put("btn_remove_driver", "Удалить");
        RU.put("driver_footer_note", "Поддерживаются Turnip-пакеты для AdrenoTools (zip-архив с библиотекой .so и meta.json). Для игры требуется Vulkan 1.2; если кастомный драйвер не запустится, игра откатится на системный драйвер.");
        RU.put("driver_save_error", "Не удалось сохранить выбор драйвера: ");
        RU.put("driver_removed_toast", "Драйвер удалён. Используется встроенный Turnip.");
        RU.put("driver_importing_busy", "Импорт драйвера...");
        RU.put("driver_imported_toast", "Драйвер успешно импортирован и выбран.");
        RU.put("driver_import_failed", "Ошибка импорта: ");
        RU.put("toast_no_picker", "Файловый менеджер не найден на этом устройстве.");

        // --- Installation Card ---
        RU.put("install_card_title", "Игра");
        RU.put("install_card_title_installed", "Игра установлена");
        RU.put("install_card_title_install", "Установка игры");
        RU.put("install_note_checking", "Проверка файлов игры...");
        RU.put("install_note_unavailable", "Установщик недоступен (%s). Игра установится сама при нажатии Play из папки install/.");
        RU.put("install_note_ready", "GTA IV с Title Update 8 установлена и готова к запуску.");
        RU.put("install_note_need_iso", "Требуется оригинальный образ диска (USA) и Title Update 8 (0.0.8.5).");
        RU.put("install_note_current", "\nТекущая установка: ");
        RU.put("btn_reinstall_episodes", "Переустановить или добавить эпизоды");
        RU.put("source_game", "Образ диска игры (.iso)");
        RU.put("source_update", "Title Update 8 (TU8)");
        RU.put("source_tlad", "The Lost and Damned (TLAD, опционально)");
        RU.put("source_tbogt", "The Ballad of Gay Tony (TBOGT, опционально)");
        RU.put("not_selected", "Не выбрано");
        RU.put("btn_choose_file", "Выбрать файл...");
        RU.put("btn_clear", "Очистить");
        RU.put("disc_inspecting", "Проверка образа диска...");
        RU.put("btn_install", "Установить");
        RU.put("btn_install_episodes", "Установить эпизоды");
        RU.put("btn_cancel", "Отмена");
        RU.put("btn_cancelling", "Отмена...");
        RU.put("install_footer_note", "Файлы можно выбрать из любого места памяти устройства или SD-карты (потребуется однократное разрешение). Либо скопируйте их в Android/data/%s/files/install/ — они подхватятся автоматически.");
        RU.put("install_perm_title", "Разрешить доступ к файлам");
        RU.put("install_perm_message", "Для чтения файлов игры из памяти устройства Liberty Recompiled необходим доступ ко всем файлам. Сейчас откроются настройки Android; предоставьте разрешение и вернитесь назад.\n\nЛибо вручную скопируйте файлы в папку Android/data/%s/files/install/.");
        RU.put("install_perm_open", "Открыть настройки");
        RU.put("install_toast_running", "Установка уже выполняется.");
        RU.put("installing_title", "Установка...");
        RU.put("installing_keep_open", "Не закрывайте приложение. Установка займёт несколько минут.");
        RU.put("checking_sources", "Проверка исходных файлов...");
        RU.put("install_progress_bytes", "%.1f из %.1f ГБ (%d%%)");
        RU.put("install_status_progress", "Установка игры... %d%%");
        RU.put("install_status_generic", "Установка игры...");
        RU.put("install_complete_toast", "Установка завершена. Нажмите Play.");
        RU.put("install_failed_title", "Ошибка установки");
        RU.put("btn_ok", "OK");
        RU.put("btn_save", "Сохранить");
        RU.put("btn_reset", "Сброс");
        RU.put("btn_close", "Закрыть");

        // --- Performance Test Settings ---
        RU.put("perf_title", "Настройки производительности");
        RU.put("perf_important", "Важно — прочитайте перед изменением");
        RU.put("perf_warning", "ВНИМАНИЕ! Настройки предназначены только для слабых устройств и ИСКЛЮЧИТЕЛЬНО для тестов. Графика СЛОМАЕТСЯ, и сломается сильно. Если у вас стабильные 30 FPS, пожалуйста, ничего здесь не трогайте, иначе игра превратится в картофельное месиво с кучей визуальных багов и вылетов. Баг-репорты на GitHub с включёнными твиками будут закрываться без рассмотрения. Вас предупредили, удачи:");
        RU.put("chk_draw_dist_title", "Дальность прорисовки 0.5 (Draw distance)");
        RU.put("chk_draw_dist_note", "Фиксированная половинная дальность; динамическая дальность выключена.");
        RU.put("chk_shadows_title", "Тени от солнца: выкл (Sun shadows off)");
        RU.put("chk_shadows_note", "Объекты не отбрасывают солнечные тени; shadow map не рендерится.");
        RU.put("chk_turbo_title", "GPU Turbo");
        RU.put("chk_turbo_note", "Фиксирует максимальную частоту Adreno GPU. Больше нагрев и расход батареи.");
        RU.put("chk_sustained_title", "Режим стабильной производительности (Sustained mode)");
        RU.put("chk_sustained_note", "Запрашивает у Android стабильные частоты без троттлинга (где поддерживается). Меньше нагрев, но пиковая скорость ниже.");
        RU.put("chk_thermal_title", "Thermal Guard (защита от перегрева)");
        RU.put("chk_thermal_note", "Снижает дальность прорисовки перед троттлингом и восстанавливает при остывании.");
        RU.put("chk_safemode_title", "Safe mode (для баг-репортов)");
        RU.put("chk_safemode_note", "Отключает оптимизации скорости и твики геймплея. Медленнее. Если баг пропал в Safe mode, обязательно укажите это в issue.");
        RU.put("refl_water_title", "Отражения на воде (Water reflections)");
        RU.put("refl_other_title", "Остальные отражения (машины, окна)");
        RU.put("opt_every_frame", "Каждый кадр");
        RU.put("opt_every_2nd", "Каждый 2-й кадр");
        RU.put("opt_every_3rd", "Каждый 3-й кадр");
        RU.put("opt_off", "Выкл");
        RU.put("refl_off_note", "Выкл: поверхность заливается сплошным цветом вместо отражения.");
        RU.put("legacy_perf_title", "Тестовые хаки графики");
        RU.put("legacy_perf_summary_default", "Тестовые хаки графики (дальность 0.5, тени...)");
        RU.put("legacy_perf_summary_active", "Тестовые хаки графики: Включены (%d)");

        // --- Thread Affinity Dialog ---
        RU.put("affinity_title", "Привязка потоков CPU");
        RU.put("aff_cores", " ядер: ");
        RU.put("aff_guessed", "  (определено приближенно: система не вернула размеры ядер)");
        RU.put("aff_layout_lanes", "Автоматический профиль: lanes (recorder на prime + старшем big, аудио на остальных big с nice -16). Задаётся через --android_thread_layout= в args.txt.");
        RU.put("aff_layout_clusters", "Автоматический профиль: clusters (таблица по кластерам). Задаётся через --android_thread_layout= в args.txt / native.toml.");
        RU.put("aff_layout_priority", "Автоматический профиль: priority (только nice-приоритеты, без масок: ядра выбирает Android). Задаётся в args.txt / native.toml.");
        RU.put("aff_layout_off", "Автоматический профиль: off (ядра и приоритеты выбирает Android; действуют только ручные маски). Задаётся в args.txt / native.toml.");
        RU.put("aff_auto_switch", "Автоматическое распределение (рекомендовано)");
        RU.put("aff_manual_note", "Вручную: отметьте «Вручную» у группы или отдельного потока и выберите ядра. Всё неотмеченное управляется автоматически.");
        RU.put("th_col_thread", "Поток");
        RU.put("th_manual", "Вручную");
        RU.put("th_whole_group", "  \u2014 вся группа");
        RU.put("aff_state_android_decides", "авто: решает Android");
        RU.put("aff_nice_kept", "nice без изменений");
        RU.put("warn_no_cores", ": не выбраны ядра, будет использовано автоматическое распределение.");
        RU.put("warn_only_little", " только на Little-ядрах: ожидайте сильную просадку FPS и статтеры.");
        RU.put("warn_rg_rec_share", "RenderGuest и GtaRecorder делят одно ядро: они работают конвейером и будут блокировать друг друга.");
        RU.put("warn_render_few_cores", "Потоки рендеринга зажаты на %d ядр.: им требуется как минимум 3.");
        RU.put("warn_audio_little", "AudioGuest только на Little-ядрах: звук может заикаться и хрипеть под нагрузкой.");
        RU.put("warn_sdl_little", "SDLThread только на Little-ядрах: готовые кадры могут выводиться с задержкой.");
        RU.put("warn_prime_only", " только на Prime-ядре: отнимает процессорное время у RenderGuest.");
        RU.put("warn_all_big", "Все тяжёлые потоки только на Big-ядрах: Prime и Little-ядра простаивают.");

        // Affinity Groups & Threads
        RU.put("grp_render", "Рендеринг (основные потоки)");
        RU.put("grp_audio", "Аудио");
        RU.put("grp_world", "Стриминг мира и чтение файлов");
        RU.put("grp_jobs", "Игровые задачи и физика");
        RU.put("grp_background", "Фоновые процессы");

        RU.put("th_render_guest_hint", "Основной поток отрисовки игры, строит каждый кадр");
        RU.put("th_recorder_hint", "Записывает команды кадра для Vulkan");
        RU.put("th_render_work_hint", "Подготавливает стейт отрисовки для рекордера");
        RU.put("th_main_xthread_hint", "Главный поток игровой логики");
        RU.put("th_sdl_thread_hint", "Главный поток SDL: UI и презентация кадра");
        RU.put("th_sdl_helpers_hint", "Дочерние потоки SDL (8-9 шт.)");
        RU.put("th_audio_guest_hint", "Игровой аудиомикшер");
        RU.put("th_host_audio_hint", "Вывод звука Android (управляется AAudio, без привязки)");
        RU.put("th_xma_decoder_hint", "Декодер сжатого звука");
        RU.put("th_audio_worker_hint", "Коллбэки XAudio для каждого кадра");
        RU.put("th_rage_stream_hint", "Подгрузка города вокруг игрока");
        RU.put("th_rage_fileio_hint", "Чтение игровых файлов с накопителя");
        RU.put("th_kernel_io_hint", "Фоновые задачи эмулируемого ядра Xbox");
        RU.put("th_rage_jobs_hint", "Анимации, искусственный интеллект и задачи движка");
        RU.put("th_rage_physics_hint", "Физический движок");
        RU.put("th_rage_other_hint", "Остальные системные потоки игры");
        RU.put("th_cmd_retirer_hint", "Освобождение памяти завершённых кадров");
        RU.put("th_recorder_helpers_hint", "Помощники компиляции шейдеров (потоки рекордера)");
        RU.put("th_sdl_activity_hint", "UI-поток Android");
        RU.put("th_background_hint", "Системные таймеры, binder и служебные потоки");

        // --- Args Tweaks Dialog ---
        RU.put("args_tweaks_title", "Параметры и твики args.txt");
        RU.put("args_tweaks_subtitle", "Настройки графики, плотности мира и очередей из args.txt");
        RU.put("args_summary", "Параметры и твики args.txt");
        RU.put("args_sec_density", "Плотность мира и трафик");
        RU.put("args_sec_graphics", "Дальность и тени");
        RU.put("args_sec_framerate", "Очереди кадров и синхронизация");
        RU.put("args_sec_system", "Звук и системные опции");

        RU.put("args_traffic_title", "Плотность трафика (машин в потоке)");
        RU.put("args_traffic_note", "Меньше машин — выше FPS и меньше нагрузка на CPU");
        RU.put("args_parked_title", "Плотность припаркованных машин");
        RU.put("args_parked_note", "Количество стоящих машин на обочинах и парковках");
        RU.put("args_ped_title", "Плотность пешеходов");
        RU.put("args_ped_note", "Количество прохожих на улицах Либерти-Сити");
        RU.put("args_scenario_ped_title", "Плотность сценариев пешеходов");
        RU.put("args_scenario_ped_note", "Сидящие на лавках, курящие, разговаривающие NPC");

        RU.put("args_shadow_dist_title", "Дальность прорисовки теней");
        RU.put("args_shadow_dist_note", "Дистанция, на которой отображаются тени объектов");
        RU.put("args_shadow_indices_title", "Мин. полигонов для отбрасывания теней");
        RU.put("args_shadow_indices_note", "Отсекает тени мелких объектов (мусор, столбики); 0 = все тени");
        RU.put("args_dynamic_draw_title", "Динамическая дальность прорисовки");
        RU.put("args_dynamic_draw_note", "Автоматически снижает дистанцию при просадках FPS");
        RU.put("args_dynamic_draw_min_title", "Мин. порог динамической дальности");
        RU.put("args_dynamic_draw_min_note", "Нижняя граница снижения при включённой динамической дальности");

        RU.put("args_queued_frames_title", "Макс. очередь кадров GPU (max queued frames)");
        RU.put("args_queued_frames_note", "1 = минимальный инпутлаг, 2-3 = стабильнее фреймрейт");
        RU.put("args_frame_complete_title", "Сигнал завершения кадра");
        RU.put("args_frame_complete_note", "Тип синхронизации кадра с дисплеем (0-2)");
        RU.put("args_async_constant_title", "Асинхронное освобождение констант GPU");
        RU.put("args_async_constant_note", "Снижает задержки рендер-потока при смене стейтов");
        RU.put("args_mailbox_title", "Режим вывода Mailbox (тройная буферизация)");
        RU.put("args_mailbox_note", "Вывод без разрывов (тиринга) с минимальной задержкой");
        RU.put("args_fifo_title", "Принудительный FIFO (классический VSync)");
        RU.put("args_fifo_note", "Жёсткая синхронизация кадров с развёрткой экрана");
        RU.put("args_prerotate_title", "Аппаратный Pre-rotation (Vulkan G2)");
        RU.put("args_prerotate_note", "Создаёт Swapchain в родной ориентации экрана; SurfaceFlinger больше не поворачивает буфер на GPU");
        RU.put("args_prerotate_0", "0 (Выкл / Legacy)");
        RU.put("args_prerotate_1", "1 (Вкл / Рекомендуется)");
        RU.put("args_prerotate_2", "2 (Реверс 180°)");
        RU.put("args_job_help_title", "Помощь воркерам Main (Work-Helping C3)");
        RU.put("args_job_help_note", "Поток Main выполняет задачи RAGE на ядре Cortex-X1 во время ожидания вместо сна");

        RU.put("args_audio_qframes_title", "Размер аудио-буфера (кадров)");
        RU.put("args_audio_qframes_note", "Меньше = ниже задержка звука; больше = без хрипов под нагрузкой");
        RU.put("args_perf_hint_title", "Android Performance Hint (ADPF)");
        RU.put("args_perf_hint_note", "Передаёт Android желаемое время кадра для частот CPU/GPU");
        RU.put("args_pin_threads_title", "Пиннинг горячих потоков (pin hot threads)");
        RU.put("args_pin_threads_note", "Автоматическая привязка тяжёлых циклов к производительным ядрам");
        RU.put("args_thread_layout_title", "Раскладка потоков CPU (layout)");
        RU.put("args_thread_layout_note", "lanes / lanes2 для привязки рендера, звука и игровых потоков");
        RU.put("aff_layout_lanes2", "Авто-раскладка: lanes2 (Main на Big, RenderGuest nice -10, Jobs nice -6). Задаётся через ");
        RU.put("args_toast_saved", "Параметры args.txt сохранены");

    }

    public static synchronized String getLang(Context context) {
        if (sCurrentLang != null) return sCurrentLang;
        SharedPreferences prefs = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
        if (prefs.contains(KEY_LANG)) {
            sCurrentLang = prefs.getString(KEY_LANG, null);
            if (sCurrentLang != null) return sCurrentLang;
        }
        // Default based on smartphone system language
        String sys = Locale.getDefault().getLanguage().toLowerCase(Locale.ROOT);
        sCurrentLang = (sys.startsWith("ru") || sys.startsWith("be") || sys.startsWith("uk") || sys.startsWith("kk"))
                ? LANG_RU : LANG_EN;
        return sCurrentLang;
    }

    public static synchronized void setLang(Context context, String lang) {
        sCurrentLang = lang;
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
                .edit()
                .putString(KEY_LANG, lang)
                .apply();
    }

    public static synchronized String toggleLang(Context context) {
        String next = LANG_RU.equals(getLang(context)) ? LANG_EN : LANG_RU;
        setLang(context, next);
        return next;
    }

    public static boolean isRu(Context context) {
        return LANG_RU.equals(getLang(context));
    }

    public static String getLanguageButtonLabel(Context context) {
        return isRu(context) ? "🌐 Язык: Русский (RU)" : "🌐 Language: English (EN)";
    }

    public static String tr(Context context, String key, String fallbackEn) {
        if (isRu(context)) {
            String val = RU.get(key);
            if (val != null) return val;
        }
        return fallbackEn;
    }

    public static String tr(Context context, String key) {
        return tr(context, key, key);
    }
}
