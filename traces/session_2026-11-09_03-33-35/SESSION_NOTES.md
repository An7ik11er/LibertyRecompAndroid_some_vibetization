# Заметки по изменениям и анализу (Сессия 2026-10-27)

> [!IMPORTANT]
> **ЗОЛОТОЕ ПРАВИЛО ПРИОРИТЕТОВ КОНФИГУРАЦИИ:**
> **Зашитое в лончере > rgs.txt > 
ative.toml**
> - Параметры в коде лончера сильнее всех и перебивают rgs.txt.
> - Аргументы в rgs.txt перебивают 
ative.toml.
> 
> [!CAUTION]
> **СТРОЖАЙШИЙ ЗАПРЕТ НА МОДИФИКАЦИЮ КОДА ЛОНЧЕРА:**
> В код лончера (LauncherActivity.java, LibertyActivity.java, Java-часть) **НЕ ЛЕЗТЬ БЕЗ ПРЯМОГО И ЧЁТКОГО УКАЗАНИЯ ПОЛЬЗОВАТЕЛЯ**! Все настройки менять строго через rgs.txt.


## 1. Контекст задач (по файлу new.txt от аналитика)

### Задача 1: Очистка параметров экрана и видеодрайвера
- **Статус:** Выполнено.
- **Действия:**
  - Удален файл `/files/env.txt` на устройстве. В нем содержалась строка `TU_DEBUG=`, которая затирала системный параметр `TU_DEBUG=sysmem` и принудительно активировала GMEM (тайлинг на чипе). Для GTA IV на Turnip GMEM дает просадку ~15% FPS из-за десятков промежуточных проходов и сбросов/восстановлений памяти.
  - Из `/files/args.txt` удален параметр `--android_surface=1280x576`, ломавший геометрию `SDLSurface`.

### Задача 2: «Игнорирование 720p по умолчанию»
- **Статус:** ОТКЛОНЕНО (Галлюцинация / Ошибка анализа в new.txt).
- **Причина:** Аналитик предположил баг в коде `video_mode_util.h` (якобы проверка `HasNonDefaultValue` ломает выбор 720p).
  - **Факт 1:** В коде `video_mode_util.h` этой проверки уже давно нет — движок честно читает CVar `resolution`.
  - **Факт 2:** В `native.toml` на телефоне прямо прописано `resolution = "1080p"`, выставленное вручную в настройках. Движок отрендерил 1080p именно поэтому. Код не трогался.

### Задача 3: Исправление сжатия SDLSurface в угол экрана
- **Статус:** Выполнено.
- **Файл:** `os/android/app/src/main/java/com/libertyrecomp/LibertyActivity.java`
- **Действия:** В методе `createSDLSurface` принудительно заданы параметры `ViewGroup.LayoutParams.MATCH_PARENT`. Теперь при установке любого фиксированного разрешения буфера (`setFixedSize`), Android аппаратно растягивает View на полный дисплей $2400\times1080$.

### Задача 4: Разгрузка Big-ядер от потока UI/презентера
- **Статус:** Выполнено.
- **Файлы:** `android_thread_pinning.cpp`, `ThreadAffinityDialog.java`
- **Действия:** В раскладке `lanes` поток `sdl_thread` перенесен с `K_BIG` (маска Big-ядер) на `K_LITTLE`. Это освободило ядро `c4` под тяжелый поток рендеринга `render_guest` и убрало конкуренцию оверлея с игрой.

### Задача 5: Оптимизация оверлея SensorSampler и частоты UI
- **Статус:** Выполнено.
- **Файл:** `glue/rexglue-sdk-main/gta4-recomp/src/debug/frame_time_dialog.cpp`
- **Действия:**
  - Ограничен поиск термозон: опрашиваются максимум 2 датчика CPU и 1 GPU (всего до 5 файлов sysfs вместо 22). Время работы сэмплера упало с 35.9 мс до долей миллисекунды.
  - Внедрено кэширование строк UI ImGui: строки форматируются и обновляются не чаще 1 раза в 250 мс, а в основном цикле рисуются через `ImGui::TextUnformatted`.

### Задача 6: Отключение тяжелых CVar диагностики и спам-логов
- **Статус:** Выполнено.
- **Файлы:** `graphics_system.cpp`, `gta4_native_hooks.cpp`
- **Действия:**
  - Дефолты CVar `gta4_native_hitch_breakdown`, `gta4_native_pipeline_report`, `gta4_trace_native_renderer`, `gta4_trace_black_outputs`, `gta4_trace_local_black_draws`, `gta4_trace_hove_gantry` установлены в `false`.
  - Заглушен спам `ubo-trace:` (закрыт флагом `gta4_native_debug_counters`) и `shadow-trace: heartbeat` (активен только при ненулевом `gta4_trace_shadow_targets`).
  - Вызовы `BuildModernEffectConstants` и расчеты вьюпортов обернуты в `if (shared_key.modern_effects_enabled)`.

---

## 2. Разделение обычного «Play» и «Play (Debug Log)» (глушение 3110 строк hitch:)
- Введен новый CVar `gta4_native_hitch_log` (по умолчанию `false`).
- При обычном **«Play»**:
  - Полностью отключены замеры времени `FrameTimelineNow()`, считывание 15 атомиков на потоке рендера и печать строки `hitch: frame=`. Процессор не тратит на это ни одного такта.
- При **«Play (Debug Log)»**:
  - Лаунчер автоматически активирует `--gta4_native_hitch_log=true` и `--log_level=info`.
- Спам `gta4-native-cause: point=alpha-pipeline-semantics` (1 165 строк) закрыт флагом `gta4_trace_native_renderer`.
- Создан профиль глубокого трейсинга: `traces/args_debug_trace.txt`.

---

## 3. Состояние Perfetto и SQL-анализатора
- Трейс Perfetto v34 (20 секунд) снимается штатно.
- Запускается встроенный CSV-профайлер (60 секунд).
- Все 41 SQL-запрос выполняются локальным Trace Processor за ~18 секунд (31-й запрос оптимизирован через `MATERIALIZED CTE`, время выполнения снижено с 10 минут до 0.73с без потери строк).
- Сводный отчет: `sql_analysis_report.md`
- Все CSV-результаты: папка `sql_results/`

---

## 4. Реализация пакета патчей и оптимизаций (new2.txt)

### Задача 1: Облегчение GtaCmdRetirer и NativeCommandPool
- **Файлы:** `src/graphics/gta4_native/graphics_system.h`, `stateful_constant_state.h`, `native_snapshot_resource.h`
- **Действия:**
  - Реализован полный метод `NativeCommand::Reset()`: сбрасывает все скаляры, очищает векторы `command_deltas_` и `command_state_deltas_` через `.clear()` с сохранением `capacity()` (лимит `kMaxKeptBytes = 32768`), сбрасывает только активные ненулевые `shared_ptr` (`if (p) p.reset();`).
  - В методе `NativeCommand::RecycleInPlace()` исключена деструкция `std::destroy_at` и повторная конструкция `std::construct_at`, теперь вызывается прямой вызов `Reset()`. Буферы не аллоцируются и не освобождаются в рантайме.
  - В `ConstantSnapshotPool::Copy` внедрен батчинг: при переполнении локального TLS-кэша (`loc.size() >= 256`) захватывается мьютекс один раз и батчем переносится до 128 элементов в общий пул, что сократило число `pthread_mutex_lock` на GtaCmdRetirer более чем в 100 раз.
  - В `NativeSnapshotResource` деаллокация осуществляется через lock-free интрузивный стек с CAS push, исключая мьютексы при штатном освобождении.

### Задача 2: Устранение лишней нагрузки в RecordNativeFrame, PrepareFrameTextures и BindCommonDrawState
- **Файлы:** `src/graphics/gta4_native/graphics_system.h`, `graphics_system.cpp`, `emission_trace_bind.inc`
- **Действия:**
  - В `PrepareFrameTextures` добавлено кэширование дескрипторов `NativeDrawDescriptorKey`: если ключ совпадает с предыдущим draw-вызовом, `AssignIndexedWorkingSet` пропускается и дескрипторы копируются мгновенно.
  - В `NativeFrameResources` добавлены поля `has_last_shared_key`, `last_shared_key` и `last_shared_allocation`.
  - В `BindCommonDrawState` добавлена проверка совпадения ключа с предыдущим вызовом: при совпадении пропускаются хэширование XXH3 (26 слотов), поиск в хэш-таблице и копирование констант (1 280 байт), сразу используется `last_shared_allocation`.
  - Вызовы `gpu_flight::Record` защищены проверкой `IsNativeFlightRecorderEnabled()`.
  - Диагностика `ClassifyTranslucentDiagnosticShader` и `emission_trace_bind.inc` закрыта проверкой `REXCVAR_GET(gta4_trace_native_renderer)`.

### Задача 3: Устранение rex::cvar::GetFlagByName из горячего цикла кадров
- **Файлы:** `src/input/absolute_pointer.cpp`, `src/input/sony_feedback_options.cpp`
- **Действия:**
  - В `GetConfiguredTouchControlsMode()` заменен поиск по строковому имени на прямой доступ через макрос `REXCVAR_GET(touch_controls)`.
  - В `sony_feedback_options.cpp` функция `GetOptions()` переведена на прямое обращение через 6 макросов `REXCVAR_GET(gta4_sony_...)`.
  - Устранены аллокации `std::string` и поиск в `std::unordered_map` на каждом опросе ввода и кадре.

### Задача 4: Конфигурация Perfetto v34 и SQL-запрос 31
- **Файлы:** `traces/perfetto_v34_config.pbtxt`, `traces/record_trace.sh`, `traces/PERFETTO_SQL_QUERIES.sql`, `traces/perfetto_queries_2026-10-26.json`
- **Действия:**
  - В секцию `ftrace_config` конфигураций Perfetto добавлена строка `atrace_apps: "com.libertyrecomp:game"` для сбора ATrace маркеров движка (RG, RW, Rec).
  - В секцию `linux.perf` добавлен фильтр `target_cmdline: "com.libertyrecomp:game"`.
  - В запросе №31 декартов JOIN по интервалу заменен на коррелированный скалярный подзапрос с `ORDER BY ts DESC LIMIT 1`. Число строк оптимизировано с 1 219 021 до 2 039, время выполнения запроса TraceProcessor упало с минут до ~3 секунд.

---

## 5. Дополнительные оптимизации по анализу форков (13_FORKS_ANALYSIS_TOP5_FINDINGS.txt)

### Дедупликация привязки индексного буфера (`vkCmdBindIndexBuffer`)
- **Файлы:** `src/graphics/gta4_native/native_draw_state_cache.h`, `graphics_system.h`, `graphics_system.cpp`
- **Действия:**
  - В `NativeDrawStateCache` добавлен метод `UpdateIndexBuffer(command_buffer, buffer, offset, index_type)` и поле `index_buffer_` (4-элементный кортеж).
  - В `Gta4NativeGraphicsSystem` реализован метод `BindNativeIndexBuffer(command_buffer, buffer, offset, index_type)` с CVar-флагом `gta4_native_dedupe_index_binding` (по умолчанию `true`).
  - Все прямые вызовы `dfn.vkCmdBindIndexBuffer` в `graphics_system.cpp` (включая горячий цикл `RecordIndexedPrimitive` и процедурные квады/веера) переведены на `BindNativeIndexBuffer`.
  - Повторные системные вызовы драйвера Vulkan с тем же буфером и смещением теперь пропускаются, снижая накладные расходы рекордера на кадр.

### Ограничение частоты отрисовки UI / ImGui на Android
- **Файлы:** `src/ui/presenter.cpp`
- **Действия:**
  - В `WaitForUITickFromUIThread()` реализовано ограничение частоты кадров UI-потока для платформ non-Win32 (Android) через CVar `present_ui_fps_limit` (по умолчанию 60 FPS / 16.6 мс).
  - Устранен бесконтрольный цикл отрисовки оверлея в `SDLThread` при активных ImGui-диалогах и непрерывных перерисовках, сберегая процессорное время ядра.

---

## 6. Замечания по тестам и третья волна оптимизаций (14_OPTIMIZATION_TASKS_BATCH3_HEHE.txt)

### Наблюдение пользователя по физике
- **Симптом:** При обработке разрушений физических объектов (столб + скамейка) наблюдаются резкие локальные просадки FPS.
- **Причина в архитектуре:** Потоки физики `rage_physics` и планировщика задач `rage_jobs` в `android_thread_pinning.cpp` назначены на `Kind::kJobsLane` (ядра 4–5) с `nice = -2`. При массивных коллизиях и фрагментации объектов гостевой физический движок плодит десятки задач, вызывая всплеск загрузки ядер и задержку синхронизации с `RenderGuest`. Взято на заметку для следующей профилировки по Perfetto SQL.

### Реализация безопасных оптимизаций из ТЗ 14
- **Файлы:** `graphics_system.cpp`
- **Действия:**
  1. **Параллельный конвейер кадров:**
     - Значение CVar `gta4_native_frame_complete_signal` по умолчанию переведено с `0` на `1` (сигнал отдается в `RenderWorkerMain` при передаче собранного кадра рекордеру).
     - Это устраняет пинг-понг между `RenderGuest` и `GtaRecorder` без риска гонок данных, позволяя гостю параллельно формировать следующий кадр.
     - На устройстве в `args.txt` уже прописан `--gta4_native_frame_complete_signal=1`.
  2. **Ранний выход во Flight Resources:**
     - В `StageNativeTextureFlightResources` и `StageNativeSurfaceFlightResources` добавлена проверка `if (!IsNativeFlightRecorderEnabled()) return;`, исключающая создание `CpuScope`, разыменование дескрипторов и холостые обходы mip-уровней при отключенной диагностике.
  3. **Обход active textures по маске в `retain_assembled`:**
     - Вместо прогона всех 26 слотов массива `textures` обходятся только биты `used_texture_mask` через `__builtin_ctz` с явной проверкой границ `idx < owner->textures.size()`.
     - Объединены в один проход регистрация текстур, маркировка `assembly_read_epoch` и учет `packed_depth_source`.
  4. **Сохранение пиннинга потоков:**
     - По рекомендации второго помощника раскладка ядер в `android_thread_pinning.cpp` оставлена без изменений, чтобы сохранить приоритет `nice = -16` для `audio_guest`, `xma_decoder` и `audio_worker` и не допустить деградации и щелчков звука.


---

## 7. Консолидированный аудиторский свод всех правок
- **Созданы файлы:**
  - `D:\LibertyRecompAndroid\FULL_CODE_CHANGES_AUDIT.md` (в корне проекта)
  - `D:\LibertyRecompAndroid\Outsider_memory\15_FULL_CODE_CHANGES_AUDIT.md` (во внешней памяти)
- **Объем:** ~504 КБ текста.
- **Содержание:**
  - Архитектурное описание всех 7 оптимизированных подсистем с объяснением причин и эффекта.
  - Сводная статистика `git diff --stat` и статус `git status -s`.
  - Полный исходный код нового класса `ThreadAffinityDialog.java`.
  - Полный единый unified diff по 54 файлам основного репозитория (4 187 строк добавлено, 2 019 удалено).
  - Полные unified diffs по всем модифицированным сабмодулям (`FFmpeg`, `libmspack`, `MoltenVK`, `ffmpeg-core`, `plume`, `XenosRecomp`, `llvm-project-libcxx`).
  - Чеклист и рекомендации для независимого аудитора.


---

## 8. Четвертая волна оптимизаций: Технический аудит 10 тезисов и 5 точечных задач
- **Источник:** `16_TECHNICAL_AUDIT_10_POINTS_AND_5_TASKS.txt` (бывший `2132132.txt`).
- **Суть:**
  1. Ликвидация ~78 000 холостых вызовов ATrace `kWorkerWaitForCommands` и тысяч холостых `worker_cv_.notify_one()`.
  2. Ускорение `NativeCommand::Reset()` под Cortex-A55 (сброс только non-null `shared_ptr`, пропуск пустых POD-массивов) и вынос `RecycleInPlace()` до захвата мьютекса в `RetireBatch`.
  3. Пакетное снятие списка в `NativeSnapshotResource::do_allocate` через `exchange(nullptr)` (полная ликвидация ABA и накладных расходов атомиков).
  4. Проверка флагов `-march=armv8.2-a+lse` в CMakeLists, проверка сброса `NativeDrawStateCache` на `vkBeginCommandBuffer` и `vkCmdExecuteCommands`, глушение `ClassifyTranslucentDiagnosticShader` и `color-resolve`.
  5. Логирование `/proc/self/task/<tid>/cpuset` при вмешательстве OxygenOS/OPlus в `android_thread_pinning.cpp`.
- **Реализация (27.10.2026 18:15):**
  - **Задача 1:** В `graphics_system.h` добавлен флаг `bool worker_waiting_ = false;` (под `render_mutex_`). В `RenderWorkerMain` скоуп `TRACE_SCOPE("RW: wait for commands")` и ожидание `render_condition_.wait(...)` обернуты строго в `if (render_worker_running_ && render_queue_.empty())`. В `SubmitTitleCommand` замер `queued_before` выполняется непосредственно перед вставкой, а сигнал `render_condition_.notify_one()` отдается только если `worker_waiting_ == true`, при очереди 0 или при `kPresent`. Полностью исключены холостые вызовы `write()` в `trace_marker` и предотвращен lost wakeup.
  - **Задача 2:** В `NativeCommand::Reset()` убрано принудительное обнуление гигантских POD-массивов (`texture_fetches`, `draw_descriptor_sets`, `binding_realization`, `texture_descriptor_indices`, `sampler_descriptor_indices`, `snapshot_render_targets`, `snapshot_depth_stencil`, `fixed_function_state`, `lighting`), сберегая пропускную способность in-order ядер Cortex-A55. Флаг `bindings_prepared` сбрасывается в `false`.
  - **Задача 3:** В `native_snapshot_resource.h` верифицировано пакетное снятие списка через `exchange(nullptr)` в `do_allocate` и CAS push в `do_deallocate` (полная свобода от ABA).
  - **Задача 4:** В `build_native.sh` добавлен флаг `-mno-outline-atomics`. В `graphics_system.cpp` добавлен явный сброс `native_draw_state_cache_.Reset()` при каждом `vkBeginCommandBuffer`. Спам `ClassifyTranslucentDiagnosticShader` (возврат `std::string_view{}`) и `color-resolve` закрыт флагом `gta4_trace_native_renderer`.
  - **Задача 5:** В `android_thread_pinning.cpp` реализована функция `ReadSmallFile(tid, "cpuset")` с обрезкой `\n`. При перебивании масок системой OxygenOS/OPlus в Logcat печатается имя cpuset (`top-app`, `foreground` и др.).
  - **Сборка:**
    - Нативные библиотеки успешно скомпилированы и синхронизированы (`build_native.sh` -> код 0).
    - APK собран: `app-debug.apk` (181 МБ, `BUILD SUCCESSFUL in 1m 23s`).
    - Установка на устройство НЕ производилась (по требованию пользователя).


---

## 9. Пятая волна оптимизаций: Устранение false sharing, Staging текстурной защиты, оверлей и откаты
- **Источник:** `17_BATCH_29_OCT_AUDIT_AND_DIFFS/` (`аудит_27.10.txt`, `правки_вручную.txt`, диффы).
- **Ключевой диагноз трейсов 27.10 (SD888):**
  1. Реальный FPS на улице — 23.6–24.6 к/с.
  2. Главное узкое место — `RenderGuest` (81% времени занят, ещё 15% ждёт ядро).
  3. 62% времени `RenderGuest` уходит на хостовый захват команд (~21 мс на кадр), с ~67 засыпаниями на `queued_protection_mutex_` за кадр (воркер держит замок на снятие пачки из 32 команд со ~200 операциями в хеш-таблице).
  4. False sharing 64-байтных линий кэша DSU/L3: структуры пулов и мьютексы делят одни линии между ядрами Cortex-X1/A78 и Cortex-A55.
  5. UI-презентер производит 58 показов в секунду вместо 24 к/с игры из-за непрерывной перерисовки безусловного `FrameTimeDialog`.
- **Реализация:**
  - **Шаг 1 (Рендерер, `graphics_system.h` и `.cpp`):**
    - Внедрен staging-индекс `staged_texture_protection_` под `staged_protection_mutex_`. `RenderGuest` захватывает ссылки локально без захвата замка очереди.
    - Перенос в очередь `queued_texture_protection_` выполняется пачкой в `PublishStagedDrawCommandsLocked(also)` под `render_mutex_`.
    - Разведены кэш-линии `alignas(64)` для `render_mutex_`, `queued_texture_protection_`, `queued_title_presents_`, `render_worker_running_`, `command_capture_mutex_`, `staged_texture_protection_`, `worker_protection_mutex_`, `frame_recorder_mutex_`.
    - Откачено зануление вьюпорта в `BuildModernEffectConstants` (расчет безусловный на редкий промах кэша).
    - CVar `gta4_native_upscaler` кэшируется как `static const bool use_sgsr` при старте.
  - **Шаг 2 (Пул снапшотов и команд, `native_snapshot_resource.h` и `native_owned_commands.h`):**
    - В `NativeSnapshotResource` раскладка `SizeClass` разбита по кэш-линиям `alignas(64)`: `class_count_` и `sizes_` (read-mostly), `local_` (только аллокатор), `returned_` (CAS каждого класса на отдельной линии).
    - В `NativeOwnedCommands` разведены `local_` и `mutex_` по 64 байта, восстановлено управление `recycling_` через CVar.
    - В `stateful_constant_state.h` проверка `CanIncrementStateVersion` перенесена ДО `memcpy` в `canonical_`, возвращен вызов `hash_callback`.
  - **Шаг 3 (Таймер, FPCR, xthread):**
    - В `timer_queue.cpp` удален искусственный `sleep_for(2ms / 200us)` перед чтением очереди.
    - В `context.h` заглушен счетчик `countFpcrWrite()`.
    - В `xthread.cpp` чтение `r4` защищено через `LookupHeap` и `QueryRangeAccess`.
  - **Шаг 4 (Потоки и UI):**
    - В `threading_posix.cpp` пропуск `cond_.notify_all()` если `cond_waiters_ == 0`, убран futex broadcast на `count_--`.
    - В `android_thread_pinning.cpp` добавлена раскладка `lanes2` (`main_xthread` на Big 0x70, `render_guest` nice -10).
    - В `gta4_app.cpp` создание `FrameTimeDialog` закрыто CVar `gta4_frame_time_overlay` (default false).
    - В `presenter.cpp` пропуск сна `WaitForUITickFromUIThread()` если `publication_progress_.pending()`.
  - **Шаг 5 (Сборка):**
    - В `build_native.sh` удален флаг `-w`, оставлены строгие предупреждения с `-Wno-asm-operand-widths`.

---

## 10. Шестая волна оптимизаций: Исправление оверлея (реальный FPS игры vs 58 FPS UI) и разблокировка презентера
- **Источник:** `18_BATCH_30_OCT_OVERLAY_AND_PRESENTER/` (`пояснения_30.10.txt`, `заметка к 30.txt`, диффы).
- **Ключевой диагноз:**
  1. Оверлей считал кадры по `io.DeltaTime` (интервал между перерисовками UI), а `WantsContinuousRepaint()` в базовом классе `rex::ui::ImGuiDialog` по умолчанию возвращал `true`. В результате `SDLThread` нарезал до 58 показов в секунду при 24 кадрах игры, искажая статистику и перегружая GPU проходами UI.
  2. В `Presenter::WaitForUITickFromUIThread` на Android отсутствовал механизм сброса ожидания при поступлении нового кадра игры (на Win32 работал `dxgi_ui_tick_force_requested_`), из-за чего игра могла спать до 16.7 мс на тике UI.
  3. В `NativeCommand::Reset()` исключение обнуления `texture_fetches` приводило к сохранению мусорных дескрипторов в неактивных слотах, нарушая `memcmp` при слиянии UP- и indexed-draws и ломая кэширование констант.
- **Реализация:**
  - **Оверлей (`frame_time_dialog.cpp/.h`):**
    - Интервалы и FPS теперь вычисляются строго по часам публикации игры (`rex_overlay_frame_clock_read`), при отсутствии хука — откат на DeltaTime с пометкой `[ui]`.
    - `WantsContinuousRepaint()` переопределен в `false` (показов ровно столько, сколько кадров игры).
    - Опрос датчиков вынесен в фоновый поток `OverlaySensors` (2 раза/с) с `nice = 10`.
    - Геймпады читаются через `SDL_GetGamepadFromID` без постоянного вызова open/close.
  - **Презентер (`presenter.cpp`):**
    - Добавлен флаг `std::atomic<bool> g_ui_tick_force_requested{false}`.
    - В `ForceUIThreadPaintTick()` флаг выставляется в `true`, а в `WaitForUITickFromUIThread()` сбрасывается через `exchange(false)`, минуя сон при показе кадра игры.
  - **Свопчейн (`vulkan_presenter.cpp`):**
    - В лог `[SwapchainCapabilities]` добавлен вывод `currentTransform` и `supportedTransforms`.
  - **Пул команд (`graphics_system.h`):**
    - В `Reset()` возвращен сброс `texture_fetches.fill(xenos::xe_gpu_texture_fetch_t{})`.
    - Лимит `kMaxKeptBytes` установлен в `16384`.
    - Все горячие структуры выровнены по кэш-линиям `alignas(64)`.




---

## 11. Седьмая волна: Гибридный оверлей (честный FPS кадра + мониторинг потоков / ядер через /proc/self/task + умный опрос термозон)
- **Причина:** Пользователь отметил, что на свежем билде оверлей потерял показания датчиков (выводил "n/a" или скрывал строки) и перестал показывать, на каких ядрах крутятся потоки. Анализ показал, что автор пакета 30.10 взял за основу старый минималистичный оверлей с HwMonitor, где не было чтения потоков, а системный `/proc/stat` на Android 14 блокируется SELinux.
- **Реализация (`frame_time_dialog.cpp/.h`):**
  - **Сохранено из 30.10:** Честный замер интервалов кадра игры по хуку `rex_overlay_frame_clock_read`, `WantsContinuousRepaint() = false` (отсутствие паразитных 58 Гц перерисовок), фоновый сэмплер `OverlaySensors` (`nice = 10`).
  - **Мониторинг потоков игры:** В фоновом сэмплере реализован обход `/proc/self/task/` (всегда доступен процессу на Android 14 без root). Парсинг `/proc/self/task/<tid>/stat` извлекает номер ядра (field 39 `processor`) и время `utime + stime`. Вычисляется процент нагрузки каждого потока: `RenderGuest [c6]`, `GtaRecorder [c7]`, `AudioGuest [c4]`, `Main XThread [c5]`, `GtaRenderWork [c5]`, `GtaCmdRetirer [c1]`, `RagePhysics [c2]`, `SDLThread [c0] <ui`.
  - **Умное сканирование термозон:** При старте сканируются `/sys/class/thermal/thermal_zone0..79`, по строке `type` определяются зоны для SoC/CPU (`cpu`, `soc`, `tsens`), GPU (`gpu`, `kgsl`) и батареи (`bat`, `bms`). В рабочем цикле читаются только найденные зоны, исключая вывод "n/a".
  - **GPU и частоты CPU:** Добавлены пути-фоллбэки для KGSL (`gpu_busy_percentage`, `gpubusy`, `gpuclk`, devfreq) и CPU clusters (`scaling_cur_freq` / `cpuinfo_cur_freq`).
  - **Сборка:** Нативный код `libmain.so` и APK `app-debug.apk` (181.7 МБ) собраны успешно.


---

## 12. Откат оверлея на исходную версию по требованию пользователя
- **Действие:** Файлы `glue/rexglue-sdk-main/gta4-recomp/src/debug/frame_time_dialog.cpp` и `frame_time_dialog.h` полностью возвращены к их оригинальной версии (исходный `HwMonitor`, расчет FPS по `io.DeltaTime`, старый вид окна).
- **Сборка и установка:** 
  - Нативная сборка: успешно (код 0).
  - Сборка APK: `BUILD SUCCESSFUL in 23s`.
  - Установка на устройство: `adb install -r` завершилась успехом (`Success`).


---

## 13. Установка полноценного продвинутого оверлея
- **Действие:** В `glue/rexglue-sdk-main/gta4-recomp/src/debug/frame_time_dialog.cpp` и `frame_time_dialog.h` установлен полноценный 1038-строчный оверлей с:
  - Фоновым потоком `SensorSampler` (опрос раз в 1 с на Little-ядрах);
  - Тремя режимами: `Mode::kFull`, `Mode::kCompact`, `Mode::kHidden` с переключением тапом по экрану и L3+R3 на геймпаде;
  - Анализом периодических микрофризов `Periodic` (every N frames);
  - Мониторингом потоков через `/proc/self/task/` с таблицей `ThreadRow` (имя, tid, % CPU, ядро `core`);
  - Декодированием термозон CPU, GPU, батареи и статуса перегрева.
- **Сборка и установка:**
  - `build_native.sh`: УСПЕШНО (`код 0`, `libmain.so` 199.3 МБ).
  - `build_apk.sh debug`: `BUILD SUCCESSFUL in 24s`.
  - Установка на устройство: `adb install -r` завершилась успехом (`Success`).


---

## 14. Интеграция правок пакета 31.10 и ограничение частоты перерисовки оверлея
- **Дата:** 28.10.2026.
- **Основание:** Разбор трейса 27.10 20:45 и пакет правок Outsider_memory/31 (разбор_31.10.txt, 31.txt).
- **Применённые изменения:**
  1. glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h и .cpp:
     - Выделена отдельная worker_condition_ для ожидания воркера, разделены сигналы воркера и продюсеров (
ender_condition_).
     - Пробуждение воркера (wake_worker) выполняется строго по флагу worker_waiting_, устранены холостые системные вызовы futex pthread_cond_signal (~1.1% CPU RenderGuest).
     - Буфер pending_state копируется вместо swap, что сохраняет ёмкость продюсера, а буфер команды растёт степенями двойки (std::bit_ceil(std::max<size_t>(bytes, 256))).
     - Метод Reset() теперь обнуляет 	exture_fetches только для реально использованных стадий (used_texture_mask), экономя ~12 кэш-линий на команду при возврате слотов ретайрером.
  2. glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h:
     - Резервирование payload и 
anges приведено к степеням двойки (избавляет от постоянных перевыделений malloc/free в jemalloc при каждом draw).
  3. glue/rexglue-sdk-main/src/graphics/gta4_native/native_snapshot_resource.h:
     - Добавлен префетч __builtin_prefetch(local, 1, 3) следующего свободного блока снапшота.
  4. glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp:
     - В профиле lanes2 заданы приоритеты: RageJobs и RagePhysics получили 
ice = -6, фоновый GtaCmdRetirer получил 
ice = +5. Активируется флагом --android_thread_layout=lanes2.
  5. glue/rexglue-sdk-main/gta4-recomp/src/debug/frame_time_dialog.h и .cpp:
     - Переопределён ool WantsContinuousRepaint() const override, возвращающий 	rue только в случае отсутствия новых кадров игры более 500 мс (загрузка, пауза). Снижает паразитные перерисовки UI с 66.5 до нормальных ~25 Гц.
- **Диффы:**
  - Обновлены соответствующие .diff файлы рядом с каждым изменённым файлом исходного кода.
- **Сборка:**
  - uild_native.sh: УСПЕШНО (код 0, libmain.so 199.3 МБ, 725 hooked functions, none called directly).
  - uild_apk.sh debug: УСПЕШНО (BUILD SUCCESSFUL in 1m 25s, pp-debug.apk 181,776,620 байт).
  - Установка APK на устройство: **пропущена** по прямому запросу пользователя («ток апк не ставь»).


---

## 15. Интеграция правок пакета 32 (CaptureKeepAlive, оптимизация ретайрера, защита текстур)
- **Дата:** 28.10.2026.
- **Основание:** Разбор трейса 28.10 11:39 (+15.4% FPS, вырос с 24.7 до 28.5) и пакет правок Outsider_memory/20_BATCH_32_OCT_RESOURCE_KEEPALIVE_AND_RETIRER (разбор_32.txt, 20_BATCH_32_OCT_NOTES.txt).
- **Применённые изменения:**
  1. glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h и .cpp:
     - Внедрён механизм CaptureKeepAlive: продюсер кадра держит одну сильную ссылку на каждый уникальный ресурс кадра, а команды получают aliased shared_ptr (std::shared_ptr<T>(alias, ptr)). Инкременты идут в единый локальный control block без вызовов weak_ptr::lock и без межъядерной гонки CAS (~2.7 мс CPU кадра). Откат без пересборки: --gta4_native_capture_keepalive=false.
     - Сбор защиты текстур вынесен из-под тяжелого 
ender_mutex_: метод AppendInFlightTextureProtection точечно читает очереди staged -> queued -> worker под своими замками.
     - EvictNativeTextureImages больше не держит 
ender_mutex_ на протяжении всего цикла вытеснения.
  2. glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h:
     - Reset() теперь отпускает текстуры и вершинные буферы только по маскам used_texture_mask и captured_stream_mask, не затрагивая пустые слоты (существенное сокращение промахов кэша на Little-ядрах A55).
     - Добавлен префетч строк памяти команд на 2 шага вперёд (PrefetchForRecycle).
     - Освобождённые слоты возвращаются в пул батчами по 128 команд, пока они ещё горячие в общем L3-кэше.
     - Добавлено предупреждающее логирование каждые 300 кадров (gta4-native-retire).
  3. glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp:
     - В MakeLanes2() возвращён приоритет 
ice = 0 для kCmdRetirer (out[kCmdRetirer] = Rule{Kind::kLittle, 0, false}), так как ретайрер загружен на 97% и занижение приоритета до +5 перегрузило бы очередь команд.
- **Диффы:**
  - Созданы и обновлены .diff файлы рядом со всеми модифицированными файлами (
ative_owned_commands.h.diff, graphics_system.h.diff, graphics_system.cpp.diff, ndroid_thread_pinning.cpp.diff).
- **Сборка и установка:**
  - uild_native.sh: УСПЕШНО (код 0, libmain.so 199.3 МБ, librexgpu-gta4-native.so 79.9 МБ, 725 hooked functions, none called directly).
  - uild_apk.sh debug: УСПЕШНО (BUILD SUCCESSFUL in 30s, pp-debug.apk 181.8 МБ).
  - Установка на устройство 2dc3ec42: УСПЕШНО (Performing Streamed Install -> Success).
  - Все библиотеки на устройстве распакованы с полными отладочными символами (79.9 МБ librexgpu-gta4-native.so).


---

## 16. Интеграция правок пакета 33 (SnapshotKeepAlive, пул одиночных команд, выравнивание кэша)
- **Дата:** 28.10.2026.
- **Основание:** Разбор трейса 28.10 12:47 (пакет 32 + lanes2, 29.5 FPS) и пакет правок Outsider_memory/21_BATCH_33_OCT_SNAPSHOT_KEEPALIVE_AND_POOL (разбор_33.txt, 21_BATCH_33_OCT_NOTES.txt).
- **Выявленные факторы трейса 12:47:**
  - Фоновый процесс MtpServer (android.process.media) съедал 33% одного Little-ядра из-за включенного режима USB «Передача файлов (MTP)».
  - Половина времени ретайрера (12 мс из 24 мс) уходила на деструкторы снапшотов воркера NativeShaderState/NativePipelineState.
  - Воркер уничтожал не вошедшие в кадр команды через Deleter (6.9% времени воркера), возвращая слоты неинициализированными, что вызывало malloc и 
eserve на RenderGuest.
- **Применённые изменения:**
  1. glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h и .cpp:
     - Внедрён механизм SnapshotKeepAlive: воркер удерживает снапшоты кадра в едином объекте, команды получают aliased shared_ptr. Деструкторы снапшотов перенесены на Big-ядро воркера во время передачи кадра рекордеру (ожидаемое снижение нагрузки на ретайрер еще на 40-50% и освобождение трети Little-ядра для RagePhysics/RageJobWork). Откат: --gta4_native_worker_snapshot_keepalive=false.
     - Сравнение снапшотов шейдера в ApplyShaderConstantDelta ограничено полями vertex/pixel/boolean (активация слияния draw-вызовов и увеличение cache hit).
     - Выравнивание слотов кэша захвата строго по 64 байтам (lignas(64) и static_assert на размер), укладывая проверку в одну кэш-линию.
     - Батчинг пробуждения воркера: спящий воркер будится каждые 64 команды (либо по таймауту 2 мс), убирая лишние futex-вызовы у RenderGuest.
  2. glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h:
     - Пул перерабатывает одиночные команды: Deleter вызывает RecycleInPlace() вместо уничтожения команды и буферов (устранение malloc в constant capture на RenderGuest).
- **Диффы:**
  - Созданы и обновлены .diff файлы рядом с каждым изменённым файлом (
ative_owned_commands.h.diff, graphics_system.h.diff, graphics_system.cpp.diff).
- **Сборка:**
  - uild_native.sh: УСПЕШНО (код 0, libmain.so 199.3 МБ, librexgpu-gta4-native.so 80.2 МБ с полными отладочными символами).
  - uild_apk.sh debug: УСПЕШНО (BUILD SUCCESSFUL in 21s, pp-debug.apk 181.8 МБ).


---

## 17. Экстренная миграция проекта на диск G:\cod в связи с отказом HDD D:
- **Дата:** 29.10.2026.
- **Причина:** Физический отказ / падение жесткого диска D:, риск полной потери данных.
- **Действия по миграции:**
  1. **Перенос файлов:**
     - Репозиторий полностью скопирован из D:\LibertyRecompAndroid в G:\cod\LibertyRecompAndroid через многопоточный 
obocopy с защитой от зависаний на битых секторах (/R:1 /W:1 /MT:8).
     - Перенесены все уникальные исходники (glue/), внешняя память (Outsider_memory/), история и логи (	races/), скрипты (os/), сторонние библиотеки (	hirdparty/, 	hird_party/, gi/, 	racy/), тяжелые инструменты (	ools/ 4.4 ГБ) и полный репозиторий .git/ (11.2 ГБ).
     - Проверена целостность Git: статус репозитория на диске G: полностью совпадает с состоянием до сбоя, все подмодули используют относительные пути worktree = ../../../... и функционируют корректно.
  2. **Устранение «неочевидных трудностей» и перепривязка путей:**
     - os/android/local.properties: переписан sdk.dir=G:/cod/cacheSlopodroid.
     - os/android/scripts/env.sh: добавлена проверка и приоритетная установка ANDROID_HOME=/g/cod/cacheSlopodroid и ANDROID_NDK=/g/cod/cacheSlopodroid/ndk/30.0.16248370, предотвращающая чтение старых системных переменных с диска D:.
     - CMake и Ninja: старый кэш сборки out/ не переносился, выполнена чистая первичная конфигурация под диск G:.
  3. **Верификация сборки:**
     - uild_native.sh: УСПЕШНО (код 0, скомпилировано и слинковано 1055 из 1055 целей, получены libmain.so 200.4 МБ, librexgpu-gta4-native.so 80.4 МБ).
     - uild_apk.sh debug: УСПЕШНО (BUILD SUCCESSFUL in 1m 5s, собран pp-debug.apk 146.9 МБ).
  4. **Итог:** Проект полностью отвязан от диска D:, автономен и работоспособен в G:\cod\LibertyRecompAndroid.


---

## 18. Глубокая очистка репозитория (3 уровня) и интеграция пакета 34
- **Дата:** 29.10.2026.
- **Основание:** Запрос пользователя на чистку дискового пространства и внедрение пакета 34 (Outsider_memory/22_BATCH_34_OCT_PIPELINE_MEMO_AND_PENDING, 22_BATCH_34_OCT_NOTES.txt, разбор_34.txt).
- **Результаты чистки (освобождено 14.5 ГБ):**
  1. **Уровень 1 (автономный мусор):**
     - Удалены: list.txt (26 МБ), gi/ (338 МБ), 	racy/ (125 МБ), 	ools/ghidra_11.2.1_PUBLIC (385 МБ), 	ools/GTAIV.EFLC.FusionFix-master (236 МБ), 	ools/xtd_tools (68 МБ), 	ools/xenia-master-1 (28 МБ), 	ools/FidelityFX-SDK-2.1.0 (118 МБ), zip-архивы старых трейсов (242 МБ).
  2. **Уровни 2 и 3 (десктопные подмодули tools/ и thirdparty/):**
     - Деинициализированы подмодули: 	ools/llvm-project-libcxx, 	ools/fsr1, 	ools/DLSS, 	ools/FidelityFX-SDK, 	ools/OpenOrbis-PS4-Toolchain, 	ools/smaa, 	ools/SparkIV, 	ools/libnx, 	ools/XexToolGUI, 	ools/extract-xiso, а также все подмодули корневого 	hirdparty/ (GameNetworkingSockets, MoltenVK, SDL, SDL_mixer, cpkg и др.).
     - Очищены их кэши в .git/modules/tools и .git/modules/thirdparty.
     - Папка проекта уменьшилась с 22.35 ГБ до 7.88 ГБ, .git/ уменьшился с 11.47 ГБ до 2.34 ГБ.
     - Свободное место на диске G: выросло с 23.5 ГБ до 38.0 ГБ (+14.5 ГБ).
- **Интеграция пакета 34:**
  1. glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h:
     - Класс NativeSnapshotPipelineMemo: снапшот пайплайна не копирует мемо-кэш (устранение ~1.7 КБ копирования на каждый draw, ожидание ~−1 мс на кадр воркера).
     - VisitProtectedTextureGenerations: обход текстурных слотов по used_texture_mask вместо 32 слотов (экономия 8 кэш-линий на команду).
     - Декларация TakeProducerPendingState.
  2. glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp:
     - Метод TakeProducerPendingState: передача pending_state через swap без лишних перевыделений (~−2% RenderGuest).
     - StageTextureProtection: пропуск блокировки staging-замка для команд без поколений текстур.
     - RenderWorkerMain: убрана лишняя пара lock/unlock worker_protection_mutex_ на команду кадра.
  3. glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h:
     - CaptureConstantPayloadDelta переписан в два прохода без ector::insert (один 
esize + memcpy на диапазон, ~−1% RenderGuest).
  4. Созданы/обновлены .diff файлы рядом с каждым исходником.
- **Сборка:**
  - uild_native.sh: УСПЕШНО (код 0, скомпилированы и слинкованы librexgpu-gta4-native.so 80.3 МБ и libmain.so 200.4 МБ).

## 19. Интеграция двуязычного интерфейса в лончер (RU / EN) и кнопка переключения (29.10.2026)

### Что сделано:
1. Создан модуль локализации [LauncherI18n.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherI18n.java) в пакете com.libertyrecomp:
   - Поддержка мгновенного переключения RU ⇄ EN.
   - Персистентное сохранение выбора пользователя в SharedPreferences (launcher_prefs.xml, ключ launcher_lang).
   - Умный дефолт: русский язык при первой установке.
   - Естественные игровые/технические англицизмы сохранены без грубого машинного перевода (Play, Debug-лог, Title Update 8, Turnip, Safe mode, GPU Turbo, Sustained mode, Thermal Guard, Water reflections, shadow map, lanes, RenderGuest, GtaRecorder, pipeline, FPS, статтеры, issue, GitHub, AdrenoTools).
2. В [LauncherActivity.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java):
   - Добавлена кнопка переключения языка 🌐 Язык: Русский (RU) / 🌐 Language: English (EN) в левой колонке с gamepad focus support.
   - При нажатии язык переключается и вызывается 
ecreate().
   - Полностью локализованы все карточки (Install, Driver), статусы, диалог доступа к файлам, подсказки геймпада и окно настроек производительности (showPerformanceTestSettings()).
3. В [ThreadAffinityDialog.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/ThreadAffinityDialog.java):
   - Локализованы заголовок, свичи, описание топологии, строки профилей (lanes/clusters/priority/off), заголовки групп и потоков, а также все 8 правил предупреждений (R0-R7).
4. Обновлены .diff файлы рядом с исходниками (LauncherActivity.java.diff, uild.gradle.kts.diff, ThreadAffinityDialog.java.diff).
5. Проверена сборка: uild_apk.sh debug завершилась с кодом 0 (BUILD SUCCESSFUL in 1m 6s, pp-debug.apk 147.1 MB).
6. Развёрнуто и протестировано на реальном устройстве 2dc3ec42: переключение работает мгновенно в обе стороны, состояние сохраняется.

## 20. Привязка языка к локали ОС и интеграция твиков args.txt в лончер (29.10.2026)

### Пользовательские требования:
1. Дефолтный язык лончера должен определяться автоматически по системному языку смартфона (если ru/be/uk/kk — Русский, иначе Английский). Ручное переключение через кнопку по-прежнему сохраняется в SharedPreferences.
2. В таком же стиле, как кнопка с раскидыванием потоков CPU (прямо под ней в `showPerformanceTestSettings()`), добавить кнопку с параметрами и твиками из `args.txt` (без дебаг/профайлер команд).
3. Для true/false — чекбоксы, для числовых параметров — ползунки (SeekBar) с отображением живого числового значения.

### Что сделано:
1. **Автоопределение языка по локали системы ([LauncherI18n.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherI18n.java)):**
   - Метод `getLang(Context)` проверяет `Locale.getDefault().getLanguage()`. Если начинается с `ru`, `be`, `uk`, `kk`, по умолчанию выбирается `ru`, иначе `en`.
   - Сохранённый пользователем выбор в `SharedPreferences` имеет наивысший приоритет.
2. **Диалог параметров и твиков ([ArgsTweaksDialog.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/ArgsTweaksDialog.java)):**
   - Полностью выдержан стиль тёмной темы лончера (карточки, акценты, поддержка фокуса D-pad / геймпада).
   - Разбит на аккуратные секции:
     - *Плотность мира и трафик:* `--gta4_traffic_density_scale` (0.00–1.00), `--gta4_parked_car_density_scale` (0.00–1.00), `--gta4_ped_density_scale` (0.00–1.00), `--gta4_scenario_ped_density_scale` (0.00–1.00).
     - *Дальность и тени:* `--gta4_shadow_distance_scale` (0.0–2.0x), `--gta4_native_shadow_min_indices` (0–500), `--gta4_dynamic_draw_distance` (чекбокс), `--gta4_dynamic_draw_distance_min` (0.10–1.00).
     - *Очереди кадров и синхронизация:* `--gta4_fps_guard` (0–60 FPS с отметкой `0 (Выкл)`), `--gta4_native_max_queued_frames` (1–4), `--gta4_native_frame_complete_signal` (0–3), `--gta4_native_async_constant_release` (чекбокс), `--vulkan_allow_present_mode_mailbox` (чекбокс), `--vulkan_prefer_present_mode_fifo` (чекбокс).
     - *Звук и системные опции:* `--audio_maxqframes` (16–128), `--android_perf_hint` (чекбокс), `--android_pin_hot_threads` (чекбокс).
   - Все отладочные / профилировочные флаги (`--gta4_native_frame_timeline`, `--gta4_native_hitch_log`, `--gta4_profile_native_*`, `--diagnostics*`, `--log_level`) скрыты от пользователя, но гарантированно сохраняются нетронутыми при перезаписи `args.txt`.
   - Индикаторы ползунков обновляются в реальном времени при перемещении (live text display).
   - Добавлены кнопки «Сброс» (возврат к дефолтам), «Отмена» и «Сохранить».
   - Защита от ошибок прав доступа: если файл `args.txt` был ранее залит через ADB под пользователем `shell`, приложение удаляет и пересоздаёт его от имени `u0_aXX`.
3. **Кнопка в окне настроек ([LauncherActivity.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java)):**
   - Кнопка `Параметры и твики args.txt` добавлена сразу под кнопкой `Привязка потоков CPU` с идентичным стилем оформления, фокусом и отступами.
4. **Сборка и тестирование на устройстве 2dc3ec42:**
   - Сборка `./os/android/scripts/build_apk.sh debug` прошла успешно (`BUILD SUCCESSFUL in 15s`, `app-debug.apk` 147.1 МБ).
   - Установлено на телефон через `adb install -r`.
   - Проверено переключение ползунка плотности трафика (0.40 -> 0.15), проверка сохранения в `/sdcard/Android/data/com.libertyrecomp/files/args.txt` (значение успешно записалось с сохранением всех комментариев и флагов).
   - Проверена работа кнопки «Сброс» (значения сбросились обратно на 0.40 и сохранились).
5. **Обновлены `.diff` файлы:**
   - `os/android/app/build.gradle.kts.diff`
   - `os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java.diff`
   - `os/android/app/src/main/java/com/libertyrecomp/LibertyActivity.java.diff`
   - `os/android/app/src/main/assets/default_args.txt.diff`

## 21. Стрипованная релизная сборка (Release APK) (29.10.2026)

### Что сделано:
1. **Стриппинг нативных библиотек ([build_native.sh](file:///G:/cod/LibertyRecompAndroid/os/android/scripts/build_native.sh)):**
   - Выполнена сборка и стейджинг с флагом `STRIP=1` через `$NDK_BIN/llvm-strip --strip-unneeded`.
   - В скрипте также добавлена поддержка стриппинга `libc++_shared.so` при `STRIP=1`.
   - Размеры нативных библиотек до и после стриппинга:
     - `libmain.so`: 200.4 МБ -> **40.5 МБ**
     - `librexgpu-gta4-native.so`: 80.3 МБ -> **29.4 МБ**
     - `librexruntime.so`: 96.2 МБ -> **8.8 МБ**
     - `librexgpu-xenos.so`: 45.4 МБ -> **4.5 МБ**
     - `libSDL3.so`: 8.5 МБ -> **1.6 МБ**
     - `libc++_shared.so`: 9.5 МБ -> **1.4 МБ**
     - `libliberty_install.so`: 3.2 МБ -> **226 КБ**
   - Суммарный объём нативных библиотек снизился с ~443 МБ до **~86 МБ**.
2. **Сборка релизного APK ([build_apk.sh](file:///G:/cod/LibertyRecompAndroid/os/android/scripts/build_apk.sh)):**
   - Выполнена сборка `./os/android/scripts/build_apk.sh release`.
   - Gradle собрал задачу `assembleRelease` (`BUILD SUCCESSFUL in 26s`).
   - Итоговый файл: `os/android/app/build/outputs/apk/release/app-release.apk`.
   - Размер APK: **42.0 МБ** (44 080 418 байт, для сравнения debug APK весил 147.1 МБ — уменьшение более чем в 3.5 раза).
3. **Верификация на устройстве (2dc3ec42):**
   - Установлен через `adb install -r`.
   - Запущен `LauncherActivity`: все функции, язык по локали, карточки и диалог параметров `args.txt` работают штатно и быстро.
4. **Обновлены `.diff` файлы:**
   - `os/android/scripts/build_native.sh.diff`

## 22. Применение патча из Outsider_memory («какие-то правки.txt») и сборка Debug APK (29.10.2026)

### Что сделано:
1. **Интеграция изменений в `glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp`:**
   - **Пункт 1 (`KeepCaptured` в `CaptureBufferResource`):**
     - В ветку чистого кэша буферов `can_reuse_clean_capture && matching_cache_entry` добавлен вызов `KeepCaptured(std::move(matching_cache_entry), cache_slot.alias, cache_slot.alias_serial)` под условием `REXCVAR_GET(gta4_native_capture_keepalive)`.
     - Гарантирует, что переиспользуемый буфер регистрируется в keepalive кадра так же, как во фронт-кэше.
   - **Пункт 2 (`state_transport` в `ValidateAndCopyCommand`):**
     - Чтение CVar `gta4_native_state_transport` заменено на `static const bool snapshot_transport` и `static const bool compare_transport`.
     - Поскольку CVar помечен `kRequiresRestart`, это устранило тысячи строковых аллокаций и проверок в секунду на главном гостевом потоке `RenderGuest`.
   - **Пункт 3 (`SnapshotDrawConstants`):**
     - Вычисление префикса `bytes = NativeConstantUploadPrefix(shader, bank_size)` перенесено до проверки `materialized_bytes`.
     - Проверка `version->materialized_bytes >= bytes` предотвращает ложные отказы снапшота и лишнюю повторную материализацию.
   - **Пункт 4 (`ClearNativeFrameCommands`):**
     - Под флагом `gta4_native_retire_stats` добавлен проход по `current_frame_` с подсчетом `total_captured_streams` (через `std::popcount(cmd.captured_stream_mask)`), `total_required_streams` (через `GetRequiredVertexStreams`), `total_mesh_draws` и `total_stream0_only_draws`.
     - Статистика стримов и `buffer_resources_epoch_` логируются каждые 300 кадров в сообщении `gta4-native-retire`.
2. **Обновлен `.diff` файл:**
   - `glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp.diff`.
3. **Сборка и развертывание:**
   - `build_native.sh`: сборка прошла успешно (`libmain.so`, `librexgpu-gta4-native.so` скомпилированы и слинкованы, все 725 хуков на месте).
   - `build_apk.sh debug`: сборка успешна (`BUILD SUCCESSFUL in 20s`, `app-debug.apk` 147.1 МБ).
   - Развернуто на устройстве `2dc3ec42` через `adb install -r`.
   - Проверен успешный запуск `LauncherActivity`.

## 23. Приведение названий трейс-маркеров (Perfetto / ATrace) к смысловым именам (29.10.2026)

### Что сделано:
1. **Анализ предложения из переписки (скриншот):**
   - Были выявлены неоднозначные названия слайсов в `graphics_system.cpp`, вызывавшие путаницу при анализе трейсов в Perfetto (в частности, маркер `Rec: wait for job` на потоке `GtaRecorder` ошибочно воспринимался как ожидание физических/гостевых джобов `RagePhysics`/`RageJobWork`).
   - На самом деле `GtaRecorder` ожидает исключительно `kPresent` джоб кадра от `RenderWork`, что в логах уже именуется как `rec-idle` (`g_wait_rec_job_ticks`).
2. **Правки в [graphics_system.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp):**
   - Строка ~6796 (`RenderWorkerMain`):
     - `TRACE_SCOPE("RW: wait for commands");` -> `TRACE_SCOPE("RW: idle (await queue)");`
   - Строка ~7874 (`FrameRecorderMain`):
     - Добавлен поясняющий комментарий: `// Idle until RenderWork posts kPresent – not guest physics/jobs.`
     - `TRACE_SCOPE("Rec: wait for job");` -> `TRACE_SCOPE("Rec: idle (await present job)");` (выровнено с логом `waits ms/f: rec-idle=...`).
   - Строка ~7910 (`WaitForFrameRecorderIdle`):
     - `TRACE_SCOPE("RW: wait recorder idle");` -> `TRACE_SCOPE("RW: wait recorder free");`
   - `RG: backpressure wait` оставлен без изменений (как и указано в скриншоте).
3. **Обновлен `.diff` файл:**
   - [graphics_system.cpp.diff](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp.diff).
4. **Сборка:**
   - Сборка нативных библиотек отменена по запросу пользователя. Все изменения внесены в исходники и зафиксированы в diff-файле.

## 24. Интеграция патча 24 из Outsider_memory (05:04-31) (29.10.2026)

### Что сделано:
1. **Дедупликация `KeepCaptured` за O(1) в [graphics_system.h](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h):**
   - В структуры `NativeBufferResource` и `NativeTextureResource` добавлено поле `mutable uint32_t keepalive_serial = 0;`.
   - В шаблонном методе `KeepCaptured` внедрена проверка:
     ```cpp
     if (raw->keepalive_serial != capture_keepalive_serial_) {
       raw->keepalive_serial = capture_keepalive_serial_;
       capture_keepalive_->refs.push_back(std::move(strong));
     }
     ```
   - Предотвращает раздувание вектора ссылок `capture_keepalive_->refs` при многократном использовании одного и того же ресурса в draw call'ах кадра и исключает ложные роллы `RollCaptureKeepAlive()` по лимиту 32768.
2. **Прореживание и защита сбора статистики стримов в [graphics_system.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp):**
   - В функции `ClearNativeFrameCommands` цикл обхода всех команд кадра `for (const NativeCommand& cmd : current_frame_)` с вызовами `std::popcount` и `GetRequiredVertexStreams` перенесён под условие 300-го кадра `if (log_retire_stats)`.
   - Устранена холостая нагрузка в 299 кадрах из 300 (экономия ~4% CPU на потоке `GtaRecorder`).
   - Исправлена работа счётчика `frames` при выключенном CVar:
     ```cpp
     const bool retire_stats_enabled = REXCVAR_GET(gta4_native_retire_stats);
     static uint32_t frames = 0;
     if (!retire_stats_enabled) {
       frames = 0;
     }
     const bool log_retire_stats = retire_stats_enabled && (++frames >= 300);
     ```
     При выключенном флаге счётчик не растёт и сбрасывается в 0, а при включении ожидает полные 300 кадров.
3. **Обновлены `.diff` файлы:**
   - [graphics_system.h.diff](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h.diff)
   - [graphics_system.cpp.diff](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp.diff)
4. **Сборка:** не запускалась (в соответствии с указаниями пользователя).

5. **Сборка и развертывание на устройстве (29.10.2026 06:09):**
   - Выполнена сборка нативных библиотек `./os/android/scripts/build_native.sh`: код возврата 0 (`libmain.so`, `librexgpu-gta4-native.so` собраны, 725 хуков на месте).
   - Выполнена сборка отладочного APK `./os/android/scripts/build_apk.sh debug`: Gradle `BUILD SUCCESSFUL in 18s`, итоговый файл `app-debug.apk` (147.1 МБ).
   - Установлено на устройство `2dc3ec42` через `adb install -r`: результат `Success`.
   - Запущен `LauncherActivity`: запуск чистый, ресурсы скопированы, ошибок нет.

## 25. Добавление суффикса `_modded` к версии лончера (29.10.2026 06:13)

### Что сделано:
1. **Конфигурация сборки ([build.gradle.kts](file:///G:/cod/LibertyRecompAndroid/os/android/app/build.gradle.kts)):**
   - Параметр `versionName` изменён с `"0.5.9.3"` на `"0.5.9.3_modded"`.
2. **Интерфейс лончера ([LauncherActivity.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java)):**
   - В методе `versionLine()` добавлена проверка и суффикс `_modded`:
     ```java
     if (version != null && !version.endsWith("_modded")) {
         version += "_modded";
     }
     ```
   - Строка версии в шапке лончера теперь отображает `v0.5.9.3_modded  ·  ...`.
3. **Обновлены `.diff` файлы:**
   - [build.gradle.kts.diff](file:///G:/cod/LibertyRecompAndroid/os/android/app/build.gradle.kts.diff)
   - [LauncherActivity.java.diff](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java.diff)
4. **Сборка:** не запускалась по прямому указанию пользователя.

## 26. Развертывание последнего отладочного APK на второе устройство (29.10.2026 06:16)

### Что сделано:
1. **Обнаружение устройства:** к ПК подключено устройство `cb4e8c0a` (модель GM1901 / OnePlus 7, платформа `msmnile` / Snapdragon 855, Android 15).
2. **Развертывание APK:**
   - Установлен последний собранный отладочный APK `app-debug.apk` (147.1 МБ от 06:09:19) через `adb -s cb4e8c0a install -r`: результат `Success`.
3. **Верификация запуска:**
   - Выполнен запуск `LauncherActivity` (`am start -n com.libertyrecomp/.LauncherActivity`).
   - Лог чистый: ресурсы скопированы в `/data/user/0/com.libertyrecomp/files/Resources`, ошибок нет.

## 27. Анализ архитектуры аудиоподсистемы (XMA2 / FFmpeg) и план автономного тестирования на ПК (29.10.2026 06:48)

### Анализ архитектуры:
1. **Железо vs Эмуляция:**
   - На оригинальном Xbox 360 аппаратный сопроцессор XMA декодировал до 64 стерео (или 128 моно) потоков сжатого звука XMA/XMA2 параллельно и без затрат CPU.
   - В ReXGlue декодирование возложено на программный поток `XMA Decoder` (`xma_decoder.cpp`), использующий библиотеку FFmpeg (`libavcodec`, кодеки `AV_CODEC_ID_XMAFRAMES` / `AV_CODEC_ID_XMA2`).
   - Декодированные плавающие сэмплы (planar float) в `XmaContext::Decode()` масштабируются, насыщаются (clamping) и упаковываются в interleaved stereo PCM в ринг-буфер для вывода через SDL3 / AAudio.
2. **Ключевые узкие места, вызывающие спайки нагрузки и аудио-статтеры на мобильных CPU:**
   - **Частый malloc/free контекстов FFmpeg (`xma_decoder_lifecycle.h`):** функция `PrepareXmaDecoderForStream` при сбросе голоса или старте нового звука полностью уничтожает старый контекст (`avcodec_free_context`) и создает новый с нуля (`avcodec_alloc_context3` + `avcodec_open2`). Это вызывает сотни тяжёлых аллокаций памяти в секунду прямо во время перестрелок и динамичных сцен.
   - **Скалярный интерливинг и клиппинг в `XmaContext::Decode`:** скалярная обработка сэмплов с поштучным вычислением сатурации.
3. **План автономного бенчмарка на ПК (`tools/`):**
   - На базе существующих тестов (`test_xma_loop_playback.cpp`, `test_xma_decoder_reset.cpp`) создать изолированный стенд тестирования XMA декодера на хосте.
   - Запустить синтетический стресс-тест с симуляцией 32–64 одновременных потоков.
   - Замерить время декодирования кадра, частоту аллокаций памяти и протестировать переиспользование (recycle pool) контекстов `AVCodecContext`, а также векторизацию интерливинга (NEON / AVX2).

## 28. Сборка и развертывание релизного стрипнутого APK (v0.5.9.3_modded) (29.10.2026 06:50)

### Что сделано:
1. **Стриппинг нативных библиотек ([build_native.sh](file:///G:/cod/LibertyRecompAndroid/os/android/scripts/build_native.sh)):**
   - Выполнен `STRIP=1 ./os/android/scripts/build_native.sh`.
   - Библиотеки пожаты `llvm-strip --strip-unneeded`: суммарный вес упал с ~443 МБ до ~85 МБ (`libmain.so`: 40.5 МБ, `librexgpu-gta4-native.so`: 29.4 МБ).
2. **Сборка релизного APK ([build_apk.sh](file:///G:/cod/LibertyRecompAndroid/os/android/scripts/build_apk.sh)):**
   - Сборка `./os/android/scripts/build_apk.sh release` прошла успешно (`BUILD SUCCESSFUL in 42s`).
   - Итоговый размер: **42.0 МБ** (`app-release.apk`, 44 081 410 байт).
   - Включает все последние нативные оптимизации (24 trace audit fixes: O(1) keepalive dedup, 300-frame retire stats thinning), переименование маркеров Perfetto и обновлённую версию `v0.5.9.3_modded`.
3. **Развёртывание и верификация на устройстве (`2dc3ec42`):**
   - Установлен через `adb install -r`: результат `Success`.
   - Запущен `LauncherActivity`.
   - Сделан скриншот экрана: в строке версии отображается `v0.5.9.3_modded · MT2111 · SM8350 · Adreno660v2`.


## 29. [ВАЖНО] Комплексный технический анализ пулов потоков RageJobWork и RagePhysics (29.10.2026 07:35)

> [!IMPORTANT]
> **Критическое узкое место архитектуры многопоточности RAGE на Android:**
> Потоки `RageJobWork` (3 потока) и `RagePhysics` (3 потока) выполняют микроскопические задачи длительностью 5–30 мкс каждая.
> Из-за трансляции оригинальных примитивов синхронизации Xbox 360 (`NtWaitForSingleObjectEx` / `NtSetEvent`) в системные вызовы ядра Linux `futex(FUTEX_WAIT)` / `futex(FUTEX_WAKE)`, в игре происходит **~3 100 переходов сон/пробуждение в секунду** (более 61 000 за 20 секунд трейса).
> Потоки проводят свыше **4,1 секунд в статусе `R` (Runnable)** в очереди планировщика Linux. Задержка пробуждения воркеров блокирует `Main XThread` на 3–5 мс каждый кадр, за которым простаивает `RenderGuest`.

### 1. Точки создания и идентификация потоков
1. **Создание гостевых потоков:**
   - Потоки создаются игровым движком RAGE через вызов функции Xbox 360 API `ExCreateThread` (`0x82A752E4`), рекомпилированной в [gta4_recomp.85.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.85.cpp#L29835-L29842).
   - Прослойка ядра [xboxkrnl_threading.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_threading.cpp#L132-L175) (`ExCreateThread_entry`) инстанциирует гостевой поток `rex::system::XThread`.
2. **Перехват имени при старте (`XThread::Execute`):**
   - В [xthread.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/system/xthread.cpp#L675-L706) при входе в поток рекомпилятор анализирует гостевой параметр контекста `r4` (структура потока RAGE):
     - `0x8285D368` $\to$ именуется **`RageJobWork`** ([xthread.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/system/xthread.cpp#L695-L696));
     - `0x82798BA0` / `0x827989F8` $\to$ именуется **`RagePhysics`** ([xthread.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/system/xthread.cpp#L697-L698)).
3. **Количество потоков в рантайме:**
   - Создаётся **ровно по 3 потока** каждого типа (3 воркера `RageJobWork` и 3 воркера `RagePhysics`). Это наследие симметричной архитектуры CPU консоли Xbox 360 Xenon (3 физических PowerPC ядра по 2 SMT потока — по одному воркеру на каждое физическое ядро).

### 2. Функциональное назначение потоков в движке RAGE
1. **`RageJobWork` (пул `sysJobQueue` / `sysJobWorker`):**
   - **Скелетная анимация и инверсная кинематика (IK):** расчёт матриц трансформации костей персонажей, блендинг анимаций ходьбы, стрельбы, посадки в транспорт (`sub_821D6260` в [gta4_recomp.5.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.5.cpp#L41822-L41950), использующий софтверную эмуляцию VMX/AltiVec SIMD-регистров).
   - **Анимация лиц и мимики:** морфинг вершин мешей лиц персонажей при речи.
   - **AI навигация толпы и транспорта:** поиск путей по навигационной сетке (navmesh).
   - **Процедурная деформация ткани и динамические частицы.**
2. **`RagePhysics` (пул физики `phSimulationWorld` / NaturalMotion Euphoria):**
   - **Euphoria Dynamic Ragdoll:** расчёт динамических сил и реакций скелетов персонажей (потеря баланса, падения с высоты, столкновения с бампером, ранения).
   - **Физика твёрдых тел (`phBound` / `phInst`):** деформация транспорта, динамика переворотов, отлетающие объекты окружения.
   - **Raycasting подвески колес и трассировка видимости (Line-of-Sight).**

### 3. Анализ кода рабочих циклов и синхронизации
1. **Воркер `RageJobWork` (`sub_8285D368` в [gta4_recomp.66.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.66.cpp#L47747-L47830)):**
   - `sub_828497D8` ([gta4_recomp.65.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.65.cpp#L77287-L77329)) $\to$ ожидает семафор наличия задач (`NtWaitForSingleObjectEx`, INFINITE).
   - `sub_8285FE78` ([gta4_recomp.66.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.66.cpp#L54500-L54513)) $\to$ блокирует критическую секцию очереди (`RtlEnterCriticalSection`).
   - Извлекает дескриптор джоба (156 байт) из кольцевого буфера.
   - `sub_8285FEE0` ([gta4_recomp.66.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.66.cpp#L54570-L54580)) $\to$ освобождает критическую секцию (`RtlLeaveCriticalSection`).
   - `rexcrt_memcpy` $\to$ копирует дескриптор задачи на локальный стек.
   - `REX_CALL_INDIRECT_FUNC` $\to$ вызывает саму функцию задачи по функциональному указателю.
   - `sub_82A13160` ([gta4_recomp.85.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.85.cpp#L11615-L11639)) $\to$ вызывает `NtSetEvent`, сигнализируя главному потоку о готовности джоба.
   - Возврат на ожидание следующей задачи.
2. **Воркер `RagePhysics` (`sub_827989F8` в [gta4_recomp.59.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.59.cpp#L54240-L54408)):**
   - `sub_82798038` ([gta4_recomp.59.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.59.cpp#L52618-L52720)) $\to$ потокобезопасная выборка из связанного списка задач физики. Захватывает мьютекс (`sub_8285FF50`), при пустой очереди спит на семафоре (`sub_828497D8 -> NtWaitForSingleObjectEx`), после пробуждения снова берёт мьютекс (`sub_8285FEF8`), декеит задачу и отпускает мьютекс (`sub_8285FFA0`).
   - `REX_CALL_INDIRECT_FUNC` $\to$ выполняет итерацию солвера / рэгдолла.
   - `sub_82798470` / `sub_82797F20` ([gta4_recomp.59.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.59.cpp#L52443-L52530)) $\to$ декрементирует остаток задач; если 0 — вызывает `sub_82797DB8` $\to$ `NtReleaseSemaphore` ([gta4_recomp.85.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.85.cpp#L11261-L11276)), разблокируя `Main XThread`.

### 4. Почему это критическая проблема производительности (данные Perfetto Traces)
1. **Огромная частота обращений к ядру Linux:**
   - Вызов `NtWaitForSingleObjectEx` транслируется в [xobject.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/system/xobject.cpp#L203-L218) $\to$ [threading_posix.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/core/threading_posix.cpp#L296-L334), где под капотом лежат `std::mutex` и `std::condition_variable` на базе Bionic libc `syscall(__NR_futex, ..., FUTEX_WAIT/FUTEX_WAKE)`.
   - За 20 с геймплея 3 потока `RageJobWork` совершают **49 560 переходов сон/пробуждение (~2 478 переходов в секунду)**.
   - 3 потока `RagePhysics` совершают **12 292 переходов сон/пробуждение (~614 переходов в секунду)**.
   - Суммарно воркеры провели **4 180 мс в статусе `R` (Runnable)** — они ожидали выделения квантов планировщиком ядра Linux из-за перегрузки очереди задач.
2. **Блокировка `Main XThread`:**
   - Главный поток игры распределяет задачи и встаёт на блокирующее ожидание завершения фазы кадра. Из-за wake-up latency на Little-ядрах (100–150 мкс на каждый контекст-свитч) главный поток простаивает по 3–5 мс каждый кадр, задерживая `RenderGuest`.
3. **Конкуренция на ядрах мобильного SoC:**
   - В [android_thread_pinning.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp#L257-L258) потоки привязаны к `Kind::kJobsLane` (Little-ядра 0–3 + нижние Big-ядра 4–5). На Little-ядрах они непрерывно конкурируют с `GtaCmdRetirer`, звуком, системными процессами Android, вызывая тысячи межкластерных миграций.

### 5. Сводка файлов
| Файл | Строки | Назначение |
|---|---|---|
| [xthread.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/system/xthread.cpp#L689-L706) | 689–706 | Перехват `r4`, идентификация точек входа `0x8285D368`, `0x82798BA0`, именование потоков. |
| [gta4_recomp.66.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.66.cpp#L47725-L47836) | 47725–47836 | `sub_8285D368` — рабочий цикл пула `RageJobWork`. |
| [gta4_recomp.59.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.59.cpp#L54240-L54408) | 54240–54408 | `sub_827989F8` — рабочий цикл пула физики `RagePhysics`. |
| [gta4_recomp.59.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.59.cpp#L52618-L52720) | 52618–52720 | `sub_82798038` — потокобезопасная очередь физических задач. |
| [gta4_recomp.85.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.85.cpp#L29835-L29842) | 29835–29842 | Создание гостевых потоков `__imp__ExCreateThread`. |
| [gta4_recomp.5.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.5.cpp#L41822-L41950) | 41822–41950 | Математика скелетной анимации / VMX SIMD костей (`sub_821D6260`). |
| [xboxkrnl_threading.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_threading.cpp#L1033-L1075) | 1033–1075 | Системные вызовы `NtWaitForSingleObjectEx`, `KeWaitForMultipleObjects`. |
| [threading_posix.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/core/threading_posix.cpp#L296-L334) | 296–334 | Реализация ожидания на базе `std::mutex` и `futex`. |
| [android_thread_pinning.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp#L313-L318) | 313–318 | Конфигурация раскладки потоков (`kJobsLane`, `nice` -6). |

### 6. Рекомендации по оптимизации
1. **Adaptive Spin-Wait в `rex::thread::Wait` (наибольший эффект):**
   Ввести короткий адаптивный спинлок (~20–40 мкс с `__builtin_arm_yield()`) перед вызовом `futex(FUTEX_WAIT)`. Так как задачи в кадре сыплются непрерывно, воркер перехватит следующую задачу мгновенно без ухода в сон и без контекст-свитча ядра (потенциал устранения до 70–80% системных переключений).
2. **Векторизация математики VMX на ARM NEON:**
   Замена софтверной эмуляции VMX регистров в функциях скелетной анимации (`sub_821D6260`) на прямые ARM NEON интринсики (`vld1q_f32`, `vmlaq_f32`).
3. **Coalescing сигналов `NtSetEvent`:**
   Пакетное уведомление о завершении группы связанных джобов вместо поштучной отправки событий на каждую кость.


## 30. Аудит карты потоков android_thread_pinning и выверенная иерархия узких мест (29.10.2026 08:10)

### 1. Состав и функциональные блоки потоков (все 20 классов):
1. **Рендеринг и трансляция:**
   - `RenderGuest` (`sub_821B6F18`): главный гостевой поток рендеринга GTA IV;
   - `GtaRecorder` (`kRecorder` / `kRecorderHelpers`): запись нативных команд Vulkan;
   - `GtaRenderWork` (`kRenderWork`): трансляция команд в вызовы Vulkan API.
2. **Логика и параллельные вычисления:**
   - `Main XThread`: главный цикл логики игры;
   - `RageJobWork` (3 потока, `0x8285D368`): анимации, IK, AI толпы, морфинг мешей;
   - `RagePhysics` (3 потока, `0x82798BA0`/`0x827989F8`): Euphoria ragdoll, жесткие тела, raycasting подвески.
3. **Стриминг и дисковый ввод-вывод (I/O):**
   - `RageStream` (`0x82902728` в [gta4_recomp.73.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.73.cpp#L1884)): менеджер потоковой загрузки секторов карты, текстур WTD, коллизий WBD (`strStreaming`);
   - `RageFileIO` (`0x8284C5E8` в [gta4_recomp.66.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.66.cpp#L5339)): асинхронный дисковый поток чтения пакетов архивов `.img`/`.rpf` (`fiDevice`);
   - `Kernel IO` (`Kernel Host Tasks`, `Kernel File IO [0..N]` в [kernel_state.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/system/kernel_state.cpp#L1323)): хостовые POSIX I/O вызовы к хранилищу Android.
4. **Звуковой стек:**
   - `AudioGuest` (`0x821909D0` в [gta4_recomp.2.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.2.cpp#L70767)): гостевой звуковой микшер RAGE (3D звук, радио, реплики);
   - `XMA Decoder` ([xma_decoder.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/audio/xma_decoder.cpp#L143)): программный декодер сжатых пакетов XMA/XMA2 через FFmpeg;
   - `Audio Worker` ([audio_system.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/audio/audio_system.cpp#L112)): хостовый микшер ReXAudio (интерливинг, клиппинг в ринг-буфер);
   - `Host Audio` (`Track` / `SDLAudio`): системный вывод Android AudioTrack / AAudio (`SCHED_FIFO`, пиннинг намеренно не трогает).
5. **Презентация и UI:**
   - `SDLThread`: нативный поток SDL (`SDL_main`), инициализация окна и презентация готовых кадров;
   - `SDLActivity`: Java UI поток Android (обработка тачей, паузы, жизненный цикл);
   - `sdl_helpers`: вспомогательные потоки таймеров/событий SDL.
6. **Специализированные безымянные гостевые потоки (`Rage_%06X` / `kRageOther`):**
   - `Rage_299490` (`0x82299490` в [gta4_recomp.12.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.12.cpp#L55444));
   - `Rage_1BA088` (`0x821BA088` в [gta4_recomp.4.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.4.cpp#L30243));
   - `Rage_556188` (`0x82556188` в [gta4_recomp.39.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.39.cpp#L14051));
   - `Rage_3A05E8` (`0x823A05E8` в [gta4_recomp.23.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.23.cpp#L13368)).
   *(Назначения потоков оцениваются по контексту дизассемблера как гипотезы, поскольку официальные отладочные символы отсутствуют).*
7. **Служебные:**
   - `GtaCmdRetirer` ([native_owned_commands.h](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h#L365)): сборщик мусора отработавших команд рендера в слэб-пул;
   - `kBackground`: системные таймеры и Binder IPC.

### 2. Корректировка оценок нагрузки и раскладки по ядрам:
- **Little-кластер:**
  - 19 секунд суммарного времени работы потоков на фоне 60 секунд общего бюджета 3 ядер Cortex-A55 составляет **~32% загрузки**, а не критические 90%.
  - `AudioGuest` в схеме `lanes` привязан к `kAudioLane` (младшие Big-ядра 4–5 на SM8350 / 3–4 на Gen 5) с приоритетом `nice -16`, поэтому относить его к нагрузке на Little-ядра некорректно.
  - `XMA Decoder` аналогично размазан по `kAudioLane` / workers.
  - Воркеры `RageJobWork` и `RagePhysics` работают в `kJobsLane` (`little | big_low`), распределяя выполнение между Little и нижними Big ядрами.
  - Очереди `Runnable` (`R`) у воркеров обусловлены в первую очередь частотой синхронизации (`futex`/CS) и блокировками, а не тотальным физическим исчерпанием мощности Little-кластера.

### 3. Итоговая выверенная иерархия нагрузки и узких мест (по трейсам 888 / Gen 5):
| Поток | Порядок Running | Назначение в `lanes` | Роль в производительности |
|---|---|---|---|
| `RenderGuest` / `GtaRecorder` / `GtaRenderWork` | **Топ** | `Big` / `Prime` | **Главное узкое место CPU:** здесь формируется и рендерится кадр. |
| `RageJobWork` | **Высокий** | `JobsLane` (`little \| big_low`) | Задержки очередей и микро-синхронизации, тормозящие `Main XThread`. |
| `AudioGuest` | **Высокий** | `AudioLane` (`big_low`, nice -16) | Тяжёлый гостевой микшер, надёжно изолирован от рекордера. |
| `RageStream` | **Средний** | `Little` (nice 0) | Фоновый стриминг мира (шум, не определяющий FPS). |
| `GtaCmdRetirer` | **Средний** | `Little` (nice 0) | Очистка команд (нагрузка снижена с помощью keepalive). |
| `RagePhysics` | **Ниже JobWork** | `JobsLane` (`little \| big_low`) | Физический шаг симуляции. |
| `Host Audio` | — | Не трогаем (`kNone`) | Системный аудиовывод Android HAL (`SCHED_FIFO`). |

> [!NOTE]
> На младших чипсетах (в частности Snapdragon 855 / Adreno 640 на OnePlus 7) ключевым ограничивающим фактором часто выступает не CPU-шедулинг, а пропускная способность и филлрейт GPU.


---

## 31. Интеграция Пакета 35, устранение Thundering Herd, False Sharing и fast-path стримов

В кодовую базу проекта полностью интегрированы оптимизации из Пакета 35 ([`26_BATCH_35_OCT_TARGETED_WAKEUPS_AND_SPIN/`](file:///G:/cod/LibertyRecompAndroid/Outsider_memory/26_BATCH_35_OCT_TARGETED_WAKEUPS_AND_SPIN/)) и аналитической записки [`25_ANALYSIS_PATCH_24_TRI_PLATFORM_AND_STREAM0.txt`](file:///G:/cod/LibertyRecompAndroid/Outsider_memory/25_ANALYSIS_PATCH_24_TRI_PLATFORM_AND_STREAM0.txt) (бывший `new_pasta.txt`).

### 1. Подсистема синхронизации и пулов потоков (`threading_posix.cpp`, Пакет 35):
* **Устранение «стада пробуждений» (Thundering Herd):**
  * В оригинальном коде `Semaphore::Release(n)`, авто-сброс `Event::Set` и `Mutant::Release` вызывали `cond_.notify_all()`, из-за чего на одну освободившуюся задачу просыпались все 3 воркера `RageJobWork`. 77% пробуждений на SD Gen 5 и 29% на SD 888 длились менее 15 мкс (воркеры сразу засыпали обратно).
  * Внедрены адресные пробуждения: авто-сброс и мутанты будят строго одного воркера, а семафор — ровно $n$ ожидающих.
* **Вынос `futex-wake` из-под мьютекса (`DeferredNotify`):**
  * Ранее сигналящий поток делал `futex-wake`, удерживая `mutex_` объекта, из-за чего разбуженный поток мгновенно блокировался на том же мьютексе и засыпал повторно (Bionic лишен механизма requeue).
  * Теперь пробуждение откладывается и срабатывает строго после `unlock` мьютекса объекта (`FireDeferred`).
* **Адаптивный короткий спин (`REX_WAIT_SPIN_US`):**
  * Ожидающий поток перед уходом в сон до 30 мкс проверяет счетчик эпохи объекта с процессорной инструкцией `isb` (на ARM64) без системных вызовов futex.
  * Бюджет адаптивно настраивается на объект (`spin_score_`), ручка регулируется переменной окружения `REX_WAIT_SPIN_US`.

### 2. Рендерер и память констант (`stateful_constant_state.h`, Пакет 35):
* Откачена двухпроходная функция `CaptureConstantPayloadDelta` из Пакета 34 к проверенной версии 31.10.
* Устранен паразитный `memset` при `resize()`, отнимавший до ~3% процессорного времени потока `RenderGuest` на Snapdragon 888 и 855.

### 3. Рендерер и граф команд (`graphics_system.h` и `graphics_system.cpp` из [`25_ANALYSIS_PATCH_24_TRI_PLATFORM_AND_STREAM0.txt`](file:///G:/cod/LibertyRecompAndroid/Outsider_memory/25_ANALYSIS_PATCH_24_TRI_PLATFORM_AND_STREAM0.txt)):
* **Устранение False Sharing в `NativeTextureResource`:**
  * Поле `keepalive_serial` (активно инкрементируемое потоком `RenderGuest` на Prime/Big ядрах) перенесено в начало структуры к неизменяемым заголовкам (`handle = 0`), изолировав его от поля `assembly_protection_epoch` (записываемого потоком `GtaRenderWork` на средних Big ядрах) в разных 64-байтных кэш-линиях CPU.
* **Аппаратно безопасный fast-path обхода `stream 0`:**
  * На основе 100% доказательства того, что все вершинные декларации игры используют только `maximum_stream == 0` (проверено на 1.3 млн дроуколлов), циклы обхода стримов в `RecordPrimitive` и `RecordIndexedPrimitive` ограничены до `maximum_stream + 1`.
  * В `GetRequiredVertexStreams` внедрен мгновенный возврат `required_streams[0] = true` при `maximum_stream == 0`.
  * В `ValidateAndCopyCommand` холостой опрос стримов 1..15 заменен быстрой 64-битной проверкой `any_hi_stream`.

### 4. Исправления UI лаунчера и аргументов запуска:
* В [ArgsTweaksDialog.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/ArgsTweaksDialog.java):
  * Диапазон сигнала завершения кадра `gta4_native_frame_complete_signal` исправлен с `0..3` на `0..2`.
  * Удален некорректный параметр `gta4_fps_guard` (вместе с привязкой из [default_args.txt](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/assets/default_args.txt)).
  * Добавлен дискретный выбор раскладок CPU (`android_thread_layout`: lanes, lanes2, clusters, priority, off).
* В [ThreadAffinityDialog.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/ThreadAffinityDialog.java):
  * Добавлена полная поддержка раскладки `L_LANES2` (Main на Big, RenderGuest nice -10, Jobs nice -6).

### 5. Сборка и деплой:
* Нативные библиотеки скомпилированы и синхронизированы в `jniLibs/arm64-v8a`.
* APK собран (`gradlew assembleDebug`) и установлен на целевое устройство (`2dc3ec42`). `args.txt` на устройстве очищен от устаревших ключей.

---

## 32. Рефакторинг настроек производительности лаунчера и локализация (29.10.2026)

### 1. Архитектура меню производительности:
* В [LauncherActivity.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java):
  * Главное диалоговое окно «Настройки производительности» (`showPerformanceTestSettings`) реорганизовано в компактный хаб с тремя основными кнопками:
    1. **Привязка потоков CPU** (`ThreadAffinityDialog`) — выбор раскладки и ручной пиннинг ядер.
    2. **Параметры и твики args.txt** (`ArgsTweaksDialog`) — плотность трафика/пешеходов, тени, очереди кадров GPU, аудио-буфер, layout.
    3. **Тестовые хаки графики** (`showLegacyTestSettingsDialog`) — старые оверрайды (дальность 0.5, отключение теней солнца, GPU Turbo, Sustained mode, Thermal Guard, Safe mode, интервалы отражений воды/транспорта).
    4. Кнопка **Закрыть** (`btn_close`).
  * Размер хаба адаптирован под `WRAP_CONTENT` по высоте вместо растягивания на 90% экрана.
  * Текст на 3-й кнопке формируется динамически (`legacyPerfSummary()`): отображает статус по умолчанию либо число активных оверрайдов (`Включены (N)` / `Active (N)`), мгновенно обновляясь через `onSaved` коллбэк.
  * Все ключи `SharedPreferences` (`perfTestPreferences`) и логика передачи через `Intent extras` в `LibertyActivity` полностью сохранены без изменений.

### 2. Исправление локализации и английских fallback-строк:
* В [ArgsTweaksDialog.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/ArgsTweaksDialog.java):
  * Исправлены все 40 вызовов `tr(key, fallbackEn)`: кириллические fallback-строки заменены на естественные технические термины на английском языке.
  * В `buildLayoutRow` добавлена локализация меток раскладок (`layoutLabels`) в зависимости от текущего языка (`LauncherI18n.isRu(ctx)`).
* В [LauncherI18n.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherI18n.java):
  * Добавлены недостающие русские переводы для `btn_close`, `legacy_perf_title`, `legacy_perf_summary_default` и `legacy_perf_summary_active`.
  * Проведена автоматическая валидация через `check_keys.py`: непереведённых строк или ошибочных fallback в коде лаунчера не осталось.

### 3. Сборка и тестирование на устройстве:
* Собрана новая версия APK (`build_apk.sh debug`), установлена на устройство (`2dc3ec42`).
* Проведено интерактивное UI-тестирование:
  * Проверены режимы English и Русский (переключение на лету).
  * Проверено открытие всех трёх вложенных диалогов.
  * Проверено динамическое обновление счётчика активных оверрайдов при сохранении.

---

## 33. Интерактивный переключатель профилей CPU и отключение внешней ссылки (29.10.2026)

### 1. Интерактивный селектор раскладок в ThreadAffinityDialog:
* В [ThreadAffinityDialog.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/ThreadAffinityDialog.java):
  * Вместо статической строчки описания профиля добавлена интерактивная кнопка `[ Профиль раскладки CPU: <name> ▾ ]` прямо под блоком топологии ядер.
  * По клику открывается `AlertDialog` с выбором из 5 профилей:
    1. `lanes` (по умолчанию: рендер на Prime/Big, звук на Big)
    2. `clusters` (по кластерам: задачи по типам ядер)
    3. `priority` (только nice: без масок ядер)
    4. `off` (без масок: решает Android)
    5. `lanes2` (Main на Big, RenderGuest nice -10, Jobs nice -6)
  * При выборе опции:
    * Новое значение немедленно записывается в `args.txt` через метод `writeLayout(ctx, layout)`.
    * Описание профиля обновляется в реальном времени.
    * Таблица потоков ниже моментально пересчитывает и перерисовывает назначения ядер под выбранный профиль (`refresh()`).
    * При закрытии окна текст кнопки в главном меню автоматически обновляется (например, `Привязка потоков CPU: Авто (lanes2)`).

### 2. Отключение открытия GitHub при нажатии на футер:
* В [LauncherActivity.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java):
  * В методе `buildGithubFooter()` полностью убран `setOnClickListener` с переходом на GitHub и отключён фокус/подсветка (`setFocusable(false)`), оставив авторский копирайт `vaduur | ViBetimized by Gemini & GVINDOLIN` статичным информационным элементом, так как основной репозиторий заморожен.

### 3. Сборка и проверка:
* APK пересобран и установлен на `2dc3ec42`.
* Проверено переключение профилей в диалоге привязки ядер (файл `args.txt` на устройстве получил `--android_thread_layout=lanes2`), проверена статичность футера.

---

## 34. Накатка Пакета 36 и рефакторинг UI диалогов (29.10.2026)

### 1. Накатка Пакета 36 (Outsider):
1. **Снятие глобального MSR-лока ([gta4_init.h](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h), [init_h.inja](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/resources/templates/codegen/init_h.inja)):**
   * Все 50 мест вызова `mfmsr`/`mtmsrd` в 89 файлах рекомпиляции обрамляют ровно одну атомарную операцию (`lwarx`/`stwcx.`), транслируемую на ARM64 в процессорный CAS (`__sync_bool_compare_and_swap`).
   * Макросы `REX_ENTER_GLOBAL_LOCK()` и `REX_LEAVE_GLOBAL_LOCK()` переведены в no-op (с сохранением возможности возврата через `#define REX_GUEST_INTERRUPT_LOCK 1`).
   * Устранены задержки и заторы на одной кэш-линии (10–16% CPU у `RageJobWork`, 2.1% у `AudioGuest`, 1.3% у `RageStream`).
2. **Синхронный декод звука XMA ([xma_decoder.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/audio/xma_decoder.cpp)):**
   * Запись в регистр Kick декодирует контекст инлайн прямо в `WriteRegister` на потоке `AudioGuest`.
   * Поток `XMA Decoder` спит в игре. Устранены ~4000 futex-переключений в секунду и вытеснение потоков рендера (`RenderGuest` и `GtaRenderWork`).
   * Включен флаг `--xma_inline_kick=true` по умолчанию.
3. **Мониторинг частот и зон нагрева ([gta4_app.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/src/gta4_app.cpp)):**
   * В строку `thermal:` добавлены средневзвешенные частоты ядер `cpu-avg-mhz=a/b/c` (из `/sys/.../cpufreq/policy*/stats/time_in_state`) и пиковые температуры CPU/DDR без системных лимитеров.
4. **Конфигурация `args.txt` и [default_args.txt](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/assets/default_args.txt):**
   * Добавлены флаги `--android_thermal_log=true` и `--xma_inline_kick=true` при активном `--android_thread_layout=lanes`.
5. **Сборка натива:**
   * Выполнен `./build_native.sh 18` с полной перекомпиляцией всех 89 `.cpp` файлов recomp, слинкованы `libmain.so` (200.5 МБ) и сопутствующие библиотеки (код возврата 0, 725 хуков проверено).

### 2. Рефакторинг UI диалогов:
1. **[ThreadAffinityDialog.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/ThreadAffinityDialog.java):**
   * Кнопка выбора профиля раскладки (`[ Профиль раскладки CPU: <name> ▾ ]`) перемещена в нижний ряд диалога: слева селектор профиля, справа кнопки «Сброс», «Отмена», «Сохранить».
   * Текстовая строка описания автоматического профиля сверху убрана.
   * Добавлен хелпер `tr(...)` для корректной локализации кнопок действий.
2. **[ArgsTweaksDialog.java](file:///G:/cod/LibertyRecompAndroid/os/android/app/src/main/java/com/libertyrecomp/ArgsTweaksDialog.java):**
   * Удален дублирующий пункт настройки «CPU Thread Layout» (`buildLayoutRow`), управление профилями централизовано в диалоге привязки ядер.

### 3. Сборка и деплой на устройство:
* APK успешно собран (`build_apk.sh debug` -> `app-debug.apk` 213 МБ).
* Установлен на устройство `2dc3ec42` через ADB (`Success`).
* Конфиг `args.txt` синхронизирован на устройстве.
* Интерактивно протестированы все диалоги и выбор профилей.

---

## 35. Накатка Пакета 37 и деплой на устройство (29.10.2026)

### 1. Состав Пакета 37 (Outsider):
1. **Оптимизация открытия XMA в FFmpeg ([wmaprodec.c](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/thirdparty/FFmpeg/libavcodec/wmaprodec.c)):**
   * Все 7 статических VLC-таблиц декодирования, синусные окна и таблица `sin64` строятся один раз на процесс через `ff_thread_once`.
   * Инициализация MDCT-преобразований ограничена только используемыми в потоке размерами (`min_samples_per_subframe`..`samples_per_frame`, для XMA это ровно 3 размера вместо 8).
   * Устранена гонка параллельной перестройки VLC-таблиц между потоками звука при одновременном открытии голосов.
   * Ускорение `decode_init` при создании новых звуков в 15–20 раз (448 пар `sin/cos` вместо ~8000).
2. **Разгрузка Main XThread: хуки Player-Info Alias ([gta4_multiplayer_64_policy.h](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/src/gta4_multiplayer_64_policy.h), [gta4_multiplayer_64_hooks.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/src/gta4_multiplayer_64_hooks.cpp)):**
   * Добавлен атомарный счетчик занятых расширенных слотов в `PlayerInfoRegistry` и метод быстрой проверки `HasExtended()`.
   * 10 хуков (`sub_8222A030`, `sub_82718AE8`, `sub_825755A0`, `sub_821F89C0`, `sub_821F8A78`, `sub_823A3DD0`, `sub_82502B88`, `sub_821C1460`, `RunPlayerInfoFloatMinimum`) при отсутствии расширенных игроков (синглплеер) мгновенно возвращаются без копий `PPCContext` и без прохода через гейт/мьютексы.
   * `WithOnlyExtendedPlayerInfo` отсекает пустые слоты до входа в гейт.
   * Эффект: -3–4% нагрузки с `Main XThread`.
3. **Разгрузка Main XThread: gta4::aspect::FinalizeDc ([gta4_aspect_hooks.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/src/gta4_aspect_hooks.cpp)):**
   * Добавлен атомарный счетчик активных макетов `dc_layout_count`.
   * Для команд вне UI (`!ui_context.active`) при нулевом счетчике осуществляется мгновенный выход без `QueryRangeAccess` под глобальным мьютексом.
   * `DcContext` получил аналогичную быструю проверку.
   * Эффект: -3.5% нагрузки с `Main XThread`, устранение очередей за глобальный мьютекс.

### 2. Сборка и деплой:
* Выполнен нативный билд `./build_native.sh 18` (`wmaprodec.c` скомпилирован в `libavcodec.a`, хуки слинкованы в `libmain.so`, проверено 725 хуков, код 0).
* Собрана сборка APK `./build_apk.sh debug` (код 0, `app-debug.apk` 213.1 МБ).
* Установлена на устройство `2dc3ec42` через ADB (`Streamed Install Success`).
* Конфигурация `args.txt` на телефоне проверена (`--android_thread_layout=lanes`, `--android_thermal_log=true`, `--xma_inline_kick=true`).

---

## 36. Стрипованная релизная сборка и A/B выводы по физике (29.10.2026)

### 1. Результаты A/B теста физики на Big-ядрах (c4–c6 / mask 0x70):
* Пользовательский тест на устройстве подтвердил: при разрушении объектов в мире (снос лавочки машиной) перенос `RageJobWork` и `RagePhysics` на Big-ядра привел к большему падению FPS, чем на маленьких ядрах A55.
* **Причина:** На Snapdragon 888 ядра Cortex-A78 уже загружены рендером (`RenderGuest`, `GtaRenderWork`) и микшером аудио (`AudioGuest`). Взрывной всплеск задач физикиEuphoria вытесняет рендер-поток из очереди планировщика. Раскладка **`lanes`** (удержание задач на Cortex-A55) обеспечивает более плавный фреймпейсинг.

### 2. Стрипованный релизный APK:
* Выполнен стейджинг библиотек со стриппингом: `STRIP=1 ./os/android/scripts/build_native.sh 18`:
  * `libmain.so`: 200.5 МБ ➔ **40.5 МБ**
  * `librexgpu-gta4-native.so`: 80.3 МБ ➔ **29.4 МБ**
  * `librexruntime.so`: 96.2 МБ ➔ **8.7 МБ**
  * Суммарный размер `jniLibs`: 433 МБ ➔ **84 МБ**.
* Собрана релизная сборка `./os/android/scripts/build_apk.sh release` (код 0, `app-release.apk` **44.0 МБ**).
* Установлена на устройство `2dc3ec42` через ADB (`Success`), запущен `LauncherActivity`.

---

## 37. Дебаг сборка и деплой на устройство (29.10.2026)

* По запросу пользователя развернуты полные unstripped-библиотеки (с символами для профилирования и трейсов):
  * Выполнен стейджинг: `./os/android/scripts/build_native.sh 18` (полный `libmain.so` ~200 МБ, хуки 725/725 проверены).
  * Выполнена сборка дебаг APK: `./os/android/scripts/build_apk.sh debug` (`app-debug.apk` ~147 МБ).
* Установка и запуск на подключённом устройстве `2dc3ec42` (Snapdragon 888):
  * `adb -s 2dc3ec42 install -r G:/cod/LibertyRecompAndroid/os/android/app/build/outputs/apk/debug/app-debug.apk` (`Success`).
  * `adb -s 2dc3ec42 shell am start -n com.libertyrecomp/.LauncherActivity` (запущен успешно).
* Конфигурация в `/sdcard/Android/data/com.libertyrecomp/files/args.txt` проверена:
  * `--android_thread_layout=lanes`
  * `--android_thermal_log=true`
  * `--xma_inline_kick=true`

---

## 38. Анализ трейса зависания (session_2026-10-30_01-15-32) и устранение дедлока в синхронизации (30.10.2026)

### 1. Контекст захвата и симптомы:
* **Сессия захвата:** `traces/session_2026-10-30_01-15-32/` (`trace_2026-10-30_01-15-32.pftrace` — 42.5 МБ, 27.05 сек; `logcat_freeze.txt` — 2.8 МБ).
* **Аналитическая записка автора:** [`29_ANALYSIS_DEADLOCK_FREEZE_FRAME32900_POST_MORTEM.txt`](file:///G:/cod/LibertyRecompAndroid/Outsider_memory/29_ANALYSIS_DEADLOCK_FREEZE_FRAME32900_POST_MORTEM.txt) (бывший `NEW_PASTA_BOLONEZZZE.txt`).
* **Симптомы:** Снос лавочки на машине / резкое торможение -> полный стоп картинки и оверлея, звук радио продолжал играть, звук тормоза залип.

### 2. Результаты глубокого анализа Perfetto Trace Processor:
* **Последний опубликованный кадр:** Frame 32900 в `10-30 01:20:01.035` (`GtaRecorder` TID 18434, utid 8). Сразу после него `GtaRecorder` ушёл в `Rec: idle (await present job)` за 3.90 сек до конца трейса.
* **Таймлайн остановки потоков:**
  * `t = -9.46 мс`: `RagePhysics` (utids 22, 5, 6133) завершили свои задачи и уснули.
  * `t = -5.32 .. -5.29 мс`: `Main XThread` (utid 6) разбудил рабочие потоки пула задач `RageJobWork` (utid 18, 17, 21), передав им задачи физики/столкновений.
  * `t = -5.24 мс`: `Main XThread` вошёл в ожидание завершения переданных задач (`state=S`) в `PosixConditionBase::WaitLocked` / `WaitMultipleUntil`.
  * `t = -5.19 мс`: `RageJobWork` (utid 18) завершил работу и вызвал `Signal()`.
  * `t = -5.11 мс`: `RageJobWork` (utid 17) завершил работу и вызвал `Signal()`.
  * `t = -5.05 мс`: `RageJobWork` (utid 21) завершил работу и вызвал `Signal()`.
  * **Критическая точка:** Ни один из потоков `RageJobWork` не разбудил `Main XThread`! `Main XThread` остался в `state=S` до самого конца трейса (3.91 секунды).
  * `t = +8.83 мс`: `RenderGuest` (utid 7) завершил отправку накопившихся команд кадра и уснул, ожидая новых команд от застывшего `Main XThread`.
  * Аудио-потоки (`AudioGuest`, `AudioTrack`, `SDLAudioP15`) непрерывно продолжали микшировать звук вплоть до `t = +3895 мс`, что идеально сошлось с показаниями пользователя (радио играло).

### 3. Корневые причины в коде синхронизации:
1. **Пропуск пробуждения в `threading_posix.cpp` (`NotifyExternalWaiters`):**
   * Ветка `if (waiter->sleeping) waiter->cond.notify_all();` пропускала отправку futex-уведомления, если ожидающий поток находился в узком окне проверки условий до установки флага `sleeping = true`.
2. **Потеря гранта при `notify_one()` в `PosixCondition<Event>::SignalLocked()`:**
   * Вызов `NotifyWaitersLocked(manual_reset_ ? kWakeAll : 1)` будил ровно один поток через `notify_one()`. При наличии нескольких ожидающих потоков `notify_one()` будил поток с неподходящим тикетом, тот снова засыпал без потребления гранта, а целевой поток оставался спящим навсегда.
3. **Бесконечные неограниченные ожидания (Deadlock-by-Design):**
   * Вызовы `cond_.wait(lock)` в `WaitLocked` и `waiter.cond.wait(lock, changed)` в `WaitMultipleUntil` не имели таймаута (`infinite`). Любой пропущенный wakeup приводил к вечному зависанию всего процесса.
   * Аналогично в `graphics_system.cpp`: `render_condition_.wait(lock, can_submit)` при backpressure блокировал поток рендеринга без таймаута.

### 4. Внедрённые исправления:
1. **`glue/rexglue-sdk-main/src/core/threading_posix.cpp`:**
   * В `NotifyExternalWaiters` снята проверка `if (waiter->sleeping)`: вызов `waiter->cond.notify_all()` теперь выполняется гарантированно всегда.
   * В `WaitMultipleUntil`: неограниченное ожидание заменено на самовосстанавливающийся `waiter.cond.wait_for(lock, std::chrono::milliseconds(20), changed)`. Если уведомление потерялось, поток просыпается каждые 20 мс и повторно проверяет все хэндлы через `ReadyLocked`.
   * В `WaitLocked`: бесконечный `cond_.wait(lock)` заменён на `cond_.wait_for(lock, std::chrono::milliseconds(20))`.
   * В `PosixCondition<Event>::SignalLocked()`: при `cond_waiters_ > 1` отправляется `kWakeAll`, гарантируя пробуждение потока с нужным тикетом.
2. **`glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp`:**
   * В `SubmitTitleCommand` и `ExecuteTitleCommand` backpressure-ожидания `render_condition_.wait(lock, can_submit)` переведены на `wait_for(lock, std::chrono::milliseconds(20), can_submit)`.
   * В `ExecuteTitleCommand` синхронное ожидание `synchronous->condition.wait` ограничено таймаутом `1000ms`.
   * В `WaitForFrameRecorderIdle` ожидание освобождения рекордера ограничено `wait_for(..., 50ms)`.

### 5. Сборка, деплой и подтверждение:
* Выполнена нативная сборка: `./os/android/scripts/build_native.sh 18` (код 0, все библиотеки `librexruntime.so`, `librexgpu-gta4-native.so`, `libmain.so` пересобраны и застейджены).
* Собрана дебаг сборка: `./os/android/scripts/build_apk.sh debug` (код 0, `app-debug.apk`).
* Выполнен деплой на подключённое устройство `2dc3ec42` через ADB (`Streamed Install Success`).
* **Результат тестирования пользователем:** Проблема полностью закрыта, зависание при ударах о лавочки и резком торможении устранено, игра больше не встаёт в мёртвый дедлок.

---

> [!IMPORTANT]
> ### ОБРАЩЕНИЕ К АВТОРУ ПАКЕТОВ (РАЗРАБОТЧИКУ СИНХРОНИЗАЦИИ / OUTSIDER):
> При накатывании следующих пакетов **НЕ ЗАТИРАЙТЕ И НЕ ОТКАТЫВАЙТЕ ЭТИ ИСПРАВЛЕНИЯ** в `threading_posix.cpp` и `graphics_system.cpp`!
> 
> Ниже приведён детальный разбор, почему оптимизации Пакета 35 привели к мертвому зависанию на Android ARM64 и как они теперь защищены:
> 
> 1. **Проблема адресного `notify_one()` в auto-reset событиях:**
>    * В Пакете 35 для auto-reset `Event` и `Semaphore` вызов `notify_all()` был заменён на `NotifyWaitersLocked(1)`.
>    * **Где ломается:** На многопоточных очередях (RAGE Task/Job System) потоки имеют разные тикеты `ticket.seen`. Вызов `notify_one()` будит произвольный поток из очереди ядра bionic futex. Если разбуженный поток имеет более новый тикет и не может потребить имеющийся грант, он уходит обратно в `cond_.wait()`. При этом поток с подходящим тикетом остаётся спать навсегда.
>    * **Правило:** При `cond_waiters_ > 1` всегда используйте `kWakeAll` (или `notify_all()`). Это даёт шанс всем зарегистрированным потокам проверить свои тикеты. Пробуждение без мьютекса (`FireDeferred` после unlock) уже устранило lock contention, поэтому `kWakeAll` не вызывает прежних тормозов.
> 
> 2. **Гонка `if (waiter->sleeping)` в `NotifyExternalWaiters`:**
>    * Ветка `if (waiter->sleeping) waiter->cond.notify_all();` пропускала системный вызов пробуждения, если сигналящий поток захватил `waiter->mutex` в узком зазоре между проверкой условий в `WaitMultipleUntil` и выставлением флага `waiter.sleeping = true`.
>    * Ожидающий поток входил в `waiter.cond.wait()` сразу после того, как воркер закончил `Signal()`, и оставался спать навсегда.
>    * **Правило:** Вызов `waiter->cond.notify_all()` в `NotifyExternalWaiters()` должен вызываться **всегда безусловно**. В POSIX вызов notify на пустой очереди является безопасным no-op.
> 
> 3. **Недопустимость неограниченных `cond.wait(lock)` (Self-Healing Timeouts):**
>    * На Android с гетерогенными ядрами (big.LITTLE) планировщик Linux может задерживать миграцию потоков на десятки миллисекунд. Любая задержка или потерянный сигнал в связке с бесконечным `cond.wait(lock)` превращает временный микрофриз в **необратимый 100% краш/фриз игры**.
>    * Замена всех `cond_.wait(lock)` на `cond_.wait_for(lock, std::chrono::milliseconds(20))` гарантирует самоисцеление: даже если сигнал по какой-то причине не дошёл, поток проснётся через 20 мс, перечитает состояние в цикле `while(true)` и продолжит кадр. В штатном режиме wake происходит за <10 мкс, так что оверхед равен 0.

---

## 39. Стрипованная релизная сборка APK (30.10.2026)

* По запросу пользователя собрана компактная релизная сборка со стриппингом всех нативных библиотек:
  * `STRIP=1 ./os/android/scripts/build_native.sh 18` (код 0).
  * Библиотеки застрипованы (`llvm-strip --strip-unneeded`):
    * `libmain.so`: 200.5 МБ ➔ **40.5 МБ**
    * `librexgpu-gta4-native.so`: 80.3 МБ ➔ **29.4 МБ**
    * `librexruntime.so`: 96.2 МБ ➔ **8.7 МБ**
    * Суммарный объём `jniLibs`: 433 МБ ➔ **84.6 МБ**.
  * Собрана релизная сборка `./os/android/scripts/build_apk.sh release` (код 0).
  * Готовый релизный файл: [os/android/app/build/outputs/apk/release/app-release.apk](file:///G:/cod/LibertyRecompAndroid/os/android/app/build/outputs/apk/release/app-release.apk) (**44.1 МБ**).
  * По указанию пользователя APK на телефон не заливался.







---

## 40. Интеграция оптимизаций из Outsider_memory/30 и деплой на устройство (30.10.2026)

* **Основание:** Анализ лога `debug-20261030-022316.log` и комплекс патчей из [`30_PATCHES_FAST_PINNING_SELECTIVE_PREFETCH_AND_POOLS.txt`](file:///G:/cod/LibertyRecompAndroid/Outsider_memory/30_PATCHES_FAST_PINNING_SELECTIVE_PREFETCH_AND_POOLS.txt).
* **Состав реализованных изменений:**
  1. **Быстрый контроль привязки ядер ([android_thread_pinning.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp)):**
     - Интервал проверки уменьшен с 400 мс до 100 мс.
     - Добавлен быстрый цикл проверки sticky-потоков через прямые системные вызовы `sched_getaffinity` / `sched_setaffinity` без чтения каталога `/proc/self/task` (76 файлов) каждые 100 мс. Полный обход `/proc` выполняется раз в 2 секунды.
     - Устранена потеря производительности (до 15–20 кадров) при попытках системного планировщика OnePlus (`cpuset=/top-app` / `SchedAssist`) сместить `GtaRenderWork` или `GtaRecorder` на чужие ядра.
  2. **Селективный префетч команд ([native_owned_commands.h](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h)):**
     - Устранён слепой цикл на 51 кэш-линию (3 218 байт с `rw=1`) в `NativeCommandPool::FixedResource::do_allocate`.
     - Префетчатся только первые 256 байт (шапка) и последние 384 байта (хвост с дельтами), которые реально перезаписываются в `ValidateAndCopyCommand`. Исключено переполнение очередей MSHR ядра Cortex-X1 мусорными запросами эксклюзивного владения на 36 неиспользуемых средних кэш-линий.
  3. **$O(1)$ доступ к пулу констант и лимит 1024 ([stateful_constant_state.h](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h)):**
     - Заменён линейный перебор вектора в `local_free_list` на структуру `FastLocalLists` с прямыми полями `vs_4096` и `ps_3584` для двух ключевых размеров банков констант игры.
     - Локальный лимит свободного списка увеличен с 256 до 1024 векторов (~4 МБ памяти), устраняя конкуренцию за глобальный `std::mutex` при массовом сбросе снапшотов констант в конце кадра.
  4. **Рендерер и захват текстур ([graphics_system.cpp](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp)):**
     - Закеширован CVar `use_keepalive = REXCVAR_GET(gta4_native_capture_keepalive);` в `CaptureBufferResource` и `CaptureTextureResource`.
     - При первой вставке текстуры в слот `TextureCaptureCacheSlot` сразу вызывается `KeepCaptured(std::move(resource), slot.alias, slot.alias_serial)`, что переводит все повторные обращения к текстуре в том же кадре в быстрый возврат алиаса без атомарного вызова `slot.resource.lock()`.
     - Точечный префетч в `RenderWorkerMain` (8 ключевых полей структуры команды вместо 51 линии).
     - Безопасный пропуск захвата и материализации пиксельных констант для пассов теней и depth-prepass (`!has_pixel_shader && !dirty_delta.pixel_constant_ranges.Any()`), исключающий потерю dirty-диапазонов при их наличии.
* **Сборка и деплой:**
  - `build_native.sh 18`: УСПЕШНО (код 0, 725 хуков верифицировано).
  - `build_apk.sh debug`: УСПЕШНО (код 0, `app-debug.apk` 241 МБ).
  - Установлено на OnePlus Snapdragon 888 (`2dc3ec42`) через ADB (`Streamed Install Success`).
  - Лаунчер успешно запущен (PID 14782).


---

## 41. Устранение артефактов пасты 30: моргание текстур и мельтешение надписи (30.10.2026)

* **Симптомы:**
  1. *Мельтешение текста в правом нижнем углу:* перед переходом от экрана загрузки к геймплею надпись в правом нижнем углу мельтешит/дрожит менее секунды.
  2. *Моргание текстур:* в игре текстуры на некоторое время мигают/исчезают и затем приходят в норму.
* **Анализ первопричин:**
  1. *Потеря инкрементальных дельт констант (мельтешение надписи):*
     - Автор пасты 30 предложил отсекать обработку пиксельных констант при `pixel_shader == 0`.
     - Архитектура передачи регистров Direct3D/Xenos является инкрементальной (дельта-модель): состояние `state.pixel_constants` на воркере синхронизируется только по изменениям (`dirty_delta`).
     - При смене экрана загрузки на мир между UI-вызовами вклиниваются теневые и глубинные проходы (`pixel_shader == 0`). Отбрасывание дельты приводило к тому, что обновлённые регистры не доходили до воркера.
     - Шрифты в движке рендерятся шейдером по векторным атласам (`VectorFontAtlas`). При последующем рендере текста UI использовал рассинхронизированные/старые регистры в течение нескольких кадров, пока игра не обновляла банк целиком.
     - **Фикс:** В [`graphics_system.cpp`](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp) в `ValidateAndCopyCommand` полностью удалён `skip_pixel_constants` и неиспользуемый `has_pixel_shader`, восстановлена строгая передача `CaptureConstantPayloadDelta`.
  2. *Уничтожение владения и устаревшие алиасы при стриминге (моргание текстур):*
     - Автор пасты 30 ошибочно добавил вызов `KeepCaptured(std::move(resource), slot.alias, slot.alias_serial)` на ветку первичной вставки текстуры в кэш (`if (cacheable && resource)`).
     - В архитектуре пакета 32 `KeepCaptured` предназначен строго для повторных обращений к уже закэшированному ресурсу, а новый ресурс возвращается как полновесный `std::shared_ptr`.
     - При стриминге текстур (`ResourceUnlock` / `ForgetTextureCapture`) перемещение ресурса в `KeepCaptured` приводило к тому, что при совпадении `keepalive_serial` ресурс не добавлялся в массив `refs` фрейма, а сам `resource` уничтожался через `std::move`. В слоте кэша оставался устаревший алиас, и текстура 1–2 кадра ссылалась на невалидную память/старый ресурс.
     - **Фикс:** В `CaptureTextureResource` убран `KeepCaptured` при создании ресурса (возвращается чистый `resource`, `slot.alias = nullptr; slot.alias_serial = 0;`). В [`graphics_system.h`](file:///G:/cod/LibertyRecompAndroid/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h) в `ForgetCaptureCache` добавлен сброс `alias = nullptr` и `alias_serial = 0`.
  3. *Инвалидация дискового кэша Turnip:*
     - При установке нового APK драйвер Mesa Turnip частично пересоздаёт кэш Vulkan-пайплайнов из-за обновления `BuildId` библиотек. Это нормальное поведение в первые 1–2 минуты появления новых объектов в мире.
* **Сборка и верификация:**
  - `build_native.sh 18`: УСПЕШНО (код 0).
  - `build_apk.sh debug`: УСПЕШНО (код 0, `app-debug.apk`).
  - Установлено на Snapdragon 888 (`2dc3ec42`) через `adb install -r`.
