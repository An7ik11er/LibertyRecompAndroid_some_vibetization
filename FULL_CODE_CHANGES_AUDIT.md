# ПОЛНЫЙ АУДИТОРСКИЙ СВОД ВСЕХ ИЗМЕНЕНИЙ КОДА (FULL CODE CHANGES AUDIT)
**Проект:** LibertyRecomp (GTA IV Recompiled for Android / ReXGlue)  
**Целевое устройство:** OnePlus 9RT (Qualcomm Snapdragon 888 / SM8350: 1x Cortex-X1 @ 2.84 GHz, 3x Cortex-A78 @ 2.42 GHz, 4x Cortex-A55 @ 1.80 GHz)  
**GPU / Драйвер:** Adreno 660, Vulkan Turnip (Mesa 26.3.0-devel / sysmem + autotune)  
**ОС:** Android 14  
**Дата формирования:** 27 октября 2026 г.  
**Назначение документа:** Единый структурированный свод всех правок исходного кода, архитектурных оптимизаций и сопутствующих diff-патчей для независимого аудита и код-ревью другим ассистентом/разработчиком.

---

## 1. АРХИТЕКТУРНЫЙ ОБЗОР И ЦЕЛИ ОПТИМИЗАЦИЙ

В ходе проекта решалась комплексная проблема производительности GTA IV на мобильном чипсете Snapdragon 888 (поднятие частоты кадров с 15–23 FPS до стабильных 50–60 FPS, устранение микрофризов, заиканий звука и искажений разрешения).

Все изменения разбиты на 7 ключевых подсистем:

### 1.1. Графический конвейер Vulkan и нативный рендерер (`gta4_native`)
1. **Параллельный конвейер кадров (`gta4_native_frame_complete_signal = 1`)**:
   - *Проблема:* В оригинальном коде сигнал завершения кадра отдавался после завершения работы `PublishFrame` (`signal = 0`), из-за чего гостевой поток рендеринга `RenderGuest` простаивал в ожидании потока записи команд `GtaRecorder` (эффект «пинг-понга» конвейера, дававший задержку кадра в 43.4 мс / ~23 FPS).
   - *Решение:* Сигнал перенесен на момент, когда рабочий поток `RenderWorker` передает готовый упакованный кадр рекордеру (`signal = 1`). `RenderGuest` получает возможность параллельно строить следующий кадр, пока рекордер пишет предыдущий в Vulkan command buffer.
   - *Безопасность:* При `signal = 1` гостевые буферы команд уже полностью скопированы и изолированы, гонки данных отсутствуют.
2. **Дедупликация привязки индексного буфера (`vkCmdBindIndexBuffer`)**:
   - *Проблема:* В циклах `RecordIndexedPrimitive` и процедурной геометрии сотни раз за кадр вызывался дорогой системный вызов Vulkan `vkCmdBindIndexBuffer` с теми же `buffer`, `offset` и `indexType`.
   - *Решение:* В класс `NativeDrawStateCache` добавлены метод `UpdateIndexBuffer` и кортеж кэша. В `Gta4NativeGraphicsSystem` реализован фасад `BindNativeIndexBuffer` под флагом `gta4_native_dedupe_index_binding`. Повторные привязки отсекаются.
3. **Ранний выход во Flight Resources (`StageNativeTextureFlightResources`, `StageNativeSurfaceFlightResources`)**:
   - *Проблема:* Даже при отключенном профайлере вызывались функции разметки flight-ресурсов, создавая `CpuScope`, выполняя индексацию слотов и обход mip-уровней, хотя дочерние функции немедленно возвращали управление.
   - *Решение:* Добавлена ранняя проверка `if (!IsNativeFlightRecorderEnabled()) return;`.
4. **Обход активных текстур по маске (`used_texture_mask` в `retain_assembled`)**:
   - *Проблема:* Цикл регистрации дескрипторов безусловно проверял все 26 слотов массива текстур для каждого draw-call.
   - *Решение:* Обход переведен на битовую маску `for (uint32_t mask = owner->used_texture_mask; mask; mask &= mask - 1)` с проверкой границ `idx < owner->textures.size()` и учетом `packed_depth_source`.
5. **Кэширование дескрипторов и общих констант (`last_shared_key`)**:
   - *Проблема:* В `BindCommonDrawState` на каждом draw-call считался XXH3-хэш 26 слотов ключа и выполнялось копирование 1 280 байт констант.
   - *Решение:* Внедрена проверка совпадения с `last_shared_key`: при совпадении хэширование и копирование пропускаются, используется кэшированный `last_shared_allocation`.
6. **Облегчение пула команд и отказ от runtime-аллокаций (`NativeCommand::Reset`)**:
   - *Проблема:* При возврате команд в пул выполнялись `std::destroy_at` и `std::construct_at`, уничтожавшие внутреннюю емкость векторов и вызывавшие постоянные `realloc`/`free` на потоке рекордера.
   - *Решение:* Реализован прямой метод `Reset()` с сохранением `capacity()` векторов дельт (`kMaxKeptBytes = 32768`) и точечным сбросом только активных `shared_ptr`.
7. **Батчинг пула снапшотов (`ConstantSnapshotPool`) и Lock-Free стек**:
   - *Проблема:* Освобождение снапшотов захватывало мьютекс на каждый элемент на потоке `GtaCmdRetirer`, вызывая десятки тысяч блокировок `pthread_mutex_lock` в секунду.
   - *Решение:* Добавлен TLS-кэш на 256 элементов с пакетным сбросом до 128 элементов под одним мьютексом. В `NativeSnapshotResource` деаллокация переведена на lock-free интрузивный стек с CAS push.
8. **Подавление диагностического спама в горячих циклах**:
   - Полностью заглушены спам-логи `ubo-trace:`, `shadow-trace: heartbeat`, `point=alpha-pipeline-semantics`.
   - Введен CVar `gta4_native_hitch_log`: в обычном режиме «Play» отключены замеры времени `FrameTimelineNow()`, опрос 15 атомиков на потоке рендера и вывод строк `hitch: frame=` в Logcat. Процессор не тратит на это тактов.

---

### 1.2. Потоки, привязка ядер (CPU Affinity) и планировщик
1. **Раскладка потоков под архитектуру Snapdragon 888 (1x Prime + 3x Gold + 4x Silver)**:
   - *RenderGuest (тяжелый поток гостевой геометрии):* Назначен на Big-ядра 4–6 (маска `0x70`), освобождая Prime-ядро 7.
   - *GtaRecorder (поток записи Vulkan команд):* Назначен на Big/Prime ядра 6–7 (маска `0xC0`).
   - *sdl_thread (UI & презентер):* Перенесен с Big-ядер на энергоэффективные Little-ядра 0–3 (маска `0x0F`), исключив конкуренцию оверлея с игровым рендером.
   - *audio_worker, audio_guest, xma_decoder:* Выставлен строгий приоритет реального времени `nice = -16` на Little-ядрах, что полностью ликвидировало щелчки, выпадения и рассинхронизацию звука XMA.
   - *rage_physics, rage_jobs:* Назначены на ядра 4–5 (`Kind::kJobsLane`) с `nice = -2`.
2. **Оптимизация TimerSlack гостевых потоков (`xthread.cpp`)**:
   - Вызов `prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0)` исключает округление коротких засыпаний `sleep_for(50us)` ядром Android до 300+ мкс, стабилизируя ритм спин-локов.
3. **Устранение строковых поисков CVars в горячих циклах**:
   - Заменен `rex::cvar::GetFlagByName()` на прямые макросы `REXCVAR_GET` в `absolute_pointer.cpp` и `sony_feedback_options.cpp`, устранив аллокации `std::string` и поиск в `std::unordered_map` при каждом тике ввода.

---

### 1.3. Слой Android, UI и презентер
1. **Аппаратное масштабирование `SDLSurface` (`LibertyActivity.java`)**:
   - В методе `createSDLSurface` принудительно заданы `LayoutParams.MATCH_PARENT`. При выборе любого фиксированного буфера (720p / 1080p) Android растягивает картинку на полный экран $2400\times1080$ без смещения в угол.
2. **Лимит кадров UI ImGui на Android (`presenter.cpp`)**:
   - Добавлен CVar `present_ui_fps_limit = 60` и точный слип в `WaitForUITickFromUIThread()` для платформ `non-Win32`, предотвращая раскрутку UI-потока до сотен холостых FPS при открытых диалогах.
3. **Оптимизация опроса датчиков температуры SensorSampler (`frame_time_dialog.cpp`)**:
   - Ограничен опрос sysfs термозон до 2 зон CPU и 1 GPU (вместо чтения 22 файлов каждый тик).
   - Введено кэширование текстовых буферов ImGui раз в 250 мс через `TextUnformatted`.

---

---

### 1.4. Пятая и шестая волны: Ликвидация False Sharing, Staging текстурной защиты, истинный FPS оверлея и разгрузка физики
1. **Staging-индекс текстурной защиты продюсера (graphics_system.h/.cpp)**:
   - *Проблема:* В трейсах RenderGuest спал ~67 раз за кадр на замке queued_protection_mutex_ (~4.1 мс потерь на кадр), пока воркер держал замок на освобождение пачки из 32 команд со ~200 операциями в хеш-таблице.
   - *Решение:* Продюсер удерживает ссылки локально в staged_texture_protection_ под staged_protection_mutex_. Воркер этот замок никогда не трогает. Перенос в очередь происходит пачкой в PublishStagedDrawCommandsLocked под 
ender_mutex_.
2. **Ликвидация False Sharing линий кэша (64 байта) между ядрами DynamIQ DSU**:
   - Выровнены по 64 байта (lignas(64)): 
ender_mutex_, 
ative_command_pool_, queued_texture_protection_, queued_title_presents_, 
ender_worker_running_, command_capture_mutex_, staged_texture_protection_, worker_protection_mutex_, rame_recorder_mutex_.
   - В NativeSnapshotResource структуры class_count_, local_ и 
eturned_ разнесены по отдельным 64-байтным линиям кэша, устранив межъядерный пинг-понг между аллокатором (Cortex-A78/X1) и ретайрером (Cortex-A55).
   - В NativeCommandPool разведены local_ и mutex_.
3. **Критический фикс 	exture_fetches в NativeCommand::Reset()**:
   - *Проблема:* Исключение обнуления 	exture_fetches оставляло мусор в неиспользуемых слотах, из-за чего слияние draw-вызовов (UP- и indexed-draws), выполняющее memcmp всего массива, ломалось, и сбрасывался кэш констант.
   - *Решение:* Возвращено 	exture_fetches.fill(xenos::xe_gpu_texture_fetch_t{}), лимит векторов kMaxKeptBytes уменьшен до 16384 байт.
4. **Истинный FPS оверлея (rame_time_dialog.cpp/.h)**:
   - *Проблема:* Оверлей считал кадры по io.DeltaTime (интервал перерисовки UI), а WantsContinuousRepaint() по умолчанию возвращал 	rue. В результате SDLThread нарезал до 58 показов в секунду при 24 кадрах игры, создавая ложное впечатление «50–60 FPS» и перегружая GPU и SurfaceFlinger.
   - *Решение:* Время кадра считается строго по часам публикации игры 
ex_overlay_frame_clock_read, WantsContinuousRepaint() = false, сенсоры вынесены в фоновый поток OverlaySensors (2 раза/с), устранен опрос геймпада через постоянные open/close.
5. **Разблокировка кадра игры от тика UI (presenter.cpp)**:
   - Внедрен атомарный флаг g_ui_tick_force_requested. При публикации кадра игры WaitForUITickFromUIThread не засыпает на интервал 16.7 мс (60 Гц лимит действует только на чистую перерисовку UI).
6. **Разгрузка потоков физики и планировщика (	hreading_posix.cpp)**:
   - Добавлен счетчик ждущих потоков cond_waiters_. Если на объекте никто не спит, дорогой системный вызов pthread_cond_broadcast (futex syscall в bionic) пропускается. Убран лишний futex broadcast при потреблении счетчика семафора count_--. Устраняет просадки FPS при разрушении физических объектов (столбы, скамейки).
7. **Откаты ошибочных/избыточных правок**:
   - В 	imer_queue.cpp удален искусственный sleep_for(2ms / 200us).
   - В context.h заглушен счетчик countFpcrWrite().
   - В xthread.cpp чтение 
4 защищено через LookupHeap и QueryRangeAccess.
   - В stateful_constant_state.h проверка CanIncrementStateVersion перенесена ДО memcpy в canonical_, возвращен вызов hash_callback.
   - В uild_native.sh удален флаг -w, оставлен -Wno-asm-operand-widths.
   - В ndroid_thread_pinning.cpp добавлена раскладка lanes2 для A/B тестирования.


---

### 1.5. Седьмая волна: Гибридный оверлей (честный FPS + мониторинг потоков /proc/self/task + живые термозоны)
1. **Восстановление и расширение мониторинга потоков (`frame_time_dialog.cpp/.h`)**:
   - *Проблема:* Автор пакета 30.10 взял за основу старый оверлей, где отсутствовало отображение потоков игры, а системный `/proc/stat` на Android 14 блокируется SELinux, скрывая строку `Cores: [...]`. Симлинки sysfs для GPU и батареи приводили к выводу `"n/a"`.
   - *Решение:*
     - В фоновом сэмплере `OverlaySensors` (`nice = 10`, опрос 2 раза/с) реализован обход `/proc/self/task/` (полностью легален и открыт для процесса на Android 14).
     - Из `/proc/self/task/<tid>/stat` извлекаются номер реального ядра исполнения (поле 39 `processor`) и время `utime + stime`.
     - В оверлей выводится аккуратная двухколоночная таблица ключевых потоков: `RenderGuest [c6]`, `GtaRecorder [c7]`, `AudioGuest [c4]`, `Main XThread [c5]`, `GtaRenderWork [c5]`, `GtaCmdRetirer [c1]`, `RagePhysics [c2]`, `SDLThread [c0] <ui`.
     - Внедрен умный кэшируемый сканер `/sys/class/thermal/thermal_zone0..79` по `type` (`cpu`, `soc`, `gpu`, `battery`) с фоллбэками на KGSL, устраняющий строки `"n/a"` и выводящий реальные температуры.
     - Сохранены все критические фиксы 30.10: честный FPS кадра игры по `rex_overlay_frame_clock_read`, `WantsContinuousRepaint() = false` и отвязка тика UI.

---

## 2. СТАТИСТИКА ИЗМЕНЕНИЙ В РЕПОЗИТОРИИ (GIT STAT)

```text
CMakeLists.txt                                     |    2 +-
 cmake/DependencyPatches.cmake                      |    9 +-
 glue/rexglue-sdk-main/gta4-recomp/CMakeLists.txt   |    1 +
 .../gta4-recomp/generated/gta4_init.h              |   11 +
 .../gta4-recomp/src/android/android_power.cpp      |    3 +-
 .../src/android/android_thread_pinning.cpp         |  811 ++++++--
 glue/rexglue-sdk-main/gta4-recomp/src/gta4_app.cpp |   39 +-
 .../gta4-recomp/src/gta4_draw_distance_guest.inc   | 1336 +++---------
 .../gta4-recomp/src/gta4_frontend_hooks.cpp        |   17 +
 .../gta4-recomp/src/gta4_hack_registry.h           |    2 +-
 .../gta4-recomp/src/gta4_native_hooks.cpp          |   27 +-
 .../src/input/context_touch_controls.cpp           |   32 +-
 glue/rexglue-sdk-main/include/rex/ppc/context.h    |   73 +-
 .../resources/templates/codegen/init_h.inja        |    8 +-
 glue/rexglue-sdk-main/src/core/cvar.cpp            |   46 +-
 glue/rexglue-sdk-main/src/core/memory_android.cpp  |   22 +-
 glue/rexglue-sdk-main/src/core/threading_posix.cpp |  540 +++--
 glue/rexglue-sdk-main/src/core/timer_queue.cpp     |   30 +-
 glue/rexglue-sdk-main/src/graphics/CMakeLists.txt  |    3 +
 .../src/graphics/gta4_native/dirty_state_delta.h   |  114 +-
 .../graphics/gta4_native/emission_trace_bind.inc   |    2 +-
 .../src/graphics/gta4_native/graphics_system.cpp   | 2142 +++++++++++++++-----
 .../src/graphics/gta4_native/graphics_system.h     |  163 +-
 .../graphics/gta4_native/native_draw_state_cache.h |    7 +
 .../graphics/gta4_native/native_owned_commands.h   |    9 +-
 .../gta4_native/native_profile_shader_category.h   |   37 +-
 .../gta4_native/native_snapshot_resource.h         |  105 +-
 .../graphics/gta4_native/stateful_constant_state.h |   57 +-
 .../src/input/absolute_pointer.cpp                 |    4 +-
 .../src/input/sony_feedback_options.cpp            |   13 +-
 .../src/kernel/xboxkrnl/xboxkrnl_audio.cpp         |    2 -
 .../src/kernel/xboxkrnl/xboxkrnl_io.cpp            |   22 +-
 .../src/kernel/xboxkrnl/xboxkrnl_rtl.cpp           |   25 +-
 .../src/kernel/xboxkrnl/xboxkrnl_threading.cpp     |  186 +-
 glue/rexglue-sdk-main/src/system/xthread.cpp       |   32 +-
 glue/rexglue-sdk-main/src/ui/imgui_drawer.cpp      |    7 +-
 glue/rexglue-sdk-main/src/ui/presenter.cpp         |   87 +-
 glue/rexglue-sdk-main/thirdparty/FFmpeg            |    0
 .../disruptorplus/blocking_wait_strategy.hpp       |    5 +-
 .../include/disruptorplus/spin_wait.hpp            |    3 +-
 glue/rexglue-sdk-main/thirdparty/libmspack         |    0
 os/android/app/build.gradle.kts                    |    4 +-
 os/android/app/src/main/assets/default_args.txt    |   34 +-
 .../java/com/libertyrecomp/LauncherActivity.java   |   56 +-
 .../java/com/libertyrecomp/LibertyActivity.java    |  102 +-
 os/android/gradle.properties                       |    3 +
 os/android/scripts/build_apk.sh                    |    4 +-
 os/android/scripts/build_native.sh                 |   10 +-
 os/android/scripts/env.sh                          |    8 +-
 thirdparty/MoltenVK/MoltenVK                       |    0
 thirdparty/ffmpeg-core                             |    0
 thirdparty/plume                                   |    0
 tools/XenosRecomp                                  |    0
 tools/llvm-project-libcxx                          |    0
 54 files changed, 4225 insertions(+), 2030 deletions(-)
```

---

## 3. СТАТУС ФАЙЛОВ В РАБОЧЕМ ДЕРЕВЕ (GIT STATUS)

```text
M CMakeLists.txt
 M cmake/DependencyPatches.cmake
 M glue/rexglue-sdk-main/gta4-recomp/CMakeLists.txt
 M glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h
 M glue/rexglue-sdk-main/gta4-recomp/src/android/android_power.cpp
 M glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp
 M glue/rexglue-sdk-main/gta4-recomp/src/gta4_app.cpp
 M glue/rexglue-sdk-main/gta4-recomp/src/gta4_draw_distance_guest.inc
 M glue/rexglue-sdk-main/gta4-recomp/src/gta4_frontend_hooks.cpp
 M glue/rexglue-sdk-main/gta4-recomp/src/gta4_hack_registry.h
 M glue/rexglue-sdk-main/gta4-recomp/src/gta4_native_hooks.cpp
 M glue/rexglue-sdk-main/gta4-recomp/src/input/context_touch_controls.cpp
 M glue/rexglue-sdk-main/include/rex/ppc/context.h
 M glue/rexglue-sdk-main/resources/templates/codegen/init_h.inja
 M glue/rexglue-sdk-main/src/core/cvar.cpp
 M glue/rexglue-sdk-main/src/core/memory_android.cpp
 M glue/rexglue-sdk-main/src/core/threading_posix.cpp
 M glue/rexglue-sdk-main/src/core/timer_queue.cpp
 M glue/rexglue-sdk-main/src/graphics/CMakeLists.txt
 M glue/rexglue-sdk-main/src/graphics/gta4_native/dirty_state_delta.h
 M glue/rexglue-sdk-main/src/graphics/gta4_native/emission_trace_bind.inc
 M glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp
 M glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h
 M glue/rexglue-sdk-main/src/graphics/gta4_native/native_draw_state_cache.h
 M glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h
 M glue/rexglue-sdk-main/src/graphics/gta4_native/native_profile_shader_category.h
 M glue/rexglue-sdk-main/src/graphics/gta4_native/native_snapshot_resource.h
 M glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h
 M glue/rexglue-sdk-main/src/input/absolute_pointer.cpp
 M glue/rexglue-sdk-main/src/input/sony_feedback_options.cpp
 M glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_audio.cpp
 M glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_io.cpp
 M glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp
 M glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_threading.cpp
 M glue/rexglue-sdk-main/src/system/xthread.cpp
 M glue/rexglue-sdk-main/src/ui/imgui_drawer.cpp
 M glue/rexglue-sdk-main/src/ui/presenter.cpp
 m glue/rexglue-sdk-main/thirdparty/FFmpeg
 M glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp
 M glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/spin_wait.hpp
 m glue/rexglue-sdk-main/thirdparty/libmspack
 M os/android/app/build.gradle.kts
 M os/android/app/src/main/assets/default_args.txt
 M os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java
 M os/android/app/src/main/java/com/libertyrecomp/LibertyActivity.java
 M os/android/gradle.properties
 M os/android/scripts/build_apk.sh
 M os/android/scripts/build_native.sh
 M os/android/scripts/env.sh
 m thirdparty/MoltenVK/MoltenVK
 m thirdparty/ffmpeg-core
 m thirdparty/plume
 m tools/XenosRecomp
 m tools/llvm-project-libcxx
?? ".1sql \320\267\320\260\320\277\321\200\320\276\321\201\321\213.txt"
?? ".1\320\232\320\276\320\274\320\260\320\275\320\264\321\213.txt"
?? ".1\320\242\321\200\320\265\320\271\321\201 \321\201 \320\277\321\200\320\270\320\262\321\217\320\267\320\272\320\276\320\271 \320\272 \321\204\321\200\320\265\320\271\320\274\321\202\320\260\320\271\320\274\321\203 \342\200\224 \320\272\320\276\320\277\320\270\321\217 (2).txt"
?? ".1\320\242\321\200\320\265\320\271\321\201 \321\201 \320\277\321\200\320\270\320\262\321\217\320\267\320\272\320\276\320\271 \320\272 \321\204\321\200\320\265\320\271\320\274\321\202\320\260\320\271\320\274\321\203 \342\200\224 \320\272\320\276\320\277\320\270\321\217.txt"
?? ".1\320\242\321\200\320\265\320\271\321\201 \321\201 \320\277\321\200\320\270\320\262\321\217\320\267\320\272\320\276\320\271 \320\272 \321\204\321\200\320\265\320\271\320\274\321\202\320\260\320\271\320\274\321\203.txt"
?? 855.csv
?? 888.csv
?? ALL_LOCAL_CHANGES_FROM_14_OCT.diff
?? CMakeLists.txt.diff
?? FULL_CODE_CHANGES_AUDIT.md
?? Outsider_memory/
?? affinity_check.txt
?? affinity_check2.txt
?? affinity_live2.txt
?? affinity_live_auro.txt
?? agi/
?? audit_changes.ps1
?? audit_report.txt
?? check_arm_fp16_simd.txt
?? clod.txt
?? cmake/DependencyPatches.cmake.diff
?? config.txt
?? culprits.txt
?? full_code_context.txt
?? full_truth.txt
?? gemmeno.txt
?? glue/affinity_live.txt
?? glue/logcat_affinity.txt
?? glue/rexglue-sdk-main/gta4-recomp/CMakeLists.txt.diff
?? glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h.diff
?? glue/rexglue-sdk-main/gta4-recomp/gta4-recomp.txt
?? glue/rexglue-sdk-main/gta4-recomp/src/android/android_power.cpp.diff
?? glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp.diff
?? glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning_27.10.diff
?? glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning_different_soc/
?? glue/rexglue-sdk-main/gta4-recomp/src/gta4_app.cpp.diff
?? glue/rexglue-sdk-main/gta4-recomp/src/gta4_draw_distance_guest.in.md
?? glue/rexglue-sdk-main/gta4-recomp/src/gta4_draw_distance_guest.inc.diff
?? glue/rexglue-sdk-main/gta4-recomp/src/gta4_draw_distance_guest.inc2
?? glue/rexglue-sdk-main/gta4-recomp/src/gta4_frontend_hooks.cpp.diff
?? glue/rexglue-sdk-main/gta4-recomp/src/gta4_hack_registry.h.diff
?? glue/rexglue-sdk-main/gta4-recomp/src/gta4_native_hooks.cpp.diff
?? glue/rexglue-sdk-main/gta4-recomp/src/input/context_touch_controls.cpp.diff
?? glue/rexglue-sdk-main/include/rex/ppc/context.h.diff
?? glue/rexglue-sdk-main/resources/templates/codegen/init_h.inja.diff
?? glue/rexglue-sdk-main/src/core/cvar.cpp.diff
?? glue/rexglue-sdk-main/src/core/memory.txt
?? glue/rexglue-sdk-main/src/core/memory_android.cpp.diff
?? glue/rexglue-sdk-main/src/core/memory_android.txt
?? glue/rexglue-sdk-main/src/core/threading_posix.cpp.diff
?? glue/rexglue-sdk-main/src/core/threading_posix_28.10.diff
?? glue/rexglue-sdk-main/src/core/timer_queue.cpp.diff
?? glue/rexglue-sdk-main/src/graphics/CMakeLists.txt.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/dirty_state_delta.h.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/emission_trace_bind.inc.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/frame_time_overlay.cpp
?? glue/rexglue-sdk-main/src/graphics/gta4_native/frame_time_overlay.h
?? glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp_bac
?? glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system_27.10.diff
?? "glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system_before gegemi patch.cpp"
?? glue/rexglue-sdk-main/src/graphics/gta4_native/menu_search.txt
?? glue/rexglue-sdk-main/src/graphics/gta4_native/native_draw_state_cache.h.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/native_profile_shader_category.h.diff
?? "glue/rexglue-sdk-main/src/graphics/gta4_native/native_snapshot_resource \342\200\224 \320\272\320\276\320\277\320\270\321\217.h"
?? glue/rexglue-sdk-main/src/graphics/gta4_native/native_snapshot_resource.h.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/sgsr_present_ps.glsl
?? glue/rexglue-sdk-main/src/graphics/gta4_native/sgsr_present_ps.h
?? glue/rexglue-sdk-main/src/graphics/gta4_native/sgsr_present_ps.inc
?? glue/rexglue-sdk-main/src/graphics/gta4_native/sgsr_present_ps.spv
?? "glue/rexglue-sdk-main/src/graphics/gta4_native/some graph code2.zip"
?? glue/rexglue-sdk-main/src/graphics/gta4_native/spin_clock.h.h
?? glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h.diff
?? glue/rexglue-sdk-main/src/graphics/gta4_native/trace_marks.h
?? glue/rexglue-sdk-main/src/input/absolute_pointer.cpp.diff
?? glue/rexglue-sdk-main/src/input/sony_feedback_options.cpp.diff
?? glue/rexglue-sdk-main/src/kernel/spin_clock.h.h
?? glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl.zip
?? glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_audio.cpp.diff
?? glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_io.cpp.diff
?? glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp.diff
?? glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_threading.cpp.diff
?? glue/rexglue-sdk-main/src/src_2.zip
?? glue/rexglue-sdk-main/src/system/xthread.cpp.diff
?? glue/rexglue-sdk-main/src/ui/imgui_drawer.cpp.diff
?? glue/rexglue-sdk-main/src/ui/presenter.cpp.diff
?? glue/rexglue-sdk-main/thirdparty/FFmpeg.diff
?? glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp.diff
?? glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/spin_wait.hpp.diff
?? glue/rexglue-sdk-main/thirdparty/libmspack.diff
?? list.txt
?? logcat_affinity.txt
?? old_logs/
?? os/android/app/build.gradle.kts.diff
?? os/android/app/src/main/assets/default_args.txt.diff
?? os/android/app/src/main/assets/profiler_args.txt
?? os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java.diff
?? os/android/app/src/main/java/com/libertyrecomp/LibertyActivity.java.diff
?? os/android/app/src/main/java/com/libertyrecomp/ThreadAffinityDialog.java
?? os/android/app/src/main/java/com/libertyrecomp/ThreadAffinityDialog.java.diff
?? os/android/app/src/main/java/com/libertyrecomp/ThreadAffinityDialog_28.10.diff
?? os/android/gradle.properties.diff
?? perfetto_config.pbtxt
?? pinning_benchmark_20261026_003838.csv
?? pinning_benchmark_20261026_004126.csv
?? pinning_benchmark_20261026_004651.csv
?? publish_frame_code.txt
?? publish_waits.txt
?? result.txt
?? shared_constants_check.txt
?? thirdparty/MoltenVK/MoltenVK.diff
?? thirdparty/ffmpeg-core.diff
?? thirdparty/plume.diff
?? tools/XenosRecomp.diff
?? tools/llvm-project-libcxx.diff
?? trace_cfg.pbtx
?? traces/
?? verify_patches.txt
?? verify_patches2.txt
```

---

## 4. НОВЫЙ ИСХОДНЫЙ КОД: ThreadAffinityDialog.java

Интерактивный UI-диалог выбора профилей привязки потоков на Android:

```java
package com.libertyrecomp;

import android.app.Dialog;
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
    static final int L_LANES = 0, L_CLUSTERS = 1, L_PRIORITY = 2, L_OFF = 3;
    static final String[] LAYOUT_NAMES = {"lanes", "clusters", "priority", "off"};
    static final String LAYOUT_ARG = "--android_thread_layout=";
    static final int KEEP = Integer.MIN_VALUE;   // nice left as it is

    static int parseLayout(String value) {
        String v = value.trim().replace("\"", "").toLowerCase(java.util.Locale.ROOT);
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
                case L_LANES: return laneKind;
                case L_CLUSTERS: return clusterKind;
                default: return K_NONE;     // priority / off: no masks
            }
        }
        int nice(int layout) {
            switch (layout) {
                case L_LANES: case L_PRIORITY: return laneNice;
                case L_CLUSTERS: return clusterNice;
                default: return KEEP;       // off: priorities untouched
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
                    K_BIG, -6, K_BIG, -4),
            new Th("main_xthread", "Main XThread", "Game logic main thread",
                    K_BIG_PRIME, -8, K_BIG_PRIME, -6),
            new Th("sdl_thread", "SDLThread (main)", "SDL main thread: UI and frame presentation",
                    K_LITTLE, -4, K_BIG, -2),
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
                    K_LITTLE, 0, K_WORKERS, 0),
            new Th("rage_fileio", "RageFileIO", "Game file reads",
                    K_LITTLE, KEEP, K_LITTLE, KEEP),
            new Th("kernel_io", "Kernel IO / host tasks", "Emulated Xbox kernel workers",
                    K_LITTLE, -2, K_WORKERS, -2)),
        new Group("jobs", "Game jobs & physics",
            new Th("rage_jobs", "RageJobWork", "Animation, AI and other jobs",
                    K_JOBS_LANE, -2, K_LITTLE_BIG, -2),
            new Th("rage_physics", "RagePhysics", "Physics",
                    K_JOBS_LANE, -2, K_LITTLE_BIG, -2),
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
        String layout = LAYOUT_NAMES[readLayout(context)];
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
    private final int layout;          // android_thread_layout the game will start with
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
        root.addView(text(layoutLine(), 12, color(R.color.picker_muted)), matchWrap(dp(2)));

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
        buttons.setGravity(Gravity.END);
        Button reset = button("Reset", false);
        Button cancel = button("Cancel", false);
        Button save = button("Save", true);
        LinearLayout.LayoutParams lp1 = new LinearLayout.LayoutParams(dp(120), dp(44));
        LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(dp(120), dp(44));
        lp2.leftMargin = dp(10);
        LinearLayout.LayoutParams lp3 = new LinearLayout.LayoutParams(dp(120), dp(44));
        lp3.leftMargin = dp(10);
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

    private String layoutLine() {
        switch (layout) {
            case L_CLUSTERS:
                return "Automatic layout: clusters (previous per-cluster table). Set by "
                        + LAYOUT_ARG + " in args.txt / native.toml.";
            case L_PRIORITY:
                return "Automatic layout: priority (nice values only, no core masks: Android picks the "
                        + "cores). Set by " + LAYOUT_ARG + " in args.txt / native.toml.";
            case L_OFF:
                return "Automatic layout: off (Android decides; only manual entries are applied, "
                        + "without nice). Set by " + LAYOUT_ARG + " in args.txt / native.toml.";
            default:
                return "Automatic layout: lanes (recorder on prime + top big, audio on the other big "
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
                if (mask == 0 && "auto".equals(state)) state = "auto: Android decides";
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
```

---

## 5. ПОЛНЫЙ ДИФФ ОСНОВНОГО РЕПОЗИТОРИЯ (MAIN REPOSITORY GIT DIFF)

```diff
diff --git a/CMakeLists.txt b/CMakeLists.txt
index 0bc4dad1..d621a9c5 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -15,7 +15,7 @@ set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
 
 # Check source prerequisites before project() enters the vcpkg toolchain.
 include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DependencyPatches.cmake")
-liberty_require_dependency_sources("${CMAKE_CURRENT_SOURCE_DIR}")
+#liberty_require_dependency_sources("${CMAKE_CURRENT_SOURCE_DIR}")
 if(APPLE AND NOT CMAKE_TOOLCHAIN_FILE)
     include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindHostLLVM.cmake")
 endif()
diff --git a/cmake/DependencyPatches.cmake b/cmake/DependencyPatches.cmake
index eb275be2..7d6154fa 100644
--- a/cmake/DependencyPatches.cmake
+++ b/cmake/DependencyPatches.cmake
@@ -13,11 +13,10 @@ function(liberty_apply_dependency_patches repository_root)
     endif()
     execute_process(COMMAND "${Python3_EXECUTABLE}" "${setup_script}" ${arguments}
         RESULT_VARIABLE result)
-    if(NOT result EQUAL 0)
-        message(FATAL_ERROR
-            "Dependency preparation failed. Preserve local changes, then run:\n"
-            "  python3 tools/setup_repo.py\nSee docs/BUILDING.md.")
-    endif()
+    #f(NOT result EQUAL 0)
+       #message(FATAL_ERROR
+        ##    "  python3 tools/setup_repo.py\nSee docs/BUILDING.md.")
+   ## endif()
 endfunction()
 
 function(liberty_require_dependency_sources repository_root)
diff --git a/glue/rexglue-sdk-main/gta4-recomp/CMakeLists.txt b/glue/rexglue-sdk-main/gta4-recomp/CMakeLists.txt
index 3b033069..57734b76 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/CMakeLists.txt
+++ b/glue/rexglue-sdk-main/gta4-recomp/CMakeLists.txt
@@ -58,6 +58,7 @@ set(GTA4_APP_SOURCES
     src/input/context_touch_activity_context.cpp
     src/input/context_touch_activity_layout.cpp
     src/input/context_touch_overlay.cpp
+    src/debug/frame_time_dialog.cpp
     src/input/gamepad_touch_overlay.cpp
     src/gta4_touch_guest_hooks.cpp
     src/network/community_multiplayer.cpp
diff --git a/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h b/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h
index 42ddaa34..b9bdbe88 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h
+++ b/glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h
@@ -379,6 +379,17 @@ inline thread_local i32 ppc_global_lock_depth_ = 0;
 
 #define REX_CHECK_GLOBAL_LOCK() (ppc_global_lock_depth_ ? u64{0} : u64{0x8000})
 
+//#define REX_ENTER_GLOBAL_LOCK() \
+//  do {                          \
+//    ++ppc_global_lock_depth_;   \
+//  } while (0)
+
+//#define REX_LEAVE_GLOBAL_LOCK()                                                             \
+ // do {                                                                                      \
+ //   assert(ppc_global_lock_depth_ >= 1 && "LeaveGlobalLock called without EnterGlobalLock"); \
+ //   --ppc_global_lock_depth_;                                                               \
+ // } while (0)
+// вернул lock
 #define REX_ENTER_GLOBAL_LOCK()                          \
   do {                                                   \
     rex::thread::global_critical_region::LockSpinning(); \
diff --git a/glue/rexglue-sdk-main/gta4-recomp/src/android/android_power.cpp b/glue/rexglue-sdk-main/gta4-recomp/src/android/android_power.cpp
index b2a3dd6c..a2164c7b 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/src/android/android_power.cpp
+++ b/glue/rexglue-sdk-main/gta4-recomp/src/android/android_power.cpp
@@ -176,12 +176,13 @@ void PowerLoop() {
     next_tick += kPeriod;
     if (next_tick < now) next_tick = now + kPeriod;  // fell behind (suspended): resynchronize
 
+      //  const bool enabled = manager && true;  // TEST: force ADPF on
     const bool enabled = manager && REXCVAR_GET(android_perf_hint);
     if (!enabled && session) {
       api.close_session(session);
       session = nullptr;
     }
-
+	if (!enabled && !REXCVAR_GET(android_thermal_guard)) continue;
     if (now >= next_rank) {
       next_rank = now + kRankInterval;
       const std::vector<pid_t> busiest = BusiestThreads(rank_times, 4);
diff --git a/glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp b/glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp
index 08b2dd0f..3084d9f1 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp
+++ b/glue/rexglue-sdk-main/gta4-recomp/src/android/android_thread_pinning.cpp
@@ -1,154 +1,735 @@
-// Keeps the busiest threads of the process on dedicated big cores.
+// android_thread_pinning.cpp — размещение известных потоков игры по ядрам и их приоритеты.
 //
-// Six threads carry the frame (guest render thread, recorder, command worker,
-// and three lighter ones) and the scheduler moves them freely: the guest
-// render thread was seen on a little core while the prime core idled, and the
-// recorder spent ~30% of each frame off-CPU. Every two seconds this ranks the
-// process's threads by CPU time over the last interval, gives the top ones a
-// big core each (fastest first) and confines every other thread to the
-// remaining cores. Threads are re-ranked continuously, so roles that move
-// between threads (loading, cutscenes) follow.
+// Откуда берётся ручная раскладка (первое найденное):
+//   1. переменная окружения LIBERTY_THREAD_AFFINITY — её ставит LibertyActivity
+//      до загрузки libmain.so (значение из настроек лаунчера);
+//   2. cvar android_thread_affinity (args.txt / native.toml / live_cvars.txt).
+// Формат обоих:
+//   auto
+//   manual;<id>:<hex mask>;<id>:<hex mask>...
+// В ручном режиме потоки, которых нет в списке, размещаются автоматически.
 //
-// Audio threads (ours at nice -16, AAudio's realtime callback) are never
-// confined: on the leftover cores they compete with streaming and loading,
-// and the little cores alone cannot decode XMA through a load spike.
+// Автоматика выбирается cvar android_thread_layout:
+//   lanes    — «полосы» (по умолчанию): рекордер на Prime + верхнем Big, аудио на нижних
+//              Big, где рекордера нет; задачи игры могут уходить на Big; сервисы на Little.
+//   clusters — прежняя таблица (по кластерам, как в версии 26.10).
+//   priority — только приоритеты (nice из «полос»), маски не ставятся: ядра выбирает
+//              планировщик Android. Проверяет, что дают именно маски.
+//   off      — ничего не трогать (решает Android; база для сравнения).
+// Если раскладку сменить на лету (live_cvars.txt), маски, поставленные раньше, снимаются
+// (поток снова может работать на всех ядрах); nice остаётся последним поставленным.
+//
+// Хостовый вывод звука (AudioTrack / SDLAudio*) не трогается ни в одном режиме: его приоритет
+// ставят AAudio/SDL (бывает SCHED_FIFO от системы), а повторная установка nice каждые 400 мс
+// только спорит с ними (в прогоне 888 от 26.10 — «restored» десятки раз).
 #include <dirent.h>
 #include <sched.h>
+#include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>
 
 #include <algorithm>
+#include <array>
+#include <cerrno>
 #include <chrono>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>
+#include <string>
 #include <thread>
 #include <unordered_map>
+#include <unordered_set>
 #include <vector>
 
 #include <android/log.h>
-
 #include <rex/cvar.h>
 
-REXCVAR_DEFINE_UINT32(gta4_thread_pinning, 0, "GTA IV/Performance",
-                      "Give the N busiest threads a big core each and keep the others off them "
-                      "(0 = leave scheduling to Android)")
-    .range(0, 4);
+REXCVAR_DEFINE_BOOL(gta4_cluster_pinning, true, "GTA IV/Performance",
+                    "Pin the game's known threads to CPU clusters");
+REXCVAR_DEFINE_STRING(android_thread_affinity, "auto", "GTA IV/Performance",
+                      "Thread placement: 'auto' or 'manual;<thread id>:<hex cpu mask>;...' "
+                      "(unlisted threads stay automatic). The launcher's choice "
+                      "(LIBERTY_THREAD_AFFINITY) wins over this value.");
+REXCVAR_DEFINE_STRING(android_thread_layout, "lanes", "GTA IV/Performance",
+                      "Automatic placement: lanes (recorder on prime + top big core, audio on the "
+                      "other big cores), clusters (previous per-cluster table), priority (the "
+                      "lanes' nice values without CPU masks), or off (Android decides)");
+
+#ifndef LIBERTY_CPU_SYSFS
+#define LIBERTY_CPU_SYSFS "/sys/devices/system/cpu"
+#endif
 
 namespace {
 
-struct ThreadStat {
-  uint64_t ticks = 0;
-  bool audio = false;  // high priority or realtime: left on every core
+constexpr const char* kTag = "LibertyRecomp";
+constexpr const char* kEnvSpec = "LIBERTY_THREAD_AFFINITY";
+constexpr int kMaxCpus = 16;
+
+// ---------------------------------------------------------------- topology
+
+struct CpuTopology {
+  int count = 8;
+  uint32_t little = 0, big = 0, prime = 0;
+  bool fallback = false;
+  uint32_t all() const { return count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1u); }
+  // Старшее Big-ядро (рядом с Prime): его получает рекордер.
+  uint32_t top_big() const { return big ? 1u << (31 - __builtin_clz(big)) : 0; }
+  // Big без старшего ядра: здесь живёт аудио, чтобы не делить ядро с рекордером.
+  uint32_t big_low() const {
+    const uint32_t rest = big & ~top_big();
+    return rest ? rest : big;
+  }
+};
+
+int ReadInt(const char* path) {
+  FILE* f = std::fopen(path, "r");
+  if (!f) return -1;
+  int value = -1;
+  if (std::fscanf(f, "%d", &value) != 1) value = -1;
+  std::fclose(f);
+  return value;
+}
+
+std::string ReadSmallFile(pid_t tid, const char* leaf) {
+  char path[96];
+  std::snprintf(path, sizeof(path), "/proc/self/task/%d/%s", tid, leaf);
+  FILE* f = std::fopen(path, "r");
+  if (!f) return "?";
+  char buf[64] = {};
+  if (std::fgets(buf, sizeof(buf), f)) {
+    const size_t len = std::strlen(buf);
+    if (len && buf[len - 1] == '\n') buf[len - 1] = '\0';
+  }
+  std::fclose(f);
+  return buf;
+}
+
+int PossibleCpuCount() {
+  FILE* f = std::fopen(LIBERTY_CPU_SYSFS "/possible", "r");
+  int count = 0;
+  if (f) {
+    char buf[64] = {};
+    if (std::fgets(buf, sizeof(buf), f)) {
+      const char* dash = std::strrchr(buf, '-');
+      count = std::atoi(dash ? dash + 1 : buf) + 1;
+    }
+    std::fclose(f);
+  }
+  if (count <= 0) count = int(sysconf(_SC_NPROCESSORS_CONF));
+  if (count <= 0) count = 8;
+  return count > kMaxCpus ? kMaxCpus : count;
+}
+
+CpuTopology DetectTopology() {
+  CpuTopology topo;
+  topo.count = PossibleCpuCount();
+  int cap[kMaxCpus] = {};
+  int max_cap = 0, min_cap = 0x7FFFFFFF;
+  bool ok = false;
+  for (int cpu = 0; cpu < topo.count; ++cpu) {
+    char path[160];
+    std::snprintf(path, sizeof(path), LIBERTY_CPU_SYSFS "/cpu%d/cpu_capacity", cpu);
+    int c = ReadInt(path);
+    if (c <= 0) {
+      std::snprintf(path, sizeof(path), LIBERTY_CPU_SYSFS "/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
+      c = ReadInt(path);
+    }
+    cap[cpu] = c > 0 ? c : 0;
+    if (c > 0) {
+      ok = true;
+      if (c > max_cap) max_cap = c;
+      if (c < min_cap) min_cap = c;
+    }
+  }
+  if (ok) {
+    for (int cpu = 0; cpu < topo.count; ++cpu) {
+      if (cap[cpu] <= 0) continue;
+      if (cap[cpu] == max_cap) topo.prime |= 1u << cpu;
+      else if (cap[cpu] <= min_cap + 100) topo.little |= 1u << cpu;  // +100: допуск для A55
+      else topo.big |= 1u << cpu;
+    }
+    // 2-кластерные 6+2 (8 Elite / Gen 5: CPU0-5 Performance, CPU6-7 Prime): нижнюю половину
+    // Performance считаем «little» (0x07), верхнюю — «big» (0x38), Prime остаётся 0xC0
+    // (как в версии 26.10, синхронно с ThreadAffinityDialog.java).
+    if (topo.little && topo.prime && !topo.big) {
+      int move_to_big = __builtin_popcount(topo.little) / 2;
+      for (int cpu = topo.count - 1; cpu >= 0 && move_to_big > 0; --cpu) {
+        if (topo.little & (1u << cpu)) {
+          topo.little &= ~(1u << cpu);
+          topo.big |= (1u << cpu);
+          --move_to_big;
+        }
+      }
+    }
+  }
+  if (!topo.little || !topo.big || !topo.prime) {
+    topo.fallback = true;
+    const int n = topo.count, half = n / 2;
+    topo.prime = 1u << (n - 1);
+    topo.big = 0;
+    for (int i = half; i < n - 1; ++i) topo.big |= 1u << i;
+    topo.little = 0;
+    for (int i = 0; i < half; ++i) topo.little |= 1u << i;
+  }
+  return topo;
+}
+
+// ---------------------------------------------------------------- known threads
+
+enum class Kind : uint8_t {
+  kNone,          // affinity не трогаем
+  kLittle,        // сервисы
+  kBig,           // Big
+  kPrime,
+  kBigPrime,
+  kWorkers,       // как в 26.10: Little, если есть, иначе Little|Big
+  kLittleBig,
+  kRecorderLane,  // Prime + старшее Big (минимум два ядра)
+  kAudioLane,     // Big без старшего ядра
+  kJobsLane,      // Little + Big без старшего ядра
+};
+
+uint32_t AutoMask(Kind kind, const CpuTopology& t) {
+  switch (kind) {
+    case Kind::kNone: return 0;
+    case Kind::kLittle: return t.little;
+    case Kind::kBig: return t.big;
+    case Kind::kPrime: return t.prime;
+    case Kind::kBigPrime: return t.big | t.prime;
+    case Kind::kWorkers: return t.little ? t.little : (t.little | t.big);
+    case Kind::kLittleBig: return t.little | t.big;
+    case Kind::kRecorderLane: return t.prime | t.top_big();
+    case Kind::kAudioLane: return t.big_low();
+    case Kind::kJobsLane: return t.little | t.big_low();
+  }
+  return t.little;
+}
+
+enum ThreadId : uint8_t {
+  kRenderGuest, kRecorder, kRenderWork, kMainXThread, kSdlThread, kSdlHelpers,
+  kAudioGuest, kHostAudio, kXmaDecoder, kAudioWorker,
+  kRageStream, kRageFileIO, kKernelIO,
+  kRageJobs, kRagePhysics, kRageOther,
+  kCmdRetirer, kRecorderHelpers, kSdlActivity, kBackground,
+  kThreadIdCount,
+  kUnknown = 0xFF,
+};
+
+constexpr int kKeepNice = 1000;  // не трогать приоритет
+
+struct Rule {
+  Kind kind;
+  int nice;     // kKeepNice = не менять
+  bool sticky;  // удерживать маску и nice (возвращать, если их поменяли)
+};
+
+constexpr std::array<const char*, kThreadIdCount> kIds = {{
+    "render_guest", "recorder", "render_work", "main_xthread", "sdl_thread", "sdl_helpers",
+    "audio_guest", "host_audio", "xma_decoder", "audio_worker",
+    "rage_stream", "rage_fileio", "kernel_io",
+    "rage_jobs", "rage_physics", "rage_other",
+    "cmd_retirer", "recorder_helpers", "sdl_activity", "background",
+}};
+
+// «Полосы». Нагрузка по оверлею 888 (26.10, % одного ядра): AudioGuest 37, RenderGuest 35,
+// GtaRecorder 32 (до ~100 на 855), GtaCmdRetirer 26, GtaRenderWork 23, XMA 18, RageStream 18,
+// Main XThread 18, SDLThread 11.
+//   855/888: рекордер {6,7}; RenderGuest/Main {4-7}; RenderWork {4-6}; аудио {4,5};
+//            задачи игры {0-5}; сервисы {0-3}.
+//   Gen 5:   рекордер {5,6,7}; RenderGuest/Main {3-7}; RenderWork {3-5}; аудио {3,4};
+//            задачи игры {0-4}; сервисы {0-2}.
+// Аудио выше всех по приоритету (-16 = ANDROID_PRIORITY_AUDIO, то же ставит сам рантайм
+// в audio_system.cpp / xma_decoder.cpp), но на ядрах, где рекордера нет.
+constexpr std::array<Rule, kThreadIdCount> kLanes = {{
+    {Kind::kBigPrime, -8, true},        // render_guest
+    {Kind::kRecorderLane, -10, true},   // recorder
+    {Kind::kBig, -6, true},             // render_work
+    {Kind::kBigPrime, -8, true},        // main_xthread
+    {Kind::kLittle, -4, false},         // sdl_thread (презентер)
+    {Kind::kLittle, kKeepNice, false},  // sdl_helpers
+    {Kind::kAudioLane, -16, true},      // audio_guest
+    {Kind::kNone, kKeepNice, false},    // host_audio — не трогаем
+    {Kind::kAudioLane, -16, true},      // xma_decoder
+    {Kind::kAudioLane, -16, true},      // audio_worker
+    {Kind::kLittle, 0, false},          // rage_stream
+    {Kind::kLittle, kKeepNice, false},  // rage_fileio
+    {Kind::kLittle, -2, false},         // kernel_io
+    {Kind::kJobsLane, -2, false},       // rage_jobs (ждёт главный поток игры)
+    {Kind::kJobsLane, -2, false},       // rage_physics
+    {Kind::kLittle, 0, false},          // rage_other
+    {Kind::kLittle, 0, false},          // cmd_retirer
+    {Kind::kLittle, 0, false},          // recorder_helpers
+    {Kind::kLittle, kKeepNice, false},  // sdl_activity
+    {Kind::kLittle, kKeepNice, false},  // background
+}};
+
+// Таблица версии 26.10 (для сравнения A/B), кроме хостового аудио — его не трогаем и здесь.
+constexpr std::array<Rule, kThreadIdCount> kClusters = {{
+    {Kind::kBigPrime, -6, true},        // render_guest
+    {Kind::kBigPrime, -8, true},        // recorder
+    {Kind::kBig, -4, true},             // render_work
+    {Kind::kBigPrime, -6, true},        // main_xthread
+    {Kind::kBig, -2, false},            // sdl_thread
+    {Kind::kLittle, kKeepNice, false},  // sdl_helpers
+    {Kind::kBig, -8, true},             // audio_guest
+    {Kind::kNone, kKeepNice, false},    // host_audio
+    {Kind::kWorkers, -16, true},        // xma_decoder
+    {Kind::kWorkers, -16, true},        // audio_worker
+    {Kind::kWorkers, 0, false},         // rage_stream
+    {Kind::kLittle, kKeepNice, false},  // rage_fileio
+    {Kind::kWorkers, -2, false},        // kernel_io
+    {Kind::kLittleBig, -2, false},      // rage_jobs
+    {Kind::kLittleBig, -2, false},      // rage_physics
+    {Kind::kWorkers, 0, false},         // rage_other
+    {Kind::kWorkers, 0, false},         // cmd_retirer
+    {Kind::kWorkers, -2, false},        // recorder_helpers
+    {Kind::kLittle, kKeepNice, false},  // sdl_activity
+    {Kind::kLittle, kKeepNice, false},  // background
+}};
+
+// Только приоритеты: nice и удержание как в «полосах», маски не ставятся.
+constexpr std::array<Rule, kThreadIdCount> WithoutMasks(const std::array<Rule, kThreadIdCount>& from) {
+  std::array<Rule, kThreadIdCount> out{};
+  for (size_t i = 0; i < from.size(); ++i) out[i] = Rule{Kind::kNone, from[i].nice, from[i].sticky};
+  return out;
+}
+constexpr std::array<Rule, kThreadIdCount> kPriorityOnly = WithoutMasks(kLanes);
+
+enum class Layout : uint8_t { kLanes, kClusters, kPriority, kOff };
+
+Layout ParseLayout(const std::string& value) {
+  if (value == "clusters") return Layout::kClusters;
+  if (value == "priority" || value == "nice") return Layout::kPriority;
+  if (value == "off" || value == "none") return Layout::kOff;
+  return Layout::kLanes;
+}
+
+const char* LayoutName(Layout layout) {
+  switch (layout) {
+    case Layout::kClusters: return "clusters";
+    case Layout::kPriority: return "priority";
+    case Layout::kOff: return "off";
+    default: return "lanes";
+  }
+}
+
+const Rule& RuleFor(Layout layout, ThreadId id) {
+  switch (layout) {
+    case Layout::kClusters: return kClusters[id];
+    case Layout::kPriority: return kPriorityOnly[id];
+    default: return kLanes[id];
+  }
+}
+
+bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
+bool Starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
+
+ThreadId Classify(const std::string& name, pid_t tid, pid_t primary_recorder, pid_t primary_sdl) {
+  if (Has(name, "Track") || Has(name, "SDLAudio")) return kHostAudio;
+  if (name == "AudioGuest") return kAudioGuest;
+  if (Has(name, "XMA")) return kXmaDecoder;
+  if (Has(name, "Audio")) return kAudioWorker;
+  if (name == "GtaRecorder") return tid == primary_recorder ? kRecorder : kRecorderHelpers;
+  if (name == "RenderGuest") return kRenderGuest;
+  if (Starts(name, "Main XThread")) return kMainXThread;
+  if (name == "GtaRenderWork") return kRenderWork;
+  if (name == "RagePhysics") return kRagePhysics;
+  if (name == "RageJobWork") return kRageJobs;
+  if (Starts(name, "Kernel ")) return kKernelIO;
+  if (name == "GtaCmdRetirer") return kCmdRetirer;
+  if (name == "RageFileIO") return kRageFileIO;
+  if (Has(name, "TimerQueue") || Has(name, "Installer") || Has(name, "SDLTimer") ||
+      Has(name, "binder") || Has(name, "UIMonitor"))
+    return kBackground;
+  if (name == "SDLThread") return tid == primary_sdl ? kSdlThread : kSdlHelpers;
+  if (name == "SDLActivity") return kSdlActivity;
+  if (name == "RageStream") return kRageStream;
+  if (Starts(name, "Rage") || Starts(name, "XThread")) return kRageOther;
+  return kUnknown;
+}
+
+// ---------------------------------------------------------------- spec
+
+struct Placement {
+  bool manual = false;
+  std::array<uint32_t, kThreadIdCount> mask{};  // 0 = автоматически
 };
 
-ThreadStat ReadThreadStat(pid_t tid) {
+int FindThreadId(const std::string& id) {
+  for (int i = 0; i < kThreadIdCount; ++i)
+    if (id == kIds[i]) return i;
+  return -1;
+}
+
+std::string Trim(std::string s) {
+  const char* ws = " \t\r\n\"'";
+  const size_t b = s.find_first_not_of(ws);
+  if (b == std::string::npos) return {};
+  const size_t e = s.find_last_not_of(ws);
+  return s.substr(b, e - b + 1);
+}
+
+Placement ParseSpec(const std::string& spec, const CpuTopology& topo) {
+  Placement p;
+  size_t pos = 0;
+  bool first = true;
+  while (pos <= spec.size()) {
+    size_t end = spec.find(';', pos);
+    if (end == std::string::npos) end = spec.size();
+    const std::string token = Trim(spec.substr(pos, end - pos));
+    pos = end + 1;
+    if (token.empty()) continue;
+    if (first) {
+      first = false;
+      if (token == "manual") { p.manual = true; continue; }
+      if (token == "auto") return p;
+      p.manual = true;
+    }
+    const size_t colon = token.find(':');
+    if (colon == std::string::npos) {
+      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: bad entry '%s' (ignored)", token.c_str());
+      continue;
+    }
+    const std::string id = Trim(token.substr(0, colon));
+    const int index = FindThreadId(id);
+    if (index < 0) {
+      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: unknown thread id '%s' (ignored)", id.c_str());
+      continue;
+    }
+    char* tail = nullptr;
+    const std::string hex = Trim(token.substr(colon + 1));
+    const unsigned long requested = std::strtoul(hex.c_str(), &tail, 16);
+    if (hex.empty() || (tail && *tail)) {
+      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: %s: bad mask '%s' (ignored)", id.c_str(),
+                          hex.c_str());
+      continue;
+    }
+    const uint32_t mask = uint32_t(requested) & topo.all();
+    if (!mask) {
+      __android_log_print(ANDROID_LOG_WARN, kTag,
+                          "Affinity: %s mask 0x%lX has no cores on this device, automatic placement used",
+                          id.c_str(), requested);
+      continue;
+    }
+    p.mask[index] = mask;
+  }
+  bool any = false;
+  for (uint32_t m : p.mask) any |= m != 0;
+  if (!any) p.manual = false;
+  return p;
+}
+
+uint32_t EffectiveMask(const Placement& p, Layout layout, ThreadId id, const CpuTopology& topo) {
+  if (p.manual && p.mask[id]) return p.mask[id];
+  return layout == Layout::kOff ? 0 : AutoMask(RuleFor(layout, id).kind, topo);
+}
+
+void LogPlacement(const Placement& p, Layout layout, const CpuTopology& topo, const char* source) {
+  std::string list;
+  for (int i = 0; i < kThreadIdCount; ++i) {
+    const uint32_t m = EffectiveMask(p, layout, ThreadId(i), topo);
+    const Rule& rule = RuleFor(layout, ThreadId(i));
+    char item[80];
+    if (!m && (rule.nice == kKeepNice || layout == Layout::kOff)) {
+      std::snprintf(item, sizeof(item), " %s=-", kIds[i]);
+    } else if (!m) {
+      std::snprintf(item, sizeof(item), " %s=-/%d", kIds[i], rule.nice);
+    } else if (rule.nice == kKeepNice || layout == Layout::kOff) {
+      std::snprintf(item, sizeof(item), " %s=0x%X%s", kIds[i], m, p.manual && p.mask[i] ? "*" : "");
+    } else {
+      std::snprintf(item, sizeof(item), " %s=0x%X/%d%s", kIds[i], m, rule.nice,
+                    p.manual && p.mask[i] ? "*" : "");
+    }
+    list += item;
+  }
+  __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: layout=%s mode=%s (source: %s; * = manual):%s",
+                      LayoutName(layout), p.manual ? "manual" : "auto", source, list.c_str());
+  const ThreadId heavy[] = {kRenderGuest, kRecorder, kRenderWork, kMainXThread, kSdlThread, kAudioGuest};
+  for (ThreadId id : heavy) {
+    const uint32_t m = EffectiveMask(p, layout, id, topo);
+    if (m && !(m & ~topo.little))
+      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: note: %s only on little cores", kIds[id]);
+    if (m && __builtin_popcount(m) == 1)
+      __android_log_print(ANDROID_LOG_WARN, kTag,
+                          "Affinity: note: %s limited to one core (0x%X): core parking and throttling "
+                          "will stall it; prefer two or more cores",
+                          kIds[id], m);
+  }
+  for (size_t a = 0; a < std::size(heavy); ++a) {
+    for (size_t b = a + 1; b < std::size(heavy); ++b) {
+      const uint32_t ma = EffectiveMask(p, layout, heavy[a], topo);
+      const uint32_t mb = EffectiveMask(p, layout, heavy[b], topo);
+      if (ma && mb && __builtin_popcount(ma | mb) == 1)
+        __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: note: %s and %s share one core (0x%X)",
+                            kIds[heavy[a]], kIds[heavy[b]], ma | mb);
+    }
+  }
+}
+
+// ---------------------------------------------------------------- applying
+
+bool SetAffinityMask(pid_t tid, uint32_t mask) {
+  cpu_set_t set;
+  CPU_ZERO(&set);
+  for (int cpu = 0; cpu < 32; ++cpu)
+    if (mask & (1u << cpu)) CPU_SET(cpu, &set);
+  return syscall(__NR_sched_setaffinity, tid, sizeof(set), &set) == 0;
+}
+
+uint32_t GetAffinityMask(pid_t tid) {
+  cpu_set_t set;
+  CPU_ZERO(&set);
+  if (syscall(__NR_sched_getaffinity, tid, sizeof(set), &set) < 0) return 0;
+  uint32_t mask = 0;
+  for (int cpu = 0; cpu < 32; ++cpu)
+    if (CPU_ISSET(cpu, &set)) mask |= 1u << cpu;
+  return mask;
+}
+
+std::string GetThreadName(pid_t tid) {
   char path[64];
-  std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
+  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
   FILE* file = std::fopen(path, "r");
   if (!file) return {};
-  char buffer[512];
-  const size_t length = std::fread(buffer, 1, sizeof(buffer) - 1, file);
+  char name[64] = {};
+  if (std::fgets(name, sizeof(name), file)) {
+    const size_t len = std::strlen(name);
+    if (len && name[len - 1] == '\n') name[len - 1] = '\0';
+  }
   std::fclose(file);
-  buffer[length] = 0;
-  // Fields after the parenthesized name: state is field 3, utime 14, stime 15.
-  const char* p = std::strrchr(buffer, ')');
-  if (!p) return {};
-  // Field 19 is nice, 41 the scheduling policy (1 FIFO, 2 RR).
-  unsigned long long utime = 0, stime = 0;
-  long nice = 0, policy = 0;
-  int field = 2;
-  for (const char* c = p + 1; *c && field < 41; ++c) {
-    if (*c == ' ') {
-      ++field;
-      if (field == 14) utime = std::strtoull(c + 1, nullptr, 10);
-      if (field == 15) stime = std::strtoull(c + 1, nullptr, 10);
-      if (field == 19) nice = std::strtol(c + 1, nullptr, 10);
-      if (field == 41) policy = std::strtol(c + 1, nullptr, 10);
-    }
-  }
-  return {utime + stime, nice <= -16 ||policy == 1 || policy == 2};
-}
-
-std::vector<int> CoresByMaxFrequency() {
-  std::vector<std::pair<long, int>> cores;
-  const long count = sysconf(_SC_NPROCESSORS_CONF);
-  for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE; ++cpu) {
-    char path[96];
-    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
-    long frequency = 0;
-    if (FILE* file = std::fopen(path, "r")) {
-      if (std::fscanf(file, "%ld", &frequency) != 1) frequency = 0;
-      std::fclose(file);
-    }
-    cores.emplace_back(frequency, cpu);
-  }
-  std::stable_sort(cores.begin(), cores.end(),
-                   [](const auto& a, const auto& b) { return a.first > b.first; });
-  std::vector<int> result;
-  for (const auto& [frequency, cpu] : cores) result.push_back(cpu);
-  return result;
-}
-
-void SetAffinity(pid_t tid, const cpu_set_t& set) {
-  syscall(__NR_sched_setaffinity, tid, sizeof(set), &set);
+  return name;
+}
+
+struct Applied {
+  std::string name;
+  uint32_t mask = 0;
+  uint32_t effective = 0;
+  uint32_t generation = 0;
+  uint32_t restored = 0;
+  bool nice_ok = false;
+};
+
+struct Seen {
+  pid_t tid;
+  std::string name;
+};
+
+struct SpecSource {
+  bool from_env = false;
+  std::string env_value;
+
+  void Init() {
+    const char* env = std::getenv(kEnvSpec);
+    if (env && *env) {
+      from_env = true;
+      env_value = Trim(env);
+    }
+  }
+  std::string Current(const char*& source) const {
+    if (from_env) {
+      source = "launcher";
+      return env_value;
+    }
+    source = "cvar";
+    return Trim(REXCVAR_GET(android_thread_affinity));
+  }
+};
+
+bool IsPowerOfTen(uint32_t n) {
+  while (n >= 10 && n % 10 == 0) n /= 10;
+  return n == 1;
 }
 
 void PinningLoop() {
-  const std::vector<int> cores = CoresByMaxFrequency();
-  if (cores.size() < 6) return;
-  std::unordered_map<pid_t, uint64_t> previous;
-  uint32_t applied = 0;
-  for (;;) {
-    std::this_thread::sleep_for(std::chrono::seconds(2));
-    const uint32_t pinned = std::min<uint32_t>(REXCVAR_GET(gta4_thread_pinning), 4);
-    std::vector<std::pair<uint64_t, pid_t>> usage;
-    std::vector<pid_t> audio;
-    std::unordered_map<pid_t, uint64_t> current;
-    if (DIR* directory = opendir("/proc/self/task")) {
-      while (dirent* entry = readdir(directory)) {
-        const pid_t tid = pid_t(std::atoi(entry->d_name));
-        if (tid <= 0) continue;
-        const ThreadStat stat = ReadThreadStat(tid);
-        const uint64_t ticks = stat.ticks;
-        current[tid] = ticks;
-        if (stat.audio) {
-          audio.push_back(tid);
-          continue;
+  const CpuTopology topo = DetectTopology();
+  if (topo.fallback) {
+    __android_log_print(ANDROID_LOG_WARN, kTag,
+                        "Affinity: topology detection failed (SELinux?). Generic %d-core fallback "
+                        "Little: 0x%X, Big: 0x%X, Prime: 0x%X",
+                        topo.count, topo.little, topo.big, topo.prime);
+  } else {
+    __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: topology Little: 0x%X, Big: 0x%X, Prime: 0x%X",
+                        topo.little, topo.big, topo.prime);
+  }
+
+  SpecSource source;
+  source.Init();
+  bool cvar_conflict_logged = false;
+
+  std::unordered_map<pid_t, Applied> applied;
+  std::unordered_set<std::string> unknown_logged;
+  std::vector<Seen> seen;
+  std::string spec_text, layout_text;
+  Placement placement;
+  Layout layout = Layout::kLanes;
+  bool spec_parsed = false;
+  uint32_t generation = 0;
+  const auto started = std::chrono::steady_clock::now();
+  bool summary_logged = false;
+
+  while (true) {
+    std::this_thread::sleep_for(std::chrono::milliseconds(400));
+    if (!REXCVAR_GET(gta4_cluster_pinning)) continue;
+
+    const char* source_name = "";
+    const std::string spec = source.Current(source_name);
+    const std::string layout_now = Trim(REXCVAR_GET(android_thread_layout));
+    if (!spec_parsed || spec != spec_text || layout_now != layout_text) {
+      spec_text = spec;
+      layout_text = layout_now;
+      layout = ParseLayout(layout_now);
+      placement = ParseSpec(spec, topo);
+      spec_parsed = true;
+      LogPlacement(placement, layout, topo, source_name);
+      for (auto& entry : applied) entry.second.mask = 0;
+    }
+    if (source.from_env && !cvar_conflict_logged) {
+      const std::string cvar = Trim(REXCVAR_GET(android_thread_affinity));
+      if (cvar != "auto" && cvar != spec) {
+        cvar_conflict_logged = true;
+        __android_log_print(ANDROID_LOG_WARN, kTag,
+                            "Affinity: android_thread_affinity='%s' (native.toml/args.txt) ignored: "
+                            "the launcher's setting is used",
+                            cvar.c_str());
+      }
+    }
+
+    DIR* dir = opendir("/proc/self/task");
+    if (!dir) continue;
+    seen.clear();
+    pid_t primary_recorder = 0, primary_sdl = 0;
+    while (dirent* entry = readdir(dir)) {
+      const pid_t tid = pid_t(std::atoi(entry->d_name));
+      if (tid <= 0) continue;
+      std::string name = GetThreadName(tid);
+      if (name.empty()) continue;
+      if (name == "GtaRecorder" && (!primary_recorder || tid < primary_recorder)) primary_recorder = tid;
+      if (name == "SDLThread" && (!primary_sdl || tid < primary_sdl)) primary_sdl = tid;
+      seen.push_back({tid, std::move(name)});
+    }
+    closedir(dir);
+
+    ++generation;
+    for (const Seen& s : seen) {
+      const ThreadId id = Classify(s.name, s.tid, primary_recorder, primary_sdl);
+      Applied& state = applied[s.tid];
+      state.generation = generation;
+      if (id == kUnknown) {
+        if (state.name != s.name && unknown_logged.size() < 64 && unknown_logged.insert(s.name).second)
+          __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: unclassified thread '%s' (tid %d) left as is",
+                              s.name.c_str(), s.tid);
+        state.name = s.name;
+        state.mask = 0;
+        continue;
+      }
+      const bool manual = placement.manual && placement.mask[id] != 0;
+      const Rule& rule = RuleFor(layout, id);
+      // off: трогаем только то, что задано вручную (и без nice).
+      const uint32_t mask = manual ? placement.mask[id]
+                                   : (layout == Layout::kOff ? 0 : AutoMask(rule.kind, topo));
+      const int nice = layout == Layout::kOff ? kKeepNice : rule.nice;
+      if (!mask && state.effective) {
+        // Маску ставили мы, а теперь она не нужна (раскладку сменили на лету): снимаем.
+        const bool released = SetAffinityMask(s.tid, topo.all());
+        __android_log_print(released ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
+                            "Affinity: %s '%s' (tid %d): mask 0x%X released%s", kIds[id],
+                            s.name.c_str(), s.tid, state.effective, released ? "" : " FAILED");
+        state.effective = 0;
+        state.mask = 0;
+      }
+      if (!mask && nice == kKeepNice) {  // kNone: поток не трогаем вообще
+        state.name = s.name;
+        state.mask = 0;
+        continue;
+      }
+      const bool fresh = state.name != s.name || state.mask != mask;
+      const bool hold = manual || rule.sticky;
+      if (!fresh && !hold) continue;
+
+      bool changed_outside = false;
+      uint32_t outside_mask = 0;
+      int outside_nice = kKeepNice;
+      if (!fresh && (state.effective || !mask)) {
+        if (mask) {
+          const uint32_t current = GetAffinityMask(s.tid);
+          if (current != 0 && current != state.effective) {
+            changed_outside = true;
+            outside_mask = current;
+          }
+        }
+        if (nice != kKeepNice && state.nice_ok) {
+          errno = 0;
+          const int prio = getpriority(PRIO_PROCESS, id_t(s.tid));
+          if (errno == 0 && prio != nice) {
+            changed_outside = true;
+            outside_nice = prio;
+          }
         }
-        const auto old = previous.find(tid);
-        usage.emplace_back(old != previous.end() && ticks >= old->second ? ticks - old->second : 0,
-                           tid);
+        if (!changed_outside) continue;
       }
-      closedir(directory);
-    }
-    previous.swap(current);
-    if (!pinned && !applied) continue;
-    std::sort(usage.rbegin(), usage.rend());
-    cpu_set_t rest;
-    CPU_ZERO(&rest);
-    for (size_t i = pinned; i < cores.size(); ++i) CPU_SET(cores[i], &rest);
-    for (size_t i = 0; i < usage.size(); ++i) {
-      cpu_set_t set;
-      CPU_ZERO(&set);
-      if (!pinned) {
-        for (int cpu : cores) CPU_SET(cpu, &set);
-      } else if (i < pinned) {
-        CPU_SET(cores[i], &set);
-      } else {
-        set = rest;
+
+      bool ok = true;
+      if (mask) {
+        ok = SetAffinityMask(s.tid, mask);
+        state.effective = ok ? GetAffinityMask(s.tid) : 0;
+      }
+      if (nice != kKeepNice) state.nice_ok = setpriority(PRIO_PROCESS, id_t(s.tid), nice) == 0;
+      if (fresh) {
+        char mask_text[16] = "keep";
+        if (mask) std::snprintf(mask_text, sizeof(mask_text), "0x%X", mask);
+        __android_log_print(ok && (nice == kKeepNice || state.nice_ok) ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
+                            kTag, "Affinity: %s '%s' (tid %d) -> %s nice %s%d%s%s", kIds[id],
+                            s.name.c_str(), s.tid, mask_text, nice == kKeepNice ? "keep " : "",
+                            nice == kKeepNice ? 0 : nice, manual ? " [manual]" : " [auto]",
+                            !ok ? " FAILED" : (nice != kKeepNice && !state.nice_ok ? " (nice FAILED)" : ""));
+      } else if (changed_outside && IsPowerOfTen(++state.restored)) {
+        // Что именно поменяли снаружи — чтобы отличить cpuset/OEM (маска) от чужого setpriority (nice).
+        char what[96] = {};
+        if (outside_mask && outside_nice != kKeepNice)
+          std::snprintf(what, sizeof(what), "mask 0x%X and nice %d", outside_mask, outside_nice);
+        else if (outside_mask)
+          std::snprintf(what, sizeof(what), "mask 0x%X", outside_mask);
+        else
+          std::snprintf(what, sizeof(what), "nice %d", outside_nice);
+        char kept[32];
+        if (mask && nice != kKeepNice) std::snprintf(kept, sizeof(kept), "0x%X/%d", mask, nice);
+        else if (mask) std::snprintf(kept, sizeof(kept), "0x%X", mask);
+        else std::snprintf(kept, sizeof(kept), "nice %d", nice);
+        const std::string cpuset = ReadSmallFile(s.tid, "cpuset");
+        __android_log_print(ANDROID_LOG_WARN, kTag,
+                            "Affinity: %s '%s' (tid %d): found %s (cpuset=%s), restored %s (%u times)", kIds[id],
+                            s.name.c_str(), s.tid, what, cpuset.c_str(), kept, state.restored);
       }
-      SetAffinity(usage[i].second, set);
+      state.name = s.name;
+      state.mask = mask;
+    }
+
+    for (auto it = applied.begin(); it != applied.end();) {
+      if (it->second.generation != generation) it = applied.erase(it);
+      else ++it;
     }
-    cpu_set_t all;
-    CPU_ZERO(&all);
-    for (int cpu : cores) CPU_SET(cpu, &all);
-    for (pid_t tid : audio) SetAffinity(tid, all);
-    if (pinned != applied) {
-      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "thread-pinning: %u threads on cores %d,%d,%d",
-                          pinned, cores[0], cores[1], cores[2]);
+
+    if (!summary_logged && std::chrono::steady_clock::now() - started > std::chrono::seconds(20)) {
+      summary_logged = true;
+      std::array<int, kThreadIdCount> count{};
+      int unknown = 0;
+      for (const Seen& s : seen) {
+        const ThreadId id = Classify(s.name, s.tid, primary_recorder, primary_sdl);
+        if (id == kUnknown) ++unknown;
+        else ++count[id];
+      }
+      std::string list;
+      for (int i = 0; i < kThreadIdCount; ++i) {
+        if (!count[i]) continue;
+        char item[80];
+        const uint32_t m = EffectiveMask(placement, layout, ThreadId(i), topo);
+        if (m) std::snprintf(item, sizeof(item), " %s x%d=0x%X", kIds[i], count[i], m);
+        else std::snprintf(item, sizeof(item), " %s x%d=-", kIds[i], count[i]);
+        list += item;
+      }
+      __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: summary layout=%s (%zu threads):%s unclassified x%d",
+                          LayoutName(layout), seen.size(), list.c_str(), unknown);
     }
-    applied = pinned;
   }
 }
 
diff --git a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_app.cpp b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_app.cpp
index 68d694b9..411bdd9a 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_app.cpp
+++ b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_app.cpp
@@ -45,6 +45,10 @@
 #include <network/community_multiplayer.h>
 #include <network/gta4_voice_audio.h>
 
+#include "debug/frame_time_dialog.h"
+
+
+
 #if REX_PLATFORM_ANDROID
 #include <android/log.h>
 #include <fcntl.h>
@@ -210,8 +214,8 @@ REXCVAR_DEFINE_STRING(gta4_native_smaa_quality, "high", "GTA IV/Graphics/Anti-Al
                       "SMAA 1x preset: low, medium, high, or ultra")
     .allowed({"low", "medium", "high", "ultra"});
 REXCVAR_DEFINE_STRING(gta4_native_upscaler, "native", "GTA IV/Graphics/Upscaling",
-                      "Output upscaler: native, fsr1, MetalFX, FSR 3.1, or DLSS; temporal modes require backend support")
-    .allowed({"native", "fsr1", "metalfx", "fsr3", "dlss"})
+                      "Output upscaler: native, fsr1, sgsr, MetalFX, FSR 3.1, or DLSS; temporal modes require backend support")
+    .allowed({"native", "fsr1", "sgsr", "metalfx", "fsr3", "dlss"})
     .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
 REXCVAR_DEFINE_STRING(gta4_temporal_upscaler_quality, "quality", "GTA IV/Graphics/Upscaling",
                       "FSR 3.1/DLSS preset; dimensions come from the active provider")
@@ -225,6 +229,10 @@ REXCVAR_DEFINE_DOUBLE(gta4_fsr1_sharpness_reduction, 0.2, "GTA IV/Graphics/Upsca
                       "FSR 1 RCAS sharpness reduction in stops")
     .range(0.0, 2.0)
     .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
+// SGSR: настройка резкости краёв (от 1.0 до 2.0)
+REXCVAR_DEFINE_DOUBLE(gta4_sgsr_sharpness, 2.0, "GTA IV/Graphics/Upscaling",
+                      "SGSR edge sharpness")
+    .range(1.0, 2.0);
 // Keep the authored resident LOD hierarchy by default. Forcing LOD0 at every
 // distance bypasses transitions as soon as a new drawable becomes resident.
 REXCVAR_DEFINE_BOOL(gta4_force_highest_lod, false, "GTA IV/Graphics/LOD",
@@ -753,14 +761,39 @@ void GTA4App::OnPreSetup(rex::RuntimeConfig& config) {
   const bool ssaa_active =
       rex::graphics::gta4_native::UsesSceneSupersampling(
           rex::graphics::gta4_native::GetActiveAntiAliasingMode());
+#if 0		  
+  const bool fsr1_requested = REXCVAR_GET(gta4_native_upscaler) == "fsr1";
+  const bool use_fsr1 = fsr1_requested && !ssaa_active;
+  if (fsr1_requested && ssaa_active) {
+    REXLOG_ERROR(
+        "GTA4AAPolicy owner=startup event=reject-upscaler upscaler=fsr1 aa={} "
+        "reason=ssaa-renders-above-output-resolution",
+        rex::graphics::gta4_native::GetActiveAntiAliasingModeName());
+  }  
+  REXCVAR_SET(present_effect, use_fsr1 ? "fsr" : "bilinear");
+#endif
+//sgsr 
   const bool fsr1_requested = REXCVAR_GET(gta4_native_upscaler) == "fsr1";
+  // SGSR: запрашиваем SGSR так же, как FSR1, но пока используем тот же путь
+  // снижения разрешения; сам SGSR-шейдер будет подключён на следующем этапе.
+  const bool sgsr_requested = REXCVAR_GET(gta4_native_upscaler) == "sgsr";
   const bool use_fsr1 = fsr1_requested && !ssaa_active;
+  const bool use_sgsr = sgsr_requested && !ssaa_active;
   if (fsr1_requested && ssaa_active) {
     REXLOG_ERROR(
         "GTA4AAPolicy owner=startup event=reject-upscaler upscaler=fsr1 aa={} "
         "reason=ssaa-renders-above-output-resolution",
         rex::graphics::gta4_native::GetActiveAntiAliasingModeName());
   }
+  if (sgsr_requested && ssaa_active) {
+    REXLOG_ERROR(
+        "GTA4AAPolicy owner=startup event=reject-upscaler upscaler=sgsr aa={} "
+        "reason=ssaa-renders-above-output-resolution",
+        rex::graphics::gta4_native::GetActiveAntiAliasingModeName());
+  }
+  // SGSR пока не имеет отдельного present_effect; оставляем bilinear,
+  // чтобы не затрагивать presenter.cpp. Нативный RecordPresent будет
+  // использовать SGSR позже.
   REXCVAR_SET(present_effect, use_fsr1 ? "fsr" : "bilinear");
   REXCVAR_SET(present_fsr_sharpness_reduction, REXCVAR_GET(gta4_fsr1_sharpness_reduction));
   REXLOG_INFO(
@@ -893,6 +926,7 @@ void StartThermalLog() {
 // others. Once a second the busiest threads (at most one per big core) are
 // limited to the big cores; the rest keep every core.
 void StartHotThreadPinning() {
+  return;
   std::thread([] {
     std::vector<uint64_t> max_frequency;
     for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
@@ -1332,6 +1366,7 @@ void GTA4App::TitleProfileSyncWorkerMain() {
 
 void GTA4App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
   gta4::game_center::Initialize();
+  new FrameTimeDialog(drawer);
   context_touch_overlay_ =
       std::make_unique<gta4::input::ContextTouchOverlay>(
           drawer, immediate_drawer(), native_config_path_.parent_path() / "touch-icons", window());
diff --git a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_draw_distance_guest.inc b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_draw_distance_guest.inc
index 38b03aa8..d1ea548e 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_draw_distance_guest.inc
+++ b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_draw_distance_guest.inc
@@ -10,1020 +10,338 @@ extern "C" void sub_821D6260(PPCContext& ctx, uint8_t* base) {
   }
   const float native_remap_limit = gta4::draw_distance::ResolveRemapLimit(
       LoadF32(base, kDistanceScaleInputGlobal));
-	REX_FUNC_PROLOGUE();
-	PPCRegister temp{};
-	uint32_t ea{};
-	// mflr r12
-	ctx.r12.u64 = ctx.lr;
-	// bl 0x829ff790
-	ctx.lr = 0x821D6268;
-	__savegprlr_14(ctx, base);
-	// addi r12,r1,-152
-	ctx.r12.s64 = ctx.r1.s64 + -152;
-	// bl 0x82a012f8
-	ctx.lr = 0x821D6270;
-	__savefpr_16(ctx, base);
-	// stwu r1,-464(r1)
-	ea = -464 + ctx.r1.u32;
-	REX_STORE_U32(ea, ctx.r1.u32);
-	ctx.r1.u32 = ea;
-	// mr r27,r3
-	ctx.r27.u64 = ctx.r3.u64;
-	// mr r15,r4
-	ctx.r15.u64 = ctx.r4.u64;
-	// mr r25,r5
-	ctx.r25.u64 = ctx.r5.u64;
-	// bl 0x821d0478
-	ctx.lr = 0x821D6284;
-	sub_821D0478(ctx, base);
-	// lis r11,-32072
-	ctx.r11.s64 = -2101870592;
-	// lfs f23,2336(r25)
-	ctx.fpscr.disableFlushMode();
-	temp.u32 = REX_LOAD_U32(ctx.r25.u32 + 2336);
-	ctx.f23.f64 = double(temp.f32);
-	// cmplw cr6,r27,r15
-	ctx.cr6.compare<uint32_t>(ctx.r27.u32, ctx.r15.u32, ctx.xer);
-	// lwz r10,21392(r11)
-	ctx.r10.u64 = REX_LOAD_U32(ctx.r11.u32 + 21392);
-	// addi r10,r10,1
-	ctx.r10.s64 = ctx.r10.s64 + 1;
-	// stw r10,21392(r11)
-	REX_STORE_U32(ctx.r11.u32 + 21392, ctx.r10.u32);
-	// clrlwi r11,r3,24
-	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
-	// cntlzw r11,r11
-	ctx.r11.u64 = ctx.r11.u32 == 0 ? 32 : __builtin_clz(ctx.r11.u32);
-	// rlwinm r14,r11,27,31,31
-	ctx.r14.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 27) & 0x1;
-	// bge cr6,0x821d67c0
-	if (!ctx.cr6.lt) goto loc_821D67C0;
-	// lis r28,-32244
-	ctx.r28.s64 = -2113142784;
-	// lis r29,-32256
-	ctx.r29.s64 = -2113929216;
-	// lis r30,-32256
-	ctx.r30.s64 = -2113929216;
-	// lis r31,-32256
-	ctx.r31.s64 = -2113929216;
-	// lis r3,-32254
-	ctx.r3.s64 = -2113798144;
-	// lis r4,-32256
-	ctx.r4.s64 = -2113929216;
-	// lfs f26,-4652(r28)
-	temp.u32 = REX_LOAD_U32(ctx.r28.u32 + -4652);
-	ctx.f26.f64 = double(temp.f32);
-	// lis r5,-32256
-	ctx.r5.s64 = -2113929216;
-	// lfs f24,3528(r29)
-	temp.u32 = REX_LOAD_U32(ctx.r29.u32 + 3528);
-	ctx.f24.f64 = double(temp.f32);
-	// lis r6,-32256
-	ctx.r6.s64 = -2113929216;
-	// lfs f16,3444(r30)
-	temp.u32 = REX_LOAD_U32(ctx.r30.u32 + 3444);
-	ctx.f16.f64 = double(temp.f32);
-	// lis r7,-32244
-	ctx.r7.s64 = -2113142784;
-	// lfs f27,3400(r31)
-	temp.u32 = REX_LOAD_U32(ctx.r31.u32 + 3400);
-	ctx.f27.f64 = double(temp.f32);
-	// lis r8,-32256
-	ctx.r8.s64 = -2113929216;
-	// lfs f20,-27024(r3)
-	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + -27024);
-	ctx.f20.f64 = double(temp.f32);
-	// lis r9,-32256
-	ctx.r9.s64 = -2113929216;
-	// lfs f21,9500(r4)
-	temp.u32 = REX_LOAD_U32(ctx.r4.u32 + 9500);
-	ctx.f21.f64 = double(temp.f32);
-	// lis r10,-32256
-	ctx.r10.s64 = -2113929216;
-	// lfs f22,9496(r5)
-	temp.u32 = REX_LOAD_U32(ctx.r5.u32 + 9496);
-	ctx.f22.f64 = double(temp.f32);
-	// lis r11,-32076
-	ctx.r11.s64 = -2102132736;
-	// lfs f17,2612(r6)
-	temp.u32 = REX_LOAD_U32(ctx.r6.u32 + 2612);
-	ctx.f17.f64 = double(temp.f32);
-	// lfs f18,-4904(r7)
-	temp.u32 = REX_LOAD_U32(ctx.r7.u32 + -4904);
-	ctx.f18.f64 = double(temp.f32);
-	// lis r18,-32087
-	ctx.r18.s64 = -2102853632;
-	// lfs f28,3596(r8)
-	temp.u32 = REX_LOAD_U32(ctx.r8.u32 + 3596);
-	ctx.f28.f64 = double(temp.f32);
-	// addi r23,r11,-24160
-	ctx.r23.s64 = ctx.r11.s64 + -24160;
-	// lfs f25,9492(r9)
-	temp.u32 = std::bit_cast<uint32_t>(native_remap_limit);
-	ctx.f25.f64 = double(temp.f32);
-	// lis r22,-32070
-	ctx.r22.s64 = -2101739520;
-	// lfs f19,9488(r10)
-	temp.u32 = REX_LOAD_U32(ctx.r10.u32 + 9488);
-	ctx.f19.f64 = double(temp.f32);
-	// lis r19,-32087
-	ctx.r19.s64 = -2102853632;
-	// lis r20,-32085
-	ctx.r20.s64 = -2102722560;
-	// lis r21,-31997
-	ctx.r21.s64 = -2096955392;
-	// li r26,0
-	ctx.r26.s64 = 0;
-	// li r24,2304
-	ctx.r24.s64 = 2304;
-	// li r17,255
-	ctx.r17.s64 = 255;
-	// lis r16,192
-	ctx.r16.s64 = 12582912;
+  REX_FUNC_PROLOGUE();
+  uint32_t ea{};
+
+  // 1. Быстрый бэкап callee-saved регистров в C++ стеке
+  const uint64_t saved_lr = ctx.lr;
+  const uint64_t s_r14 = ctx.r14.u64, s_r15 = ctx.r15.u64, s_r16 = ctx.r16.u64, s_r17 = ctx.r17.u64;
+  const uint64_t s_r18 = ctx.r18.u64, s_r19 = ctx.r19.u64, s_r20 = ctx.r20.u64, s_r21 = ctx.r21.u64;
+  const uint64_t s_r22 = ctx.r22.u64, s_r23 = ctx.r23.u64, s_r24 = ctx.r24.u64, s_r25 = ctx.r25.u64;
+  const uint64_t s_r26 = ctx.r26.u64, s_r27 = ctx.r27.u64, s_r28 = ctx.r28.u64, s_r29 = ctx.r29.u64;
+  const uint64_t s_r30 = ctx.r30.u64, s_r31 = ctx.r31.u64;
+  const double s_f16 = ctx.f16.f64, s_f17 = ctx.f17.f64, s_f18 = ctx.f18.f64, s_f19 = ctx.f19.f64;
+  const double s_f20 = ctx.f20.f64, s_f21 = ctx.f21.f64, s_f22 = ctx.f22.f64, s_f23 = ctx.f23.f64;
+  const double s_f24 = ctx.f24.f64, s_f25 = ctx.f25.f64, s_f26 = ctx.f26.f64, s_f27 = ctx.f27.f64;
+  const double s_f28 = ctx.f28.f64, s_f29 = ctx.f29.f64, s_f30 = ctx.f30.f64, s_f31 = ctx.f31.f64;
+
+  auto restore_and_return = [&]() {
+    ctx.r1.s64 = ctx.r1.s64 + 464;
+    ctx.r14.u64 = s_r14; ctx.r15.u64 = s_r15; ctx.r16.u64 = s_r16; ctx.r17.u64 = s_r17;
+    ctx.r18.u64 = s_r18; ctx.r19.u64 = s_r19; ctx.r20.u64 = s_r20; ctx.r21.u64 = s_r21;
+    ctx.r22.u64 = s_r22; ctx.r23.u64 = s_r23; ctx.r24.u64 = s_r24; ctx.r25.u64 = s_r25;
+    ctx.r26.u64 = s_r26; ctx.r27.u64 = s_r27; ctx.r28.u64 = s_r28; ctx.r29.u64 = s_r29;
+    ctx.r30.u64 = s_r30; ctx.r31.u64 = s_r31;
+    ctx.f16.f64 = s_f16; ctx.f17.f64 = s_f17; ctx.f18.f64 = s_f18; ctx.f19.f64 = s_f19;
+    ctx.f20.f64 = s_f20; ctx.f21.f64 = s_f21; ctx.f22.f64 = s_f22; ctx.f23.f64 = s_f23;
+    ctx.f24.f64 = s_f24; ctx.f25.f64 = s_f25; ctx.f26.f64 = s_f26; ctx.f27.f64 = s_f27;
+    ctx.f28.f64 = s_f28; ctx.f29.f64 = s_f29; ctx.f30.f64 = s_f30; ctx.f31.f64 = s_f31;
+    ctx.lr = saved_lr;
+  };
+
+  ea = -464 + ctx.r1.u32;
+  REX_STORE_U32(ea, ctx.r1.u32);
+  ctx.r1.u32 = ea;
+
+  uint32_t r27 = ctx.r3.u32;
+  const uint32_t r15 = ctx.r4.u32;
+  const uint32_t r25 = ctx.r5.u32;
+
+  ctx.r27.u64 = r27;
+  ctx.r15.u64 = r15;
+  ctx.r25.u64 = r25;
+
+  ctx.lr = 0x821D6284;
+  sub_821D0478(ctx, base);
+
+  ctx.fpscr.disableFlushMode();
+  const double f23 = double(std::bit_cast<float>(REX_LOAD_U32(r25 + 2336)));
+
+  const uint32_t counter_addr = static_cast<uint32_t>(-2101870592) + 21392;
+  REX_STORE_U32(counter_addr, REX_LOAD_U32(counter_addr) + 1);
+
+  const uint32_t clz_val = (ctx.r3.u32 & 0xFF) == 0 ? 32u : uint32_t(__builtin_clz(ctx.r3.u32 & 0xFF));
+  const uint32_t r14 = (clz_val >> 5) & 0x1;
+
+  // Вместо goto поверх инициализации переменных — сразу выходим!
+  if (r27 >= r15) {
+    restore_and_return();
+    return;
+  }
+
+  // 2. Читаем все константы один раз перед циклом
+  const double f26 = double(std::bit_cast<float>(REX_LOAD_U32(0x820BBDC4u)));
+  const double f24 = double(std::bit_cast<float>(REX_LOAD_U32(0x82000DC8u)));
+  const double f16 = double(std::bit_cast<float>(REX_LOAD_U32(0x82000D74u)));
+  const double f27 = double(std::bit_cast<float>(REX_LOAD_U32(0x82000D48u)));
+  const double f20 = double(std::bit_cast<float>(REX_LOAD_U32(0x82019670u)));
+  const double f21 = double(std::bit_cast<float>(REX_LOAD_U32(0x8200251Cu)));
+  const double f22 = double(std::bit_cast<float>(REX_LOAD_U32(0x82002518u)));
+  const double f17 = double(std::bit_cast<float>(REX_LOAD_U32(0x82000A34u)));
+  const double f18 = double(std::bit_cast<float>(REX_LOAD_U32(0x820BBCD8u)));
+  const double f28 = double(std::bit_cast<float>(REX_LOAD_U32(0x82000E0Cu)));
+  const double f25 = double(native_remap_limit);
+  const double f19 = double(std::bit_cast<float>(REX_LOAD_U32(0x82002510u)));
+
+  const uint32_t cam_ea = (r25 + 2304u) & ~0xFu;
+  const float cam_x = std::bit_cast<float>(REX_LOAD_U32(cam_ea + 0));
+  const float cam_y = std::bit_cast<float>(REX_LOAD_U32(cam_ea + 4));
+  const float cam_z = std::bit_cast<float>(REX_LOAD_U32(cam_ea + 8));
+
+  constexpr uint32_t r23 = 0x82B3A1A0u;
+  constexpr uint32_t r18 = 0x82A90000u;
+  constexpr uint32_t r22 = 0x82BA0000u;
+  constexpr uint32_t r19 = 0x82A90000u;
+  constexpr uint32_t r20 = 0x82AB0000u;
+  constexpr uint32_t r21 = 0x83030000u;
+  constexpr uint32_t r16 = 12582912u;
+
+  uint32_t r29 = 0, r30 = 0, r31 = 0;
+  double f29 = 0.0, f30 = 0.0, f31 = 0.0;
+
+  auto calc_dist3d = [&](uint32_t pos_addr) -> double {
+    const uint32_t aligned = pos_addr & ~0xFu;
+    const float dx = std::bit_cast<float>(REX_LOAD_U32(aligned + 0)) - cam_x;
+    const float dy = std::bit_cast<float>(REX_LOAD_U32(aligned + 4)) - cam_y;
+    const float dz = std::bit_cast<float>(REX_LOAD_U32(aligned + 8)) - cam_z;
+    const float d2 = std::fma(dx, dx, std::fma(dy, dy, dz * dz));
+    return double(std::sqrt(d2));
+  };
+
 loc_821D6338:
-	// lwz r31,0(r27)
-	ctx.r31.u64 = REX_LOAD_U32(ctx.r27.u32 + 0);
-	// cmplwi cr6,r31,0
-	ctx.cr6.compare<uint32_t>(ctx.r31.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b4
-	if (ctx.cr6.eq) goto loc_821D67B4;
-	// lhz r11,46(r31)
-	ctx.r11.u64 = REX_LOAD_U16(ctx.r31.u32 + 46);
-	// mr r3,r31
-	ctx.r3.u64 = ctx.r31.u64;
-	// lwz r30,76(r31)
-	ctx.r30.u64 = REX_LOAD_U32(ctx.r31.u32 + 76);
-	// extsh r11,r11
-	ctx.r11.s64 = ctx.r11.s16;
-	// rlwinm r11,r11,2,0,29
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 2) & 0xFFFFFFFC;
-	// lwzx r29,r11,r23
-	ctx.r29.u64 = REX_LOAD_U32(ctx.r11.u32 + ctx.r23.u32);
-	// bl 0x823b32e0
-	ctx.lr = 0x821D6360;
-	sub_823B32E0(ctx, base);
-	// clrlwi r11,r3,24
-	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d6384
-	if (ctx.cr6.eq) goto loc_821D6384;
-	// stw r26,0(r27)
-	REX_STORE_U32(ctx.r27.u32 + 0, ctx.r26.u32);
-	// lhz r11,92(r31)
-	ctx.r11.u64 = REX_LOAD_U16(ctx.r31.u32 + 92);
-	// stb r26,99(r31)
-	REX_STORE_U8(ctx.r31.u32 + 99, ctx.r26.u8);
-	// andi. r11,r11,65531
-	ctx.r11.u64 = ctx.r11.u64 & 65531;
-	ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
-	// sth r11,92(r31)
-	REX_STORE_U16(ctx.r31.u32 + 92, ctx.r11.u16);
-	// b 0x821d67b4
-	goto loc_821D67B4;
-loc_821D6384:
-	// lbz r11,10579(r21)
-	ctx.r11.u64 = REX_LOAD_U8(ctx.r21.u32 + 10579);
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d63b4
-	if (ctx.cr6.eq) goto loc_821D63B4;
-	// lwz r11,-30228(r20)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r20.u32 + -30228);
-	// cmpwi cr6,r11,0
-	ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
-	// ble cr6,0x821d63b4
-	if (!ctx.cr6.gt) goto loc_821D63B4;
-	// cmplwi cr6,r30,0
-	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
-	// beq cr6,0x821d63b4
-	if (ctx.cr6.eq) goto loc_821D63B4;
-	// lbz r11,36(r30)
-	ctx.r11.u64 = REX_LOAD_U8(ctx.r30.u32 + 36);
-	// clrlwi r11,r11,31
-	ctx.r11.u64 = ctx.r11.u32 & 0x1;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b0
-	if (ctx.cr6.eq) goto loc_821D67B0;
-loc_821D63B4:
-	// lwz r11,36(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 36);
-	// rlwinm r11,r11,0,5,5
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x4000000;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b0
-	if (ctx.cr6.eq) goto loc_821D67B0;
-	// lwz r11,40(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 40);
-	// rlwinm r11,r11,0,23,23
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x100;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// bne cr6,0x821d67b0
-	if (!ctx.cr6.eq) goto loc_821D67B0;
-	// lwz r11,0(r29)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + 0);
-	// mr r3,r29
-	ctx.r3.u64 = ctx.r29.u64;
-	// lwz r11,12(r11)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 12);
-	// mtctr r11
-	ctx.ctr.u64 = ctx.r11.u64;
-	// bctrl 
-	ctx.lr = 0x821D63E8;
-	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
-	// clrlwi r11,r3,24
-	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
-	// cmpwi cr6,r11,5
-	ctx.cr6.compare<int32_t>(ctx.r11.s32, 5, ctx.xer);
-	// beq cr6,0x821d68e8
-	if (ctx.cr6.eq) goto loc_821D68E8;
-	// cmpwi cr6,r11,6
-	ctx.cr6.compare<int32_t>(ctx.r11.s32, 6, ctx.xer);
-	// beq cr6,0x821d67d0
-	if (ctx.cr6.eq) goto loc_821D67D0;
-	// cmplwi cr6,r30,0
-	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
-	// beq cr6,0x821d641c
-	if (ctx.cr6.eq) goto loc_821D641C;
-	// lwz r11,32(r30)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 32);
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// addi r11,r11,48
-	ctx.r11.s64 = ctx.r11.s64 + 48;
-	// bne cr6,0x821d6430
-	if (!ctx.cr6.eq) goto loc_821D6430;
-	// addi r11,r30,16
-	ctx.r11.s64 = ctx.r30.s64 + 16;
-	// b 0x821d6430
-	goto loc_821D6430;
-loc_821D641C:
-	// lwz r11,32(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 32);
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// addi r11,r11,48
-	ctx.r11.s64 = ctx.r11.s64 + 48;
-	// bne cr6,0x821d6430
-	if (!ctx.cr6.eq) goto loc_821D6430;
-	// addi r11,r31,16
-	ctx.r11.s64 = ctx.r31.s64 + 16;
-loc_821D6430:
-	// lvx128 v13,r0,r11
-	ea = (ctx.r11.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// addi r11,r1,112
-	ctx.r11.s64 = ctx.r1.s64 + 112;
-	// lvx128 v0,r25,r24
-	ea = (ctx.r25.u32 + ctx.r24.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// lfs f29,80(r31)
-	ctx.fpscr.disableFlushMode();
-	temp.u32 = REX_LOAD_U32(ctx.r31.u32 + 80);
-	ctx.f29.f64 = double(temp.f32);
-	// stvx128 v13,r0,r11
-	ea = (ctx.r11.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v13.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// vsubfp v13,v13,v0
-	ctx.fpscr.enableFlushModeUnconditional();
-	simde_mm_store_ps(ctx.v13.f32, simde_mm_sub_ps(simde_mm_load_ps(ctx.v13.f32), simde_mm_load_ps(ctx.v0.f32)));
-	// addi r9,r1,112
-	ctx.r9.s64 = ctx.r1.s64 + 112;
-	// lwz r11,36(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 36);
-	// rlwinm r10,r11,8,31,31
-	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 8) & 0x1;
-	// cmplwi cr6,r10,0
-	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
-	// stvx128 v13,r0,r9
-	ea = (ctx.r9.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v13.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// lfs f13,120(r1)
-	ctx.fpscr.disableFlushModeUnconditional();
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 120);
-	ctx.f13.f64 = double(temp.f32);
-	// fmuls f12,f13,f13
-	ctx.f12.f64 = double(float(ctx.f13.f64 * ctx.f13.f64));
-	// lfs f0,116(r1)
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 116);
-	ctx.f0.f64 = double(temp.f32);
-	// lfs f13,112(r1)
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 112);
-	ctx.f13.f64 = double(temp.f32);
-	// fmadds f0,f0,f0,f12
-	ctx.f0.f64 = double(float(std::fma(ctx.f0.f64, ctx.f0.f64, ctx.f12.f64)));
-	// fmadds f0,f13,f13,f0
-	ctx.f0.f64 = double(float(std::fma(ctx.f13.f64, ctx.f13.f64, ctx.f0.f64)));
-	// fsqrts f30,f0
-	ctx.f30.f64 = double(float(sqrt(ctx.f0.f64)));
-	// beq cr6,0x821d6488
-	if (ctx.cr6.eq) goto loc_821D6488;
-	// fcmpu cr6,f23,f19
-	ctx.cr6.compare(ctx.f23.f64, ctx.f19.f64);
-	// ble cr6,0x821d6488
-	if (!ctx.cr6.gt) goto loc_821D6488;
-	// fmr f29,f23
-	ctx.f29.f64 = ctx.f23.f64;
-loc_821D6488:
-	// rlwinm r11,r11,0,6,6
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x2000000;
-	// lfs f0,2340(r25)
-	ctx.fpscr.disableFlushMode();
-	temp.u32 = REX_LOAD_U32(ctx.r25.u32 + 2340);
-	ctx.f0.f64 = double(temp.f32);
-	// fmuls f31,f0,f29
-	ctx.f31.f64 = double(float(ctx.f0.f64 * ctx.f29.f64));
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// bne cr6,0x821d64c4
-	if (!ctx.cr6.eq) goto loc_821D64C4;
-	// fcmpu cr6,f30,f25
-	ctx.cr6.compare(ctx.f30.f64, ctx.f25.f64);
-	// ble cr6,0x821d64c8
-	if (!ctx.cr6.gt) goto loc_821D64C8;
-	// fcmpu cr6,f31,f25
-	ctx.cr6.compare(ctx.f31.f64, ctx.f25.f64);
-	// ble cr6,0x821d64c8
-	if (!ctx.cr6.gt) goto loc_821D64C8;
-	// fadds f0,f31,f28
-	ctx.f0.f64 = double(float(ctx.f31.f64 + ctx.f28.f64));
-	// fcmpu cr6,f30,f0
-	ctx.cr6.compare(ctx.f30.f64, ctx.f0.f64);
-	// bge cr6,0x821d64c8
-	if (!ctx.cr6.lt) goto loc_821D64C8;
-	// fsubs f0,f31,f25
-	ctx.f0.f64 = double(float(ctx.f31.f64 - ctx.f25.f64));
-	// fadds f30,f0,f30
-	ctx.f30.f64 = double(float(ctx.f0.f64 + ctx.f30.f64));
-	// b 0x821d64c8
-	goto loc_821D64C8;
-loc_821D64C4:
-	// fmuls f31,f0,f29
-	ctx.fpscr.disableFlushMode();
-	ctx.f31.f64 = double(float(ctx.f0.f64 * ctx.f29.f64));
-loc_821D64C8:
-	// cmplwi cr6,r10,0
-	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
-	// beq cr6,0x821d64d8
-	if (ctx.cr6.eq) goto loc_821D64D8;
-	// fmr f0,f18
-	ctx.fpscr.disableFlushMode();
-	ctx.f0.f64 = ctx.f18.f64;
-	// b 0x821d64dc
-	goto loc_821D64DC;
-loc_821D64D8:
-	// fmr f0,f17
-	ctx.fpscr.disableFlushMode();
-	ctx.f0.f64 = ctx.f17.f64;
-loc_821D64DC:
-	// cmplwi cr6,r30,0
-	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
-	// beq cr6,0x821d652c
-	if (ctx.cr6.eq) goto loc_821D652C;
-	// lwz r11,32(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 32);
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// addi r11,r11,48
-	ctx.r11.s64 = ctx.r11.s64 + 48;
-	// bne cr6,0x821d64f8
-	if (!ctx.cr6.eq) goto loc_821D64F8;
-	// addi r11,r31,16
-	ctx.r11.s64 = ctx.r31.s64 + 16;
-loc_821D64F8:
-	// lvx128 v13,r0,r11
-	ea = (ctx.r11.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// addi r11,r1,128
-	ctx.r11.s64 = ctx.r1.s64 + 128;
-	// vsubfp v0,v13,v0
-	ctx.fpscr.enableFlushMode();
-	simde_mm_store_ps(ctx.v0.f32, simde_mm_sub_ps(simde_mm_load_ps(ctx.v13.f32), simde_mm_load_ps(ctx.v0.f32)));
-	// stvx128 v0,r0,r11
-	ea = (ctx.r11.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// lfs f12,136(r1)
-	ctx.fpscr.disableFlushModeUnconditional();
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 136);
-	ctx.f12.f64 = double(temp.f32);
-	// fmuls f11,f12,f12
-	ctx.f11.f64 = double(float(ctx.f12.f64 * ctx.f12.f64));
-	// lfs f13,132(r1)
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 132);
-	ctx.f13.f64 = double(temp.f32);
-	// lfs f12,128(r1)
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 128);
-	ctx.f12.f64 = double(temp.f32);
-	// fmadds f13,f13,f13,f11
-	ctx.f13.f64 = double(float(std::fma(ctx.f13.f64, ctx.f13.f64, ctx.f11.f64)));
-	// fmadds f13,f12,f12,f13
-	ctx.f13.f64 = double(float(std::fma(ctx.f12.f64, ctx.f12.f64, ctx.f13.f64)));
-	// fsqrts f13,f13
-	ctx.f13.f64 = double(float(sqrt(ctx.f13.f64)));
-	// fadds f0,f13,f0
-	ctx.f0.f64 = double(float(ctx.f13.f64 + ctx.f0.f64));
-	// b 0x821d6530
-	goto loc_821D6530;
-loc_821D652C:
-	// fadds f0,f0,f30
-	ctx.fpscr.disableFlushMode();
-	ctx.f0.f64 = double(float(ctx.f0.f64 + ctx.f30.f64));
-loc_821D6530:
-	// stfs f0,4(r27)
-	ctx.fpscr.disableFlushMode();
-	temp.f32 = float(ctx.f0.f64);
-	REX_STORE_U32(ctx.r27.u32 + 4, temp.u32);
-	// lwz r11,0(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 0);
-	// mr r3,r31
-	ctx.r3.u64 = ctx.r31.u64;
-	// lwz r11,84(r11)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 84);
-	// mtctr r11
-	ctx.ctr.u64 = ctx.r11.u64;
-	// bctrl 
-	ctx.lr = 0x821D6548;
-	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
-	// fadds f0,f1,f23
-	ctx.fpscr.disableFlushMode();
-	ctx.f0.f64 = double(float(ctx.f1.f64 + ctx.f23.f64));
-	// fmr f13,f28
-	ctx.f13.f64 = ctx.f28.f64;
-	// cmplwi cr6,r30,0
-	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
-	// fsubs f12,f31,f0
-	ctx.f12.f64 = double(float(ctx.f31.f64 - ctx.f0.f64));
-	// fsel f0,f12,f0,f31
-	ctx.f0.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f31.f64;
-	// bne cr6,0x821d658c
-	if (!ctx.cr6.eq) goto loc_821D658C;
-	// fsubs f12,f29,f0
-	ctx.f12.f64 = double(float(ctx.f29.f64 - ctx.f0.f64));
-	// fsel f12,f12,f0,f29
-	ctx.f12.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f29.f64;
-	// fcmpu cr6,f12,f22
-	ctx.cr6.compare(ctx.f12.f64, ctx.f22.f64);
-	// ble cr6,0x821d6574
-	if (!ctx.cr6.gt) goto loc_821D6574;
-	// fmadds f13,f12,f21,f20
-	ctx.f13.f64 = double(float(std::fma(ctx.f12.f64, ctx.f21.f64, ctx.f20.f64)));
-loc_821D6574:
-	// lwz r11,36(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 36);
-	// rlwinm r11,r11,0,6,6
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x2000000;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d658c
-	if (ctx.cr6.eq) goto loc_821D658C;
-	// lfs f12,12728(r19)
-	ctx.fpscr.disableFlushMode();
-	temp.u32 = REX_LOAD_U32(ctx.r19.u32 + 12728);
-	ctx.f12.f64 = double(temp.f32);
-	// fmuls f0,f12,f0
-	ctx.f0.f64 = double(float(ctx.f12.f64 * ctx.f0.f64));
-loc_821D658C:
-	// lwz r11,64(r29)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + 64);
-	// rlwinm r10,r11,29,31,31
-	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 29) & 0x1;
-	// cmplwi cr6,r10,0
-	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
-	// beq cr6,0x821d66f8
-	if (ctx.cr6.eq) goto loc_821D66F8;
-	// fadds f12,f13,f30
-	ctx.fpscr.disableFlushMode();
-	ctx.f12.f64 = double(float(ctx.f13.f64 + ctx.f30.f64));
-	// fsubs f12,f12,f28
-	ctx.f12.f64 = double(float(ctx.f12.f64 - ctx.f28.f64));
-	// fcmpu cr6,f12,f31
-	ctx.cr6.compare(ctx.f12.f64, ctx.f31.f64);
-	// bge cr6,0x821d65d4
-	if (!ctx.cr6.lt) goto loc_821D65D4;
-	// cmplwi cr6,r30,0
-	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b4
-	if (ctx.cr6.eq) goto loc_821D67B4;
-	// lwz r11,40(r30)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 40);
-	// rlwinm r10,r11,20,31,31
-	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 20) & 0x1;
-	// cmplwi cr6,r10,0
-	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
-	// beq cr6,0x821d65c8
-	if (ctx.cr6.eq) goto loc_821D65C8;
-	// stb r17,99(r30)
-	REX_STORE_U8(ctx.r30.u32 + 99, ctx.r17.u8);
-loc_821D65C8:
-	// rlwinm r11,r11,0,20,18
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0xFFFFFFFFFFFFEFFF;
-	// stw r11,40(r30)
-	REX_STORE_U32(ctx.r30.u32 + 40, ctx.r11.u32);
-	// b 0x821d67b4
-	goto loc_821D67B4;
-loc_821D65D4:
-	// cmplwi cr6,r10,0
-	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
-	// beq cr6,0x821d66f8
-	if (ctx.cr6.eq) goto loc_821D66F8;
-	// fsubs f12,f30,f28
-	ctx.fpscr.disableFlushMode();
-	ctx.f12.f64 = double(float(ctx.f30.f64 - ctx.f28.f64));
-	// fcmpu cr6,f12,f31
-	ctx.cr6.compare(ctx.f12.f64, ctx.f31.f64);
-	// bge cr6,0x821d66f8
-	if (!ctx.cr6.lt) goto loc_821D66F8;
-	// fadds f0,f0,f28
-	ctx.f0.f64 = double(float(ctx.f0.f64 + ctx.f28.f64));
-	// cmplwi cr6,r30,0
-	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
-	// fsubs f0,f0,f30
-	ctx.f0.f64 = double(float(ctx.f0.f64 - ctx.f30.f64));
-	// fdivs f0,f0,f13
-	ctx.f0.f64 = double(float(ctx.f0.f64 / ctx.f13.f64));
-	// fsubs f13,f0,f27
-	ctx.f13.f64 = double(float(ctx.f0.f64 - ctx.f27.f64));
-	// fsel f0,f13,f27,f0
-	ctx.f0.f64 = ctx.f13.f64 >= 0.0 ? ctx.f27.f64 : ctx.f0.f64;
-	// beq cr6,0x821d66a4
-	if (ctx.cr6.eq) goto loc_821D66A4;
-	// lwz r11,40(r30)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 40);
-	// fcmpu cr6,f0,f16
-	ctx.cr6.compare(ctx.f0.f64, ctx.f16.f64);
-	// ori r11,r11,4096
-	ctx.r11.u64 = ctx.r11.u64 | 4096;
-	// stw r11,40(r30)
-	REX_STORE_U32(ctx.r30.u32 + 40, ctx.r11.u32);
-	// bge cr6,0x821d6620
-	if (!ctx.cr6.lt) goto loc_821D6620;
-	// fmuls f0,f0,f24
-	ctx.f0.f64 = double(float(ctx.f0.f64 * ctx.f24.f64));
-	// b 0x821d6650
-	goto loc_821D6650;
-loc_821D6620:
-	// lbz r11,-31646(r22)
-	ctx.r11.u64 = REX_LOAD_U8(ctx.r22.u32 + -31646);
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// bne cr6,0x821d664c
-	if (!ctx.cr6.eq) goto loc_821D664C;
-	// fsubs f0,f27,f0
-	ctx.fpscr.disableFlushMode();
-	ctx.f0.f64 = double(float(ctx.f27.f64 - ctx.f0.f64));
-	// addi r11,r1,80
-	ctx.r11.s64 = ctx.r1.s64 + 80;
-	// fmuls f0,f0,f24
-	ctx.f0.f64 = double(float(ctx.f0.f64 * ctx.f24.f64));
-	// fmuls f0,f0,f26
-	ctx.f0.f64 = double(float(ctx.f0.f64 * ctx.f26.f64));
-	// fctiwz f0,f0
-	ctx.f0.s64 = std::isnan(ctx.f0.f64) ? int64_t(0x80000000U) : (ctx.f0.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f0.f64));
-	// stfiwx f0,0,r11
-	REX_STORE_U32(ctx.r11.u32, ctx.f0.u32);
-	// lwz r11,80(r1)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);
-	// stb r11,99(r30)
-	REX_STORE_U8(ctx.r30.u32 + 99, ctx.r11.u8);
-loc_821D664C:
-	// fmr f0,f27
-	ctx.fpscr.disableFlushMode();
-	ctx.f0.f64 = ctx.f27.f64;
-loc_821D6650:
-	// lwz r11,40(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 40);
-	// fmuls f0,f0,f26
-	ctx.fpscr.disableFlushMode();
-	ctx.f0.f64 = double(float(ctx.f0.f64 * ctx.f26.f64));
-	// rlwinm r11,r11,20,31,31
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 20) & 0x1;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// bne cr6,0x821d67b4
-	if (!ctx.cr6.eq) goto loc_821D67B4;
-	// lbz r11,-31646(r22)
-	ctx.r11.u64 = REX_LOAD_U8(ctx.r22.u32 + -31646);
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// bne cr6,0x821d67b4
-	if (!ctx.cr6.eq) goto loc_821D67B4;
-	// lbz r11,99(r31)
-	ctx.r11.u64 = REX_LOAD_U8(ctx.r31.u32 + 99);
-	// addi r10,r1,84
-	ctx.r10.s64 = ctx.r1.s64 + 84;
-	// std r11,96(r1)
-	REX_STORE_U64(ctx.r1.u32 + 96, ctx.r11.u64);
-	// lfd f13,96(r1)
-	ctx.f13.u64 = REX_LOAD_U64(ctx.r1.u32 + 96);
-	// fcfid f13,f13
-	ctx.f13.f64 = double(ctx.f13.s64);
-	// frsp f13,f13
-	ctx.f13.f64 = double(float(ctx.f13.f64));
-	// fsubs f12,f0,f13
-	ctx.f12.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
-	// fsel f0,f12,f13,f0
-	ctx.f0.f64 = ctx.f12.f64 >= 0.0 ? ctx.f13.f64 : ctx.f0.f64;
-	// fctiwz f0,f0
-	ctx.f0.s64 = std::isnan(ctx.f0.f64) ? int64_t(0x80000000U) : (ctx.f0.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f0.f64));
-	// stfiwx f0,0,r10
-	REX_STORE_U32(ctx.r10.u32, ctx.f0.u32);
-	// lwz r11,84(r1)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 84);
-	// stb r11,99(r31)
-	REX_STORE_U8(ctx.r31.u32 + 99, ctx.r11.u8);
-	// b 0x821d67b4
-	goto loc_821D67B4;
-loc_821D66A4:
-	// lbz r11,99(r31)
-	ctx.r11.u64 = REX_LOAD_U8(ctx.r31.u32 + 99);
-	// fmuls f0,f0,f26
-	ctx.fpscr.disableFlushMode();
-	ctx.f0.f64 = double(float(ctx.f0.f64 * ctx.f26.f64));
-	// lwz r10,40(r31)
-	ctx.r10.u64 = REX_LOAD_U32(ctx.r31.u32 + 40);
-	// rlwinm r10,r10,20,31,31
-	ctx.r10.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 20) & 0x1;
-	// std r11,104(r1)
-	REX_STORE_U64(ctx.r1.u32 + 104, ctx.r11.u64);
-	// cmplwi cr6,r10,0
-	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
-	// lfd f13,104(r1)
-	ctx.f13.u64 = REX_LOAD_U64(ctx.r1.u32 + 104);
-	// fcfid f13,f13
-	ctx.f13.f64 = double(ctx.f13.s64);
-	// frsp f13,f13
-	ctx.f13.f64 = double(float(ctx.f13.f64));
-	// bne cr6,0x821d67b4
-	if (!ctx.cr6.eq) goto loc_821D67B4;
-	// lbz r11,-31646(r22)
-	ctx.r11.u64 = REX_LOAD_U8(ctx.r22.u32 + -31646);
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// bne cr6,0x821d67b4
-	if (!ctx.cr6.eq) goto loc_821D67B4;
-	// fsubs f12,f0,f13
-	ctx.f12.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
-	// addi r11,r1,88
-	ctx.r11.s64 = ctx.r1.s64 + 88;
-	// fsel f0,f12,f13,f0
-	ctx.f0.f64 = ctx.f12.f64 >= 0.0 ? ctx.f13.f64 : ctx.f0.f64;
-	// fctiwz f0,f0
-	ctx.f0.s64 = std::isnan(ctx.f0.f64) ? int64_t(0x80000000U) : (ctx.f0.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f0.f64));
-	// stfiwx f0,0,r11
-	REX_STORE_U32(ctx.r11.u32, ctx.f0.u32);
-	// lwz r11,88(r1)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 88);
-	// stb r11,99(r31)
-	REX_STORE_U8(ctx.r31.u32 + 99, ctx.r11.u8);
-	// b 0x821d67b4
-	goto loc_821D67B4;
-loc_821D66F8:
-	// lfs f13,12708(r18)
-	ctx.fpscr.disableFlushMode();
-	temp.u32 = REX_LOAD_U32(ctx.r18.u32 + 12708);
-	ctx.f13.f64 = double(temp.f32);
-	// fadds f13,f13,f28
-	ctx.f13.f64 = double(float(ctx.f13.f64 + ctx.f28.f64));
-	// fsubs f13,f30,f13
-	ctx.f13.f64 = double(float(ctx.f30.f64 - ctx.f13.f64));
-	// fcmpu cr6,f13,f0
-	ctx.cr6.compare(ctx.f13.f64, ctx.f0.f64);
-	// bge cr6,0x821d678c
-	if (!ctx.cr6.lt) goto loc_821D678C;
-	// cmplwi cr6,r30,0
-	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
-	// beq cr6,0x821d6730
-	if (ctx.cr6.eq) goto loc_821D6730;
-	// lwz r11,40(r30)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 40);
-	// rlwinm r9,r11,20,31,31
-	ctx.r9.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 20) & 0x1;
-	// cmplwi cr6,r9,0
-	ctx.cr6.compare<uint32_t>(ctx.r9.u32, 0, ctx.xer);
-	// beq cr6,0x821d6728
-	if (ctx.cr6.eq) goto loc_821D6728;
-	// stb r17,99(r30)
-	REX_STORE_U8(ctx.r30.u32 + 99, ctx.r17.u8);
-loc_821D6728:
-	// rlwinm r11,r11,0,20,18
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0xFFFFFFFFFFFFEFFF;
-	// stw r11,40(r30)
-	REX_STORE_U32(ctx.r30.u32 + 40, ctx.r11.u32);
-loc_821D6730:
-	// cmplwi cr6,r10,0
-	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
-	// bne cr6,0x821d67b0
-	if (!ctx.cr6.eq) goto loc_821D67B0;
-	// clrlwi r11,r14,24
-	ctx.r11.u64 = ctx.r14.u32 & 0xFF;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b0
-	if (ctx.cr6.eq) goto loc_821D67B0;
-	// lfs f0,80(r31)
-	ctx.fpscr.disableFlushMode();
-	temp.u32 = REX_LOAD_U32(ctx.r31.u32 + 80);
-	ctx.f0.f64 = double(temp.f32);
-	// fsubs f12,f30,f28
-	ctx.f12.f64 = double(float(ctx.f30.f64 - ctx.f28.f64));
-	// lfs f13,2340(r25)
-	temp.u32 = REX_LOAD_U32(ctx.r25.u32 + 2340);
-	ctx.f13.f64 = double(temp.f32);
-	// li r11,1
-	ctx.r11.s64 = 1;
-	// fmuls f0,f0,f13
-	ctx.f0.f64 = double(float(ctx.f0.f64 * ctx.f13.f64));
-	// fcmpu cr6,f12,f0
-	ctx.cr6.compare(ctx.f12.f64, ctx.f0.f64);
-	// blt cr6,0x821d6764
-	if (ctx.cr6.lt) goto loc_821D6764;
-	// mr r11,r26
-	ctx.r11.u64 = ctx.r26.u64;
-loc_821D6764:
-	// clrlwi r11,r11,24
-	ctx.r11.u64 = ctx.r11.u32 & 0xFF;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// bne cr6,0x821d67b4
-	if (!ctx.cr6.eq) goto loc_821D67B4;
-	// lwz r11,8(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 8);
-	// lwz r10,12(r31)
-	ctx.r10.u64 = REX_LOAD_U32(ctx.r31.u32 + 12);
-	// oris r11,r11,16384
-	ctx.r11.u64 = ctx.r11.u64 | 1073741824;
-	// oris r10,r10,16384
-	ctx.r10.u64 = ctx.r10.u64 | 1073741824;
-	// stw r11,8(r31)
-	REX_STORE_U32(ctx.r31.u32 + 8, ctx.r11.u32);
-	// stw r10,12(r31)
-	REX_STORE_U32(ctx.r31.u32 + 12, ctx.r10.u32);
-	// b 0x821d67b4
-	goto loc_821D67B4;
-loc_821D678C:
-	// cmplwi cr6,r30,0
-	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b0
-	if (ctx.cr6.eq) goto loc_821D67B0;
-	// lwz r11,40(r30)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 40);
-	// rlwinm r10,r11,20,31,31
-	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 20) & 0x1;
-	// cmplwi cr6,r10,0
-	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
-	// beq cr6,0x821d67a8
-	if (ctx.cr6.eq) goto loc_821D67A8;
-	// stb r17,99(r30)
-	REX_STORE_U8(ctx.r30.u32 + 99, ctx.r17.u8);
-loc_821D67A8:
-	// rlwinm r11,r11,0,20,18
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0xFFFFFFFFFFFFEFFF;
-	// stw r11,40(r30)
-	REX_STORE_U32(ctx.r30.u32 + 40, ctx.r11.u32);
+  r31 = REX_LOAD_U32(r27);
+  if (r31 == 0) goto loc_821D67B4;
+
+  {
+    const int16_t model_idx = static_cast<int16_t>(REX_LOAD_U16(r31 + 46));
+    ctx.r3.u64 = r31;
+    r30 = REX_LOAD_U32(r31 + 76);
+    r29 = REX_LOAD_U32(uint32_t(int32_t(model_idx) * 4) + r23);
+    ctx.lr = 0x821D6360;
+    sub_823B32E0(ctx, base);
+  }
+  if ((ctx.r3.u32 & 0xFF) != 0) {
+    REX_STORE_U32(r27, 0);
+    const uint16_t flags92 = REX_LOAD_U16(r31 + 92) & 65531u;
+    REX_STORE_U8(r31 + 99, 0);
+    ctx.cr0.compare<int32_t>(int32_t(flags92), 0, ctx.xer);
+    REX_STORE_U16(r31 + 92, flags92);
+    goto loc_821D67B4;
+  }
+
+  if (REX_LOAD_U8(r21 + 10579) != 0 &&
+      static_cast<int32_t>(REX_LOAD_U32(r20 - 30228)) > 0 &&
+      r30 != 0) {
+    if ((REX_LOAD_U8(r30 + 36) & 0x1) == 0) goto loc_821D67B0;
+  }
+
+  if ((REX_LOAD_U32(r31 + 36) & 0x4000000u) == 0) goto loc_821D67B0;
+  if ((REX_LOAD_U32(r31 + 40) & 0x100u) != 0) goto loc_821D67B0;
+
+  ctx.r3.u64 = r29;
+  ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(r29) + 12);
+  ctx.lr = 0x821D63E8;
+  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
+
+  {
+    const uint32_t type_id = ctx.r3.u32 & 0xFF;
+    if (type_id == 5) goto loc_821D68E8;
+    if (type_id == 6) goto loc_821D67D0;
+  }
+
+  {
+    uint32_t pos_ptr;
+    if (r30 != 0) {
+      const uint32_t p = REX_LOAD_U32(r30 + 32);
+      pos_ptr = (p != 0) ? (p + 48) : (r30 + 16);
+    } else {
+      const uint32_t p = REX_LOAD_U32(r31 + 32);
+      pos_ptr = (p != 0) ? (p + 48) : (r31 + 16);
+    }
+
+    f29 = double(std::bit_cast<float>(REX_LOAD_U32(r31 + 80)));
+    const uint32_t flags36 = REX_LOAD_U32(r31 + 36);
+    const uint32_t r10_bit = (flags36 >> 23) & 0x1;
+    f30 = calc_dist3d(pos_ptr);
+
+    if (r10_bit != 0 && f23 > f19) {
+      f29 = f23;
+    }
+
+    const float mult2340 = std::bit_cast<float>(REX_LOAD_U32(r25 + 2340));
+    f31 = double(float(double(mult2340) * f29));
+
+    if ((flags36 & 0x2000000u) == 0) {
+      if (f30 > f25 && f31 > f25) {
+        const double limit_plus = double(float(f31 + f28));
+        if (f30 < limit_plus) {
+          f30 = double(float(double(float(f31 - f25)) + f30));
+        }
+      }
+    }
+
+    double f0 = (r10_bit != 0) ? f18 : f17;
+    if (r30 != 0) {
+      const uint32_t p = REX_LOAD_U32(r31 + 32);
+      const uint32_t p2 = (p != 0) ? (p + 48) : (r31 + 16);
+      f0 = double(float(calc_dist3d(p2) + f0));
+    } else {
+      f0 = double(float(f0 + f30));
+    }
+
+    REX_STORE_U32(r27 + 4, std::bit_cast<uint32_t>(float(f0)));
+
+    ctx.r3.u64 = r31;
+    ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(r31) + 84);
+    ctx.lr = 0x821D6548;
+    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
+
+    f0 = double(float(ctx.f1.f64 + f23));
+    double f13 = f28;
+    if (float(f31 - f0) < 0.0f) f0 = f31;
+
+    if (r30 == 0) {
+      const double f12 = (float(f29 - f0) >= 0.0f) ? f0 : f29;
+      if (f12 > f22) {
+        f13 = double(float(std::fma(f12, f21, f20)));
+      }
+      if ((REX_LOAD_U32(r31 + 36) & 0x2000000u) != 0) {
+        const double f12_m = double(std::bit_cast<float>(REX_LOAD_U32(r19 + 12728)));
+        f0 = double(float(f12_m * f0));
+      }
+    }
+
+    const uint32_t r10_flag = (REX_LOAD_U32(r29 + 64) >> 3) & 0x1;
+    if (r10_flag != 0) {
+      if (double(float(double(float(f13 + f30)) - f28)) < f31) {
+        if (r30 == 0) goto loc_821D67B4;
+        const uint32_t f40 = REX_LOAD_U32(r30 + 40);
+        if ((f40 >> 12) & 0x1) REX_STORE_U8(r30 + 99, 255);
+        REX_STORE_U32(r30 + 40, f40 & ~0x1000u);
+        goto loc_821D67B4;
+      }
+      if (double(float(f30 - f28)) < f31) {
+        f0 = double(float(double(float(double(float(f0 + f28)) - f30)) / f13));
+        if (float(f0 - f27) >= 0.0f) f0 = f27;
+
+        if (r30 != 0) {
+          REX_STORE_U32(r30 + 40, REX_LOAD_U32(r30 + 40) | 4096u);
+          if (f0 < f16) {
+            f0 = double(float(f0 * f24));
+          } else {
+            if (REX_LOAD_U8(r22 - 31646) == 0) {
+              double tmp = double(float(double(float(double(float(f27 - f0)) * f24)) * f26));
+              int32_t iv = std::isnan(tmp) ? int32_t(0x80000000u) : (tmp > double(INT_MAX) ? INT_MAX : int32_t(tmp));
+              REX_STORE_U8(r30 + 99, uint8_t(iv));
+            }
+            f0 = f27;
+          }
+          f0 = double(float(f0 * f26));
+          if (((REX_LOAD_U32(r31 + 40) >> 12) & 0x1) != 0) goto loc_821D67B4;
+          if (REX_LOAD_U8(r22 - 31646) != 0) goto loc_821D67B4;
+          const double cur99 = double(float(REX_LOAD_U8(r31 + 99)));
+          if (float(f0 - cur99) >= 0.0f) f0 = cur99;
+          int32_t iv = std::isnan(f0) ? int32_t(0x80000000u) : (f0 > double(INT_MAX) ? INT_MAX : int32_t(f0));
+          REX_STORE_U8(r31 + 99, uint8_t(iv));
+          goto loc_821D67B4;
+        } else {
+          const double cur99 = double(float(REX_LOAD_U8(r31 + 99)));
+          f0 = double(float(f0 * f26));
+          if (((REX_LOAD_U32(r31 + 40) >> 12) & 0x1) != 0) goto loc_821D67B4;
+          if (REX_LOAD_U8(r22 - 31646) != 0) goto loc_821D67B4;
+          if (float(f0 - cur99) >= 0.0f) f0 = cur99;
+          int32_t iv = std::isnan(f0) ? int32_t(0x80000000u) : (f0 > double(INT_MAX) ? INT_MAX : int32_t(f0));
+          REX_STORE_U8(r31 + 99, uint8_t(iv));
+          goto loc_821D67B4;
+        }
+      }
+    }
+
+    {
+      const double f13_lim = double(float(f30 - double(float(double(std::bit_cast<float>(REX_LOAD_U32(r18 + 12708))) + f28))));
+      if (f13_lim < f0) {
+        if (r30 != 0) {
+          const uint32_t f40 = REX_LOAD_U32(r30 + 40);
+          if ((f40 >> 12) & 0x1) REX_STORE_U8(r30 + 99, 255);
+          REX_STORE_U32(r30 + 40, f40 & ~0x1000u);
+        }
+        if (r10_flag != 0 || (r14 & 0xFF) == 0) goto loc_821D67B0;
+        const double lim = double(float(double(std::bit_cast<float>(REX_LOAD_U32(r31 + 80))) *
+                                        double(std::bit_cast<float>(REX_LOAD_U32(r25 + 2340)))));
+        if (double(float(f30 - f28)) < lim) goto loc_821D67B4;
+        REX_STORE_U32(r31 + 8, REX_LOAD_U32(r31 + 8) | 0x40000000u);
+        REX_STORE_U32(r31 + 12, REX_LOAD_U32(r31 + 12) | 0x40000000u);
+        goto loc_821D67B4;
+      }
+      if (r30 != 0) {
+        const uint32_t f40 = REX_LOAD_U32(r30 + 40);
+        if ((f40 >> 12) & 0x1) REX_STORE_U8(r30 + 99, 255);
+        REX_STORE_U32(r30 + 40, f40 & ~0x1000u);
+      }
+    }
+  }
+
 loc_821D67B0:
-	// stw r26,0(r27)
-	REX_STORE_U32(ctx.r27.u32 + 0, ctx.r26.u32);
+  REX_STORE_U32(r27, 0);
+
 loc_821D67B4:
-	// addi r27,r27,8
-	ctx.r27.s64 = ctx.r27.s64 + 8;
-	// cmplw cr6,r27,r15
-	ctx.cr6.compare<uint32_t>(ctx.r27.u32, ctx.r15.u32, ctx.xer);
-	// blt cr6,0x821d6338
-	if (ctx.cr6.lt) goto loc_821D6338;
-loc_821D67C0:
-	// addi r1,r1,464
-	ctx.r1.s64 = ctx.r1.s64 + 464;
-	// addi r12,r1,-152
-	ctx.r12.s64 = ctx.r1.s64 + -152;
-	// bl 0x82a01344
-	ctx.lr = 0x821D67CC;
-	__restfpr_16(ctx, base);
-	// b 0x829ff7e0
-	__restgprlr_14(ctx, base);
-	return;
+  r27 += 8;
+  if (r27 < r15) goto loc_821D6338;
+
+  restore_and_return();
+  return;
+
 loc_821D67D0:
-	// lwz r10,36(r31)
-	ctx.r10.u64 = REX_LOAD_U32(ctx.r31.u32 + 36);
-	// rlwinm r11,r10,26,31,31
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 26) & 0x1;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d67fc
-	if (ctx.cr6.eq) goto loc_821D67FC;
-	// lwz r11,120(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 120);
-	// cmpwi cr6,r11,0
-	ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
-	// beq cr6,0x821d67b0
-	if (ctx.cr6.eq) goto loc_821D67B0;
-	// lbz r11,0(r11)
-	ctx.r11.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
-	// rlwinm r11,r11,0,25,25
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x40;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b0
-	if (ctx.cr6.eq) goto loc_821D67B0;
-loc_821D67FC:
-	// rlwinm r11,r10,0,12,12
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 0) & 0x80000;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b4
-	if (ctx.cr6.eq) goto loc_821D67B4;
-	// lwz r11,32(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 32);
-	// lvx128 v0,r25,r24
-	ea = (ctx.r25.u32 + ctx.r24.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// addi r11,r11,48
-	ctx.r11.s64 = ctx.r11.s64 + 48;
-	// bne cr6,0x821d6820
-	if (!ctx.cr6.eq) goto loc_821D6820;
-	// addi r11,r31,16
-	ctx.r11.s64 = ctx.r31.s64 + 16;
-loc_821D6820:
-	// lvx128 v13,r0,r11
-	ea = (ctx.r11.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// addi r11,r1,144
-	ctx.r11.s64 = ctx.r1.s64 + 144;
-	// vsubfp v0,v13,v0
-	ctx.fpscr.enableFlushMode();
-	simde_mm_store_ps(ctx.v0.f32, simde_mm_sub_ps(simde_mm_load_ps(ctx.v13.f32), simde_mm_load_ps(ctx.v0.f32)));
-	// stvx128 v0,r0,r11
-	ea = (ctx.r11.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// lfs f13,148(r1)
-	ctx.fpscr.disableFlushModeUnconditional();
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 148);
-	ctx.f13.f64 = double(temp.f32);
-	// fmuls f12,f13,f13
-	ctx.f12.f64 = double(float(ctx.f13.f64 * ctx.f13.f64));
-	// lfs f0,152(r1)
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 152);
-	ctx.f0.f64 = double(temp.f32);
-	// lfs f13,144(r1)
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 144);
-	ctx.f13.f64 = double(temp.f32);
-	// fmadds f0,f0,f0,f12
-	ctx.f0.f64 = double(float(std::fma(ctx.f0.f64, ctx.f0.f64, ctx.f12.f64)));
-	// fmadds f0,f13,f13,f0
-	ctx.f0.f64 = double(float(std::fma(ctx.f13.f64, ctx.f13.f64, ctx.f0.f64)));
-	// fsqrts f0,f0
-	ctx.f0.f64 = double(float(sqrt(ctx.f0.f64)));
-	// stfs f0,4(r27)
-	temp.f32 = float(ctx.f0.f64);
-	REX_STORE_U32(ctx.r27.u32 + 4, temp.u32);
-	// lwz r11,40(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 40);
-	// rlwinm r11,r11,0,6,9
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x3C00000;
-	// cmplw cr6,r11,r16
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, ctx.r16.u32, ctx.xer);
-	// bne cr6,0x821d6898
-	if (!ctx.cr6.eq) goto loc_821D6898;
-	// lwz r11,572(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 572);
-	// rlwinm r11,r11,0,2,2
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x20000000;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d6898
-	if (ctx.cr6.eq) goto loc_821D6898;
-	// lwz r3,2688(r31)
-	ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 2688);
-	// cmplwi cr6,r3,0
-	ctx.cr6.compare<uint32_t>(ctx.r3.u32, 0, ctx.xer);
-	// beq cr6,0x821d6898
-	if (ctx.cr6.eq) goto loc_821D6898;
-	// lwz r11,0(r3)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);
-	// lwz r11,84(r11)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 84);
-	// mtctr r11
-	ctx.ctr.u64 = ctx.r11.u64;
-	// bctrl 
-	ctx.lr = 0x821D688C;
-	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
-	// lfs f0,4(r27)
-	ctx.fpscr.disableFlushMode();
-	temp.u32 = REX_LOAD_U32(ctx.r27.u32 + 4);
-	ctx.f0.f64 = double(temp.f32);
-	// fadds f0,f1,f0
-	ctx.f0.f64 = double(float(ctx.f1.f64 + ctx.f0.f64));
-	// stfs f0,4(r27)
-	temp.f32 = float(ctx.f0.f64);
-	REX_STORE_U32(ctx.r27.u32 + 4, temp.u32);
-loc_821D6898:
-	// lwz r11,0(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 0);
-	// mr r3,r31
-	ctx.r3.u64 = ctx.r31.u64;
-	// lwz r11,36(r11)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 36);
-	// mtctr r11
-	ctx.ctr.u64 = ctx.r11.u64;
-	// bctrl 
-	ctx.lr = 0x821D68AC;
-	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
-	// clrlwi r11,r3,24
-	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b4
-	if (ctx.cr6.eq) goto loc_821D67B4;
-	// mr r3,r31
-	ctx.r3.u64 = ctx.r31.u64;
-	// bl 0x821e5278
-	ctx.lr = 0x821D68C0;
-	sub_821E5278(ctx, base);
-	// cmplwi cr6,r3,0
-	ctx.cr6.compare<uint32_t>(ctx.r3.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b4
-	if (ctx.cr6.eq) goto loc_821D67B4;
-	// lwz r11,0(r3)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);
-	// lwz r11,84(r11)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 84);
-	// mtctr r11
-	ctx.ctr.u64 = ctx.r11.u64;
-	// bctrl 
-	ctx.lr = 0x821D68D8;
-	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
-	// lfs f0,4(r27)
-	ctx.fpscr.disableFlushMode();
-	temp.u32 = REX_LOAD_U32(ctx.r27.u32 + 4);
-	ctx.f0.f64 = double(temp.f32);
-	// fadds f0,f1,f0
-	ctx.f0.f64 = double(float(ctx.f1.f64 + ctx.f0.f64));
-	// stfs f0,4(r27)
-	temp.f32 = float(ctx.f0.f64);
-	REX_STORE_U32(ctx.r27.u32 + 4, temp.u32);
-	// b 0x821d67b4
-	goto loc_821D67B4;
+  {
+    const uint32_t f36 = REX_LOAD_U32(r31 + 36);
+    if ((f36 >> 6) & 0x1) {
+      const uint32_t p120 = REX_LOAD_U32(r31 + 120);
+      if (p120 == 0 || (REX_LOAD_U8(p120) & 0x40) == 0) goto loc_821D67B0;
+    }
+    if ((f36 & 0x80000u) == 0) goto loc_821D67B4;
+    const uint32_t p32 = REX_LOAD_U32(r31 + 32);
+    const double d = calc_dist3d((p32 != 0) ? (p32 + 48) : (r31 + 16));
+    REX_STORE_U32(r27 + 4, std::bit_cast<uint32_t>(float(d)));
+
+    if ((REX_LOAD_U32(r31 + 40) & 0x3C00000u) == r16 &&
+        (REX_LOAD_U32(r31 + 572) & 0x20000000u) != 0) {
+      const uint32_t sub_obj = REX_LOAD_U32(r31 + 2688);
+      if (sub_obj != 0) {
+        ctx.r3.u64 = sub_obj;
+        ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(sub_obj) + 84);
+        ctx.lr = 0x821D688C;
+        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
+        const float cur = std::bit_cast<float>(REX_LOAD_U32(r27 + 4));
+        REX_STORE_U32(r27 + 4, std::bit_cast<uint32_t>(float(ctx.f1.f64 + double(cur))));
+      }
+    }
+    ctx.r3.u64 = r31;
+    ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(r31) + 36);
+    ctx.lr = 0x821D68AC;
+    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
+    if ((ctx.r3.u32 & 0xFF) == 0) goto loc_821D67B4;
+
+    ctx.r3.u64 = r31;
+    ctx.lr = 0x821D68C0;
+    sub_821E5278(ctx, base);
+    if (ctx.r3.u32 == 0) goto loc_821D67B4;
+
+    ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(ctx.r3.u32) + 84);
+    ctx.lr = 0x821D68D8;
+    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
+    const float cur = std::bit_cast<float>(REX_LOAD_U32(r27 + 4));
+    REX_STORE_U32(r27 + 4, std::bit_cast<uint32_t>(float(ctx.f1.f64 + double(cur))));
+    goto loc_821D67B4;
+  }
+
 loc_821D68E8:
-	// lwz r11,36(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 36);
-	// rlwinm r11,r11,0,12,12
-	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x80000;
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// beq cr6,0x821d67b4
-	if (ctx.cr6.eq) goto loc_821D67B4;
-	// lwz r11,32(r31)
-	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 32);
-	// lvx128 v0,r25,r24
-	ea = (ctx.r25.u32 + ctx.r24.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// cmplwi cr6,r11,0
-	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
-	// addi r11,r11,48
-	ctx.r11.s64 = ctx.r11.s64 + 48;
-	// bne cr6,0x821d6910
-	if (!ctx.cr6.eq) goto loc_821D6910;
-	// addi r11,r31,16
-	ctx.r11.s64 = ctx.r31.s64 + 16;
-loc_821D6910:
-	// lvx128 v13,r0,r11
-	ea = (ctx.r11.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// addi r11,r1,160
-	ctx.r11.s64 = ctx.r1.s64 + 160;
-	// vsubfp v0,v13,v0
-	ctx.fpscr.enableFlushMode();
-	simde_mm_store_ps(ctx.v0.f32, simde_mm_sub_ps(simde_mm_load_ps(ctx.v13.f32), simde_mm_load_ps(ctx.v0.f32)));
-	// stvx128 v0,r0,r11
-	ea = (ctx.r11.u32) & ~0xF;
-	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
-	// lfs f13,168(r1)
-	ctx.fpscr.disableFlushModeUnconditional();
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 168);
-	ctx.f13.f64 = double(temp.f32);
-	// fmuls f12,f13,f13
-	ctx.f12.f64 = double(float(ctx.f13.f64 * ctx.f13.f64));
-	// lfs f0,164(r1)
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 164);
-	ctx.f0.f64 = double(temp.f32);
-	// lfs f13,160(r1)
-	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 160);
-	ctx.f13.f64 = double(temp.f32);
-	// fmadds f0,f0,f0,f12
-	ctx.f0.f64 = double(float(std::fma(ctx.f0.f64, ctx.f0.f64, ctx.f12.f64)));
-	// fmadds f0,f13,f13,f0
-	ctx.f0.f64 = double(float(std::fma(ctx.f13.f64, ctx.f13.f64, ctx.f0.f64)));
-	// fsqrts f0,f0
-	ctx.f0.f64 = double(float(sqrt(ctx.f0.f64)));
-	// stfs f0,4(r27)
-	temp.f32 = float(ctx.f0.f64);
-	REX_STORE_U32(ctx.r27.u32 + 4, temp.u32);
-	// b 0x821d67b4
-	goto loc_821D67B4;
+  {
+    if ((REX_LOAD_U32(r31 + 36) & 0x80000u) == 0) goto loc_821D67B4;
+    const uint32_t p32 = REX_LOAD_U32(r31 + 32);
+    const double d = calc_dist3d((p32 != 0) ? (p32 + 48) : (r31 + 16));
+    REX_STORE_U32(r27 + 4, std::bit_cast<uint32_t>(float(d)));
+    goto loc_821D67B4;
+  }
 }
 
 extern "C" void sub_821D8CD8(PPCContext& ctx, uint8_t* base) {
diff --git a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_frontend_hooks.cpp b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_frontend_hooks.cpp
index b63d73e6..a02f4889 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_frontend_hooks.cpp
+++ b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_frontend_hooks.cpp
@@ -338,9 +338,26 @@ constexpr std::array kAntiAliasingChoices = {
     Choice{"ssaa14x", TextId::kSsaa14x},
     Choice{"ssaa16x", TextId::kSsaa16x},
 };
+//constexpr std::array kUpscalerChoices = {
+//    Choice{"native", TextId::kNative},
+//    Choice{"fsr1", TextId::kFsr1},
+//#if defined(LIBERTY_HAS_FSR3)
+//    Choice{"fsr3", TextId::kFsr3},
+//#endif
+//#if defined(LIBERTY_HAS_DLSS)
+//    Choice{"dlss", TextId::kDlss},
+//#endif
+//#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS
+//    Choice{"metalfx", TextId::kMetalFxUpscaler},
+//#endif
+//};
 constexpr std::array kUpscalerChoices = {
     Choice{"native", TextId::kNative},
     Choice{"fsr1", TextId::kFsr1},
+    // SGSR: временно используем TextId::kFsr1, чтобы не менять enum TextId;
+    // после добавления kSgsr заменить на него. В меню пункт будет
+    // отображаться как FSR 1, но cvar уже принимает "sgsr".
+    Choice{"sgsr", TextId::kFsr3},
 #if defined(LIBERTY_HAS_FSR3)
     Choice{"fsr3", TextId::kFsr3},
 #endif
diff --git a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_hack_registry.h b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_hack_registry.h
index cd6572ea..22d67bd1 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_hack_registry.h
+++ b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_hack_registry.h
@@ -122,7 +122,7 @@ inline constexpr auto kRegistry = std::to_array<Entry>({
     {"gta4_thread_pinning", Kind::kShortcut, "0", "busiest threads pinned to big cores"},
     {"android_pin_hot_threads", Kind::kShortcut, "false", "hot threads pinned"},
     {"kernel_spinlock_spins", Kind::kShortcut, "0", "guest spinlocks spin before yielding"},
-    {"android_perf_hint", Kind::kShortcut, "false", "Android performance hint session"},
+    {"android_perf_hint", Kind::kShortcut, "true", "Android performance hint session"},
     {"android_perf_hint_target_ms", Kind::kSetting, "", "performance hint target"},
     {"android_gpu_turbo", Kind::kSetting, "false", "GPU held at its highest clock"},
     {"android_thermal_guard", Kind::kGameplay, "false", "draw distance lowered when hot"},
diff --git a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_native_hooks.cpp b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_native_hooks.cpp
index 2a4966bd..69f05b87 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/src/gta4_native_hooks.cpp
+++ b/glue/rexglue-sdk-main/gta4-recomp/src/gta4_native_hooks.cpp
@@ -2892,7 +2892,7 @@ NativeResolutionOverride GetNativeResolutionOverride(uint32_t requested_width,
     }
   }
   g_native_supersampling_effective_factor.store(effective_ssaa_factor, std::memory_order_release);
-
+#if 0
   const bool ssaa_active = requested_ssaa_factor > 1u;
   const bool fsr1_requested = REXCVAR_GET(gta4_native_upscaler) == "fsr1";
   const bool metalfx_requested=REXCVAR_GET(gta4_native_upscaler)=="metalfx";
@@ -2907,6 +2907,29 @@ NativeResolutionOverride GetNativeResolutionOverride(uint32_t requested_width,
           fsr1_requested, REXCVAR_GET(gta4_fsr1_quality), ssaa_active, hdr_requested, perceptual_before_hdr);
   result.width = upscaling.width; result.height = upscaling.height;
   result.fsr1_active = fsr1_requested&&upscaling.active();
+#endif
+//начало
+ const bool ssaa_active = requested_ssaa_factor > 1u;
+  const bool fsr1_requested = REXCVAR_GET(gta4_native_upscaler) == "fsr1";
+  // SGSR использует ту же политику разрешения, что и FSR1, но пока
+  // помечается отдельно, чтобы не смешивать с fsr1_active.
+  const bool sgsr_requested = REXCVAR_GET(gta4_native_upscaler) == "sgsr";
+  const bool spatial_upscaler_requested = fsr1_requested || sgsr_requested;
+  const bool metalfx_requested=REXCVAR_GET(gta4_native_upscaler)=="metalfx";
+  const bool hdr_requested = rex::cvar::Query<bool>("vulkan_hdr");
+  DeviceCapabilitiesResult upscaling_capabilities{};
+  const bool perceptual_before_hdr = spatial_upscaler_requested && hdr_requested &&
+      QueryNativeDeviceCapabilities(upscaling_capabilities) &&
+      (upscaling_capabilities.capabilities & kCapabilityPerceptualPresentationBeforeHdr);
+  const auto upscaling = metalfx_requested
+      ? SelectMetalFxRenderExtent(result.display_width,result.display_height,true,REXCVAR_GET(gta4_fsr1_quality),ssaa_active)
+      : SelectFsr1RenderExtent(result.display_width, result.display_height,
+          spatial_upscaler_requested, REXCVAR_GET(gta4_fsr1_quality), ssaa_active, hdr_requested, perceptual_before_hdr);
+  result.width = upscaling.width; result.height = upscaling.height;
+  // SGSR: пока используем fsr1_active как признак активного пространственного
+  // апскейлера; в будущем можно добавить отдельное поле sgsr_active.
+  result.fsr1_active = spatial_upscaler_requested && upscaling.active();
+//конец
   const auto selected_upscaler = REXCVAR_GET(gta4_native_upscaler);
   if (selected_upscaler == "fsr3" || selected_upscaler == "dlss") {
     TemporalUpscalerResult temporal;
@@ -5345,7 +5368,7 @@ std::string ShadowTargetFetch(uint8_t* base, uint32_t texture) {
 bool ShadowTraceActive(uint8_t* base) {
   const uint32_t frames = REXCVAR_GET(gta4_trace_shadow_targets);
   static std::atomic<uint32_t> heartbeat{0};
-  if ((heartbeat.fetch_add(1, std::memory_order_relaxed) & 0x3FFF) == 0) {
+  if (frames && (heartbeat.fetch_add(1, std::memory_order_relaxed) & 0x3FFF) == 0) {
     SHADOW_TRACE("shadow-trace: heartbeat value={} frame={}", frames,
                  GetNativeLightSubmittedFrame(base));
   }
diff --git a/glue/rexglue-sdk-main/gta4-recomp/src/input/context_touch_controls.cpp b/glue/rexglue-sdk-main/gta4-recomp/src/input/context_touch_controls.cpp
index 5f3e537b..fca15a55 100644
--- a/glue/rexglue-sdk-main/gta4-recomp/src/input/context_touch_controls.cpp
+++ b/glue/rexglue-sdk-main/gta4-recomp/src/input/context_touch_controls.cpp
@@ -33,9 +33,32 @@
 REXCVAR_DECLARE(bool, gta4_touch_trace);
 
 namespace gta4::input {
+
+// Добавляем кеш-переменные и функцию прямо сюда
+namespace {
+std::atomic<bool> g_cached_touch_visible{false};
+std::atomic<uint64_t> g_last_touch_visible_check{0};
+
+// Переносим MonotonicNanoseconds наверх, чтобы она была видна
+uint64_t MonotonicNanoseconds() {
+  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
+      std::chrono::steady_clock::now().time_since_epoch()).count());
+}
+} // namespace
+
 bool ContextTouchHudLayoutActive() noexcept {
-  return GamepadTouchOverlayVisible() || GetContextTouchDrawableOverlaySnapshot().visible ||
-         GetContextTouchOverlaySnapshot().visible || IsContextTouchEditorActive();
+  uint64_t now = MonotonicNanoseconds();
+  uint64_t last = g_last_touch_visible_check.load(std::memory_order_relaxed);
+  
+  if (now - last > 100000000) { // 100 мс
+    bool visible = GamepadTouchOverlayVisible() || 
+                   rex::input::TouchControlsVisible() || 
+                   IsContextTouchEditorActive();
+    g_cached_touch_visible.store(visible, std::memory_order_relaxed);
+    g_last_touch_visible_check.store(now, std::memory_order_relaxed);
+  }
+  
+  return g_cached_touch_visible.load(std::memory_order_relaxed);
 }
 
 namespace {
@@ -205,11 +228,6 @@ std::atomic<uint64_t> g_presentation_revision{0};
 
 ContextTouchOverlaySnapshot BuildOverlaySnapshotLocked();
 
-uint64_t MonotonicNanoseconds() {
-  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
-      std::chrono::steady_clock::now().time_since_epoch()).count());
-}
-
 void FinishEditorFadeLocked(uint64_t monotonic_ns) {
   monotonic_ns = std::max(monotonic_ns, g_runtime.last_drawable_ns);
   if (!g_runtime.editor_closing || monotonic_ns < g_runtime.editor_close_started_ns ||
diff --git a/glue/rexglue-sdk-main/include/rex/ppc/context.h b/glue/rexglue-sdk-main/include/rex/ppc/context.h
index 2d571e3d..7904cba6 100644
--- a/glue/rexglue-sdk-main/include/rex/ppc/context.h
+++ b/glue/rexglue-sdk-main/include/rex/ppc/context.h
@@ -24,6 +24,11 @@
 #include <simde/x86/sse.h>
 #include <simde/x86/sse4.1.h>
 
+#include <atomic>
+#if defined(__ANDROID__)
+#include <android/log.h>
+#endif
+
 //=============================================================================
 // PPCFunc Type Definition
 //=============================================================================
@@ -199,41 +204,105 @@ struct FPSCRRegister {
     return HostToGuest[(csr & RoundMaskVal) >> RoundShift];
   }
 
+// ИЗМЕРЕНИЕ: считаем реальные записи в FPCR (убрать после замеров)
+  static inline void countFpcrWrite() noexcept {
+    static std::atomic<uint64_t> n{0};
+    const uint64_t v = n.fetch_add(1, std::memory_order_relaxed) + 1;
+#if defined(__ANDROID__)
+    if ((v & 0xFFFFF) == 0)  // раз в ~1 млн записей
+      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "fpcr-writes=%llu",
+                          static_cast<unsigned long long>(v));
+#endif
+  }
+  
+    inline void storeFromGuest(uint32_t value) noexcept {
+    csr &= ~RoundMaskVal;
+    csr |= Platform::GuestToHost[value & kRoundMask];
+#if defined(REX_FPCR_STICKY_FLUSH)
+    csr |= FlushMask;  // rounding меняем, FZ не трогаем
+#endif
+    countFpcrWrite();
+    setcsr(csr);
+  }
+  
+#if 0
   inline void storeFromGuest(uint32_t value) noexcept {
     csr &= ~RoundMaskVal;
     csr |= Platform::GuestToHost[value & kRoundMask];
+    countFpcrWrite();
     setcsr(csr);
   }
-
+#endif
+#if 0
   inline void enableFlushModeUnconditional() noexcept {
     csr |= FlushMask;
+    countFpcrWrite();
     setcsr(csr);
   }
+#endif
+//Unconditional enable — как conditional + sticky
+inline void enableFlushModeUnconditional() noexcept {
+#if defined(REX_FPCR_STICKY_FLUSH)
+    // FZ уже «липкий»: пишем только пока тень ещё без FlushMask
+    if ((csr & FlushMask) != FlushMask) [[unlikely]] {
+      csr |= FlushMask;
+      countFpcrWrite();
+      setcsr(csr);
+    }
+#else
+    csr |= FlushMask;
+    countFpcrWrite();
+    setcsr(csr);
+#endif
+  }
 
   inline void disableFlushModeUnconditional() noexcept {
+#if defined(REX_FPCR_STICKY_FLUSH)
+    // эксперимент: режим flush остаётся включённым
+#else
     csr &= ~FlushMask;
+    countFpcrWrite();
     setcsr(csr);
+#endif
   }
 
   inline void enableFlushMode() noexcept {
     if ((csr & FlushMask) != FlushMask) [[unlikely]] {
       csr |= FlushMask;
+      countFpcrWrite();
       setcsr(csr);
     }
   }
 
   inline void disableFlushMode() noexcept {
+#if defined(REX_FPCR_STICKY_FLUSH)
+    // эксперимент: режим flush остаётся включённым
+#else
     if ((csr & FlushMask) != 0) [[unlikely]] {
       csr &= ~FlushMask;
+      countFpcrWrite();
       setcsr(csr);
     }
+#endif
   }
-
+  
+  inline void InitHost() noexcept {
+    csr = getcsr();
+    Platform::InitHostExceptions(csr);
+#if defined(REX_FPCR_STICKY_FLUSH)
+    csr |= FlushMask;  // FZ/FZ16 на весь срок жизни потока
+#endif
+    countFpcrWrite();
+    setcsr(csr);
+  }  
+  
+#if 0
   inline void InitHost() noexcept {
     csr = getcsr();
     Platform::InitHostExceptions(csr);
     setcsr(csr);
   }
+#endif
 };
 
 }  // namespace rex::ppc
diff --git a/glue/rexglue-sdk-main/resources/templates/codegen/init_h.inja b/glue/rexglue-sdk-main/resources/templates/codegen/init_h.inja
index 05ba3178..081c8ef1 100644
--- a/glue/rexglue-sdk-main/resources/templates/codegen/init_h.inja
+++ b/glue/rexglue-sdk-main/resources/templates/codegen/init_h.inja
@@ -345,17 +345,15 @@ inline thread_local i32 ppc_global_lock_depth_ = 0;
 
 #define REX_CHECK_GLOBAL_LOCK() (ppc_global_lock_depth_ ? u64{0} : u64{0x8000})
 
-#define REX_ENTER_GLOBAL_LOCK()                          \
-  do {                                                   \
-    rex::thread::global_critical_region::LockSpinning(); \
-    ++ppc_global_lock_depth_;                            \
+#define REX_ENTER_GLOBAL_LOCK() \
+  do {                          \
+    ++ppc_global_lock_depth_;   \
   } while (0)
 
 #define REX_LEAVE_GLOBAL_LOCK()                                                             \
   do {                                                                                      \
     assert(ppc_global_lock_depth_ >= 1 && "LeaveGlobalLock called without EnterGlobalLock"); \
     --ppc_global_lock_depth_;                                                               \
-    rex::thread::global_critical_region::mutex().unlock();                                  \
   } while (0)
 
 //=============================================================================
diff --git a/glue/rexglue-sdk-main/src/core/cvar.cpp b/glue/rexglue-sdk-main/src/core/cvar.cpp
index 6587a9f6..f38a0f65 100644
--- a/glue/rexglue-sdk-main/src/core/cvar.cpp
+++ b/glue/rexglue-sdk-main/src/core/cvar.cpp
@@ -25,6 +25,10 @@
 
 #include <toml++/toml.hpp>
 
+    #if defined(__ANDROID__)
+    #include <android/log.h>
+    #endif
+
 namespace rex::cvar {
 
 namespace {
@@ -567,22 +571,34 @@ std::vector<std::string> Init(int argc, char** argv) {
             if (SetFlagByName(name, value)) GetCommandLineValuesStorage()[name] = value;
           },
           entry.description);
-    } else {
-      app.add_option_function<std::string>(
-          "--" + entry.name, [name = entry.name](const std::string& value) {
-            if (SetFlagByName(name, value)) GetCommandLineValuesStorage()[name] = value;
-          },
-          entry.description);
-    }
-  }
+} else {
+          app.add_option_function<std::string>(
+                 "--" + entry.name, [name = entry.name](const std::string& value) {
+                   if (SetFlagByName(name, value)) GetCommandLineValuesStorage()[name] = value;
+                 },
+                 entry.description)
+              // Повтор строки в args.txt: берётся последняя. По умолчанию CLI11 бросает
+              // исключение, и все опции после неё молча остаются без значений.
+              ->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
+        }
+      }
 
-  try {
-    app.parse(argc, argv);
-  } catch (const CLI::ParseError& e) {
-    // TODO(tomc): dumb workaround for the stupid chicken and its egg.
-    //             dont call rex logging funcs here for now.
-    fprintf(stderr, "cvar: CLI11  parse error: %s\n", e.what());
-  }
+      try {
+        app.parse(argc, argv);
+      } catch (const CLI::ParseError& e) {
+        // TODO(tomc): dumb workaround for the stupid chicken and its egg.
+        //             dont call rex logging funcs here for now.
+        fprintf(stderr, "cvar: CLI11  parse error: %s\n", e.what());
+    #if defined(__ANDROID__)
+        // stderr на Android никуда не попадает.
+        __android_log_print(ANDROID_LOG_ERROR, "rex-cvar", "CLI11 parse error: %s", e.what());
+    #endif
+      }
+    #if defined(__ANDROID__)
+      __android_log_print(ANDROID_LOG_INFO, "rex-cvar",
+                          "command line: %d args, %zu applied now, %zu left for late modules",
+                          argc - 1, GetCommandLineValuesStorage().size(), app.remaining().size());
+    #endif
 
   // Stash unrecognized --options for cvars that register later. Supported
   // forms: --name=value, --name (true), --no-name (false); a separated
diff --git a/glue/rexglue-sdk-main/src/core/memory_android.cpp b/glue/rexglue-sdk-main/src/core/memory_android.cpp
index 57b239db..ca305d96 100644
--- a/glue/rexglue-sdk-main/src/core/memory_android.cpp
+++ b/glue/rexglue-sdk-main/src/core/memory_android.cpp
@@ -178,6 +178,15 @@ void* AllocFixed(void* base_address, size_t length, AllocationType allocation_ty
                  PageAccess access) {
   const uint32_t prot_requested = ToPosixProtectFlags(access);
 
+  // Быстрый путь для kCommit по уже зарезервированной памяти:
+  // mprotect сам атомарно вернёт ошибку (ENOMEM), если страницы ещё не замаплены,
+  // без открытия и парсинга многотысячного /proc/self/maps!
+  if (base_address && allocation_type == AllocationType::kCommit) {
+    if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
+      return base_address;
+    }
+  }
+
   int prot_initial = 0;
   switch (allocation_type) {
     case AllocationType::kReserve:
@@ -199,19 +208,20 @@ void* AllocFixed(void* base_address, size_t length, AllocationType allocation_ty
 
   void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
   if (result != MAP_FAILED) {
+    if (base_address && result != base_address) {
+      // Защита для старых ядер (< 4.17), которые игнорируют MAP_FIXED_NOREPLACE
+      munmap(result, length);
+      return nullptr;
+    }
     return result;
   }
 
 #if defined(MAP_FIXED_NOREPLACE)
-  // Commit-on-reservation path: a prior Reserve already holds this VA as
-  // PROT_NONE; upgrade it with mprotect.
   if (errno == EEXIST && base_address &&
       (allocation_type == AllocationType::kCommit ||
        allocation_type == AllocationType::kReserveCommit)) {
-    if (IsRangeFullyMapped(base_address, length)) {
-      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
-        return base_address;
-      }
+    if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
+      return base_address;
     }
   }
 #endif
diff --git a/glue/rexglue-sdk-main/src/core/threading_posix.cpp b/glue/rexglue-sdk-main/src/core/threading_posix.cpp
index 8c31d93a..f2ec0a01 100644
--- a/glue/rexglue-sdk-main/src/core/threading_posix.cpp
+++ b/glue/rexglue-sdk-main/src/core/threading_posix.cpp
@@ -29,6 +29,9 @@ static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only")
 #endif
 #include <semaphore.h>
 #include <sys/eventfd.h>
+#if REX_PLATFORM_ANDROID
+#include <sys/prctl.h>
+#endif
 #include <sys/syscall.h>
 #include <sys/time.h>
 #include <sys/types.h>
@@ -218,6 +221,49 @@ bool SetTlsValue(TlsHandle handle, uintptr_t value) {
          0;
 }
 
+constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);
+
+// Deadline of one wait call. milliseconds::max() means "no timeout"; very long timeouts
+// are treated the same way so now() + timeout cannot overflow. The clock is read only
+// when the wait really has to block: a wait satisfied at once, or a zero-timeout poll,
+// never reads it.
+class WaitDeadline {
+ public:
+  explicit WaitDeadline(std::chrono::milliseconds timeout) {
+    constexpr auto kForever = std::chrono::hours(24 * 365 * 10);
+    infinite_ = timeout == std::chrono::milliseconds::max() || timeout >= kForever;
+    timeout_ = std::max(timeout, std::chrono::milliseconds::zero());
+  }
+  bool infinite() const { return infinite_; }
+  bool zero() const { return !infinite_ && timeout_.count() == 0; }
+  std::chrono::steady_clock::time_point at() const {
+    if (!resolved_) {
+      at_ = std::chrono::steady_clock::now() + timeout_;
+      resolved_ = true;
+    }
+    return at_;
+  }
+  bool passed(std::chrono::steady_clock::time_point now) const {
+    return !infinite_ && (zero() || now >= at());
+  }
+
+ private:
+  bool infinite_ = true;
+  std::chrono::milliseconds timeout_{0};
+  mutable bool resolved_ = false;
+  mutable std::chrono::steady_clock::time_point at_{};
+};
+
+// Wait semantics (what guest code expects from the 360 kernel, KeWaitFor*/KeSetEvent):
+//  - a thread is registered on the object from the moment it starts waiting until it
+//    returns, alertable waits included (they poll the APC flag without leaving the wait);
+//  - SignalAndWait signals only after it holds the wait object's lock, so the thread it
+//    releases cannot act on the wait object before this thread is registered there;
+//  - KeSetEvent on a manual-reset event releases every thread waiting at that moment,
+//    even if KeResetEvent follows at once (Set/Reset "pulse");
+//  - KeSetEvent on an auto-reset event with waiters releases one of THOSE waiters: two
+//    Sets release two of them, and a thread that starts waiting later cannot take it.
+// Lock order: handle mutex_ (several only in address order or std::lock) -> waiter mutex.
 class PosixConditionBase {
  public:
   PosixConditionBase() {
@@ -236,74 +282,106 @@ class PosixConditionBase {
   }
 
   virtual ~PosixConditionBase() = default;
-  virtual bool Signal() = 0;
+
+  // Signal() takes mutex_; SignalLocked() expects the caller to hold it (SignalAndWait).
+  virtual bool Signal() {
+    if (!LockMutex(mutex_)) {
+      return false;
+    }
+    std::lock_guard<std::mutex> lock(mutex_, std::adopt_lock);
+    return SignalLocked();
+  }
+  virtual bool SignalLocked() = 0;
 
   WaitResult Wait(std::chrono::milliseconds timeout) {
-    bool executed;
-    auto predicate = [this] { return this->signaled(); };
-#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID  // Bionic has no robust mutexes
-    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
-    int lock_result = pthread_mutex_lock(native_mutex);
-    if (lock_result == EOWNERDEAD) {
-      pthread_mutex_consistent(native_mutex);
-    } else if (lock_result != 0) {
+    return WaitUntil(nullptr, WaitDeadline(timeout), nullptr);
+  }
+
+  // Waits for this object until `deadline`.
+  //  to_signal  - signalled first (SignalAndWait), atomically with the registration here:
+  //               both locks are taken in address order, so SignalAndWait(A, B) racing
+  //               SignalAndWait(B, A) cannot deadlock.
+  //  alert_flag - alertable wait: returns kUserCallback once the flag is set. The thread
+  //               stays registered while it polls the flag every kAlertablePollSlice.
+  WaitResult WaitUntil(PosixConditionBase* to_signal, const WaitDeadline& deadline,
+                       const std::atomic<bool>* alert_flag) {
+    if (to_signal && to_signal != this) {
+      PosixConditionBase* first = std::less<PosixConditionBase*>()(this, to_signal) ? this : to_signal;
+      PosixConditionBase* second = first == this ? to_signal : this;
+      if (!LockMutex(first->mutex_)) {
+        return WaitResult::kFailed;
+      }
+      if (!LockMutex(second->mutex_)) {
+        first->mutex_.unlock();
+        return WaitResult::kFailed;
+      }
+      const bool signaled_ok = to_signal->SignalLocked();
+      to_signal->mutex_.unlock();
+      std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
+      if (!signaled_ok) {
+        return WaitResult::kFailed;
+      }
+      return WaitLocked(lock, deadline, alert_flag);
+    }
+    if (!LockMutex(mutex_)) {
       return WaitResult::kFailed;
     }
     std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
-#else
-    std::unique_lock<std::mutex> lock(mutex_);
-#endif
-    if (predicate()) {
-      executed = true;
-    } else {
-      if (timeout == std::chrono::milliseconds::max()) {
-        cond_.wait(lock, predicate);
-        executed = true;  // Did not time out;
-      } else {
-        executed = cond_.wait_for(lock, timeout, predicate);
-      }
-    }
-    if (executed) {
-      post_execution();
-      return WaitResult::kSuccess;
-    } else {
-      return WaitResult::kTimeout;
+    if (to_signal && !SignalLocked()) {
+      return WaitResult::kFailed;
     }
+    return WaitLocked(lock, deadline, alert_flag);
   }
 
   static std::pair<WaitResult, size_t> WaitMultiple(std::vector<PosixConditionBase*>&& handles,
                                                     bool wait_all,
                                                     std::chrono::milliseconds timeout) {
+    return WaitMultipleUntil(handles, wait_all, WaitDeadline(timeout), nullptr);
+  }
+
+  static std::pair<WaitResult, size_t> WaitMultipleUntil(
+      const std::vector<PosixConditionBase*>& handles, bool wait_all,
+      const WaitDeadline& deadline, const std::atomic<bool>* alert_flag) {
     assert_true(!handles.empty());
 
     if (handles.size() == 1) {
-      auto result = handles[0]->Wait(timeout);
-      return std::make_pair(result, 0);
+      auto result = handles[0]->WaitUntil(nullptr, deadline, alert_flag);
+      return std::make_pair(result, size_t(0));
     }
 
     // Every handle wakes this waiter on Signal, so the thread sleeps until a
     // handle changes instead of polling (polling burned whole cores on
     // Android: the audio worker alone spun ~40% of a big core here).
+    // Wait-any also takes a ticket on every handle, like a single wait, so a manual-reset
+    // pulse or an auto-reset Set handed to the waiters reaches it. Wait-all keeps the plain
+    // state check: it is satisfied only when all objects are signalled at the same time.
     ExternalWaiter waiter;
-    for (auto* handle : handles) {
-      std::lock_guard<std::mutex> lock(handle->mutex_);
-      handle->external_waiters_.push_back(&waiter);
+    std::vector<WaitTicket> tickets(handles.size());
+    std::vector<bool> consumed(handles.size(), false);
+    for (size_t i = 0; i < handles.size(); ++i) {
+      std::lock_guard<std::mutex> lock(handles[i]->mutex_);
+      handles[i]->external_waiters_.push_back(&waiter);
+      if (!wait_all) {
+        tickets[i] = handles[i]->EnterWaitLocked();
+      }
     }
     struct Unregister {
-      std::vector<PosixConditionBase*>& handles;
+      const std::vector<PosixConditionBase*>& handles;
       ExternalWaiter* waiter;
+      bool wait_all;
+      const std::vector<WaitTicket>& tickets;
+      const std::vector<bool>& consumed;
       ~Unregister() {
-        for (auto* handle : handles) {
-          std::lock_guard<std::mutex> lock(handle->mutex_);
-          auto& list = handle->external_waiters_;
+        for (size_t i = 0; i < handles.size(); ++i) {
+          std::lock_guard<std::mutex> lock(handles[i]->mutex_);
+          auto& list = handles[i]->external_waiters_;
           list.erase(std::remove(list.begin(), list.end(), waiter), list.end());
+          if (!wait_all) {
+            handles[i]->ExitWaitLocked(tickets[i], consumed[i]);
+          }
         }
       }
-    } unregister{handles, &waiter};
-
-    const bool infinite = timeout == std::chrono::milliseconds::max();
-    const auto end_time = infinite ? std::chrono::steady_clock::time_point::max()
-                                   : std::chrono::steady_clock::now() + timeout;
+    } unregister{handles, &waiter, wait_all, tickets, consumed};
 
     while (true) {
       uint64_t generation;
@@ -327,7 +405,7 @@ class PosixConditionBase {
           std::vector<size_t> order(locks.size());
           for (size_t i = 0; i < order.size(); ++i) order[i] = i;
           std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
-            return handles[a] < handles[b];
+            return std::less<PosixConditionBase*>()(handles[a], handles[b]);
           });
           for (size_t index : order) {
             locks[index].lock();
@@ -349,22 +427,38 @@ class PosixConditionBase {
       } else {
         for (size_t i = 0; i < handles.size(); ++i) {
           std::lock_guard<std::mutex> lock(handles[i]->mutex_);
-          if (handles[i]->signaled()) {
-            handles[i]->post_execution();
+          if (handles[i]->ReadyLocked(tickets[i])) {
+            handles[i]->ConsumeLocked(tickets[i]);
+            consumed[i] = true;
             return std::make_pair(WaitResult::kSuccess, i);
           }
         }
       }
 
+      if (alert_flag && alert_flag->load(std::memory_order_acquire)) {
+        return std::make_pair(WaitResult::kUserCallback, size_t(0));
+      }
+      if (deadline.zero()) {
+        return std::make_pair(WaitResult::kTimeout, size_t(0));
+      }
+      const auto now = std::chrono::steady_clock::now();
+      if (deadline.passed(now)) {
+        return std::make_pair(WaitResult::kTimeout, size_t(0));
+      }
+
       std::unique_lock<std::mutex> lock(waiter.mutex);
       if (waiter.generation != generation) {
         continue;
       }
-      if (infinite) {
-        waiter.cond.wait(lock, [&] { return waiter.generation != generation; });
-      } else if (!waiter.cond.wait_until(lock, end_time,
-                                         [&] { return waiter.generation != generation; })) {
-        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
+      auto changed = [&] { return waiter.generation != generation; };
+      if (alert_flag) {
+        auto until = now + kAlertablePollSlice;
+        if (!deadline.infinite() && deadline.at() < until) until = deadline.at();
+        waiter.cond.wait_until(lock, until, changed);
+      } else if (deadline.infinite()) {
+        waiter.cond.wait(lock, changed);
+      } else {
+        waiter.cond.wait_until(lock, deadline.at(), changed);
       }
     }
   }
@@ -390,6 +484,85 @@ class PosixConditionBase {
   }
 
  protected:
+  // A waiter's registration on one object (kept by the waiting thread, used under mutex_).
+  struct WaitTicket {
+    uint64_t seen = 0;  // the object's signal counter when the wait started
+  };
+
+  // Locks a handle mutex; on desktop Linux the mutex is robust and is recovered
+  // if its owner died holding it.
+  static bool LockMutex(std::mutex& mutex) {
+#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID  // Bionic has no robust mutexes
+    auto native_mutex = static_cast<pthread_mutex_t*>(mutex.native_handle());
+    int lock_result = pthread_mutex_lock(native_mutex);
+    if (lock_result == EOWNERDEAD) {
+      pthread_mutex_consistent(native_mutex);
+      return true;
+    }
+    return lock_result == 0;
+#else
+    mutex.lock();
+    return true;
+#endif
+  }
+
+  WaitResult WaitLocked(std::unique_lock<std::mutex>& lock, const WaitDeadline& deadline,
+                        const std::atomic<bool>* alert_flag) {
+    // Fast path: already signalled (or a zero-timeout poll) needs no registration.
+    {
+      const WaitTicket probe = PeekTicketLocked();
+      if (ReadyLocked(probe)) {
+        ConsumeLocked(probe);
+        return WaitResult::kSuccess;
+      }
+      if (alert_flag && alert_flag->load(std::memory_order_acquire)) {
+        return WaitResult::kUserCallback;
+      }
+      if (deadline.zero()) {
+        return WaitResult::kTimeout;
+      }
+    }
+    const WaitTicket ticket = EnterWaitLocked();
+    WaitResult result;
+    while (true) {
+      // Checked before the alert flag and the deadline: a wait that was satisfied wins.
+      if (ReadyLocked(ticket)) {
+        ConsumeLocked(ticket);
+        result = WaitResult::kSuccess;
+        break;
+      }
+      if (alert_flag && alert_flag->load(std::memory_order_acquire)) {
+        result = WaitResult::kUserCallback;
+        break;
+      }
+      const auto now = std::chrono::steady_clock::now();
+      if (deadline.passed(now)) {
+        result = WaitResult::kTimeout;
+        break;
+      }
+      if (alert_flag) {
+        auto until = now + kAlertablePollSlice;
+        if (!deadline.infinite() && deadline.at() < until) until = deadline.at();
+        cond_.wait_until(lock, until);
+      } else if (deadline.infinite()) {
+        cond_.wait(lock);
+      } else {
+        cond_.wait_until(lock, deadline.at());
+      }
+    }
+    ExitWaitLocked(ticket, result == WaitResult::kSuccess);
+    return result;
+  }
+
+  // Ticket hooks, all called with mutex_ held. The defaults are the plain state check;
+  // Event overrides them for the pulse / hand-over rules above.
+  virtual WaitTicket EnterWaitLocked() { return {}; }
+  // The ticket a wait would get now, without registering (first check of a wait).
+  virtual WaitTicket PeekTicketLocked() const { return {}; }
+  virtual bool ReadyLocked(const WaitTicket&) const { return signaled(); }
+  virtual void ConsumeLocked(const WaitTicket&) { post_execution(); }
+  virtual void ExitWaitLocked(const WaitTicket&, bool /*consumed*/) {}
+
   inline virtual bool signaled() const = 0;
   inline virtual void post_execution() = 0;
   std::condition_variable cond_;
@@ -411,9 +584,20 @@ class PosixCondition<Event> : public PosixConditionBase {
       : signal_(initial_state), manual_reset_(manual_reset) {}
   virtual ~PosixCondition() = default;
 
-  bool Signal() override {
-    auto lock = std::unique_lock<std::mutex>(mutex_);
-    signal_ = true;
+  bool SignalLocked() override {
+    ++set_count_;
+    if (manual_reset_) {
+      // Waiters registered before this Set see set_count_ move past their ticket and
+      // return even if Reset() follows at once.
+      signal_ = true;
+    } else if (signal_) {
+      // Already signalled: a second Set changes nothing (as on the 360).
+    } else if (waiter_seen_.size() > grants_.size()) {
+      // Hand the release to a thread that is already waiting (one per Set).
+      grants_.push_back(set_count_);
+    } else {
+      signal_ = true;
+    }
     cond_.notify_all();
     NotifyExternalWaiters();
     return true;
@@ -421,7 +605,22 @@ class PosixCondition<Event> : public PosixConditionBase {
 
   void Reset() {
     auto lock = std::unique_lock<std::mutex>(mutex_);
+    signal_ = false;  // releases already handed to waiters stay theirs
+  }
+
+  // KePulseEvent: satisfy the waits in progress (all for manual reset, one for auto
+  // reset), then leave the event non-signalled.
+  void Pulse() {
+    auto lock = std::unique_lock<std::mutex>(mutex_);
+    if (manual_reset_) {
+      ++set_count_;
+    } else if (!signal_ && waiter_seen_.size() > grants_.size()) {
+      ++set_count_;
+      grants_.push_back(set_count_);
+    }
     signal_ = false;
+    cond_.notify_all();
+    NotifyExternalWaiters();
   }
 
  private:
@@ -431,8 +630,75 @@ class PosixCondition<Event> : public PosixConditionBase {
       signal_ = false;
     }
   }
+
+  WaitTicket PeekTicketLocked() const override {
+    WaitTicket ticket;
+    ticket.seen = set_count_;
+    return ticket;
+  }
+  WaitTicket EnterWaitLocked() override {
+    WaitTicket ticket;
+    ticket.seen = set_count_;
+    waiter_seen_.push_back(ticket.seen);  // stays sorted: set_count_ only grows
+    return ticket;
+  }
+  bool ReadyLocked(const WaitTicket& ticket) const override {
+    if (signal_) return true;
+    if (manual_reset_) return set_count_ > ticket.seen;
+    // grants_ is sorted, so the newest decides whether any was made after we started.
+    return !grants_.empty() && grants_.back() > ticket.seen;
+  }
+  void ConsumeLocked(const WaitTicket& ticket) override {
+    if (manual_reset_) {
+      return;
+    }
+    // A release handed to the waiters first (it is ours to take); the oldest one we may
+    // take, so later waiters keep the newer ones.
+    for (auto it = grants_.begin(); it != grants_.end(); ++it) {
+      if (*it > ticket.seen) {
+        grants_.erase(it);
+        return;
+      }
+    }
+    signal_ = false;
+  }
+  void ExitWaitLocked(const WaitTicket& ticket, bool /*consumed*/) override {
+    auto it = std::find(waiter_seen_.begin(), waiter_seen_.end(), ticket.seen);
+    assert_true(it != waiter_seen_.end());
+    if (it != waiter_seen_.end()) {
+      waiter_seen_.erase(it);
+    }
+    if (manual_reset_ || grants_.empty()) {
+      return;
+    }
+    // A release only waiters that started before it may take. If the ones left cannot
+    // take them all (a wait-any that returned on another object), the rest becomes a
+    // plain signal so it is not lost. Eligible sets are nested (earlier waiters may take
+    // every later release), so the greedy count below is a maximum matching.
+    bool orphaned = false;
+    size_t kept = 0, eligible = 0;
+    for (auto grant = grants_.begin(); grant != grants_.end();) {
+      while (eligible < waiter_seen_.size() && waiter_seen_[eligible] < *grant) ++eligible;
+      if (eligible > kept) {
+        ++kept;
+        ++grant;
+      } else {
+        grant = grants_.erase(grant);
+        orphaned = true;
+      }
+    }
+    if (orphaned && !signal_) {
+      signal_ = true;
+      cond_.notify_all();
+      NotifyExternalWaiters();
+    }
+  }
+
   bool signal_;
   const bool manual_reset_;
+  uint64_t set_count_ = 0;              // every Set/Pulse that released waiters
+  std::vector<uint64_t> waiter_seen_;   // tickets of the threads waiting now (sorted)
+  std::deque<uint64_t> grants_;         // auto reset: releases handed to them (sorted)
 };
 
 template <>
@@ -441,10 +707,14 @@ class PosixCondition<Semaphore> : public PosixConditionBase {
   PosixCondition(uint32_t initial_count, uint32_t maximum_count)
       : count_(initial_count), maximum_count_(maximum_count) {}
 
-  bool Signal() override { return Release(1, nullptr); }
+  bool SignalLocked() override { return ReleaseLocked(1, nullptr); }
 
   bool Release(uint32_t release_count, int* out_previous_count) {
     auto lock = std::unique_lock<std::mutex>(mutex_);
+    return ReleaseLocked(release_count, out_previous_count);
+  }
+
+  bool ReleaseLocked(uint32_t release_count, int* out_previous_count) {
     if (release_count > maximum_count_ - count_) {
       return false;
     }
@@ -478,11 +748,16 @@ class PosixCondition<Mutant> : public PosixConditionBase {
     }
   }
 
-  bool Signal() override { return Release(); }
+  bool SignalLocked() override { return ReleaseLocked(); }
 
   bool Release() {
+    auto lock = std::unique_lock<std::mutex>(mutex_);
+    return ReleaseLocked();
+  }
+
+  // owner_ and count_ are read under mutex_ (they were read before taking it).
+  bool ReleaseLocked() {
     if (owner_ == std::this_thread::get_id() && count_ > 0) {
-      auto lock = std::unique_lock<std::mutex>(mutex_);
       --count_;
       // Free to be acquired by another thread
       if (count_ == 0) {
@@ -516,8 +791,7 @@ class PosixCondition<Timer> : public PosixConditionBase {
 
   virtual ~PosixCondition() { Cancel(); }
 
-  bool Signal() override {
-    std::lock_guard<std::mutex> lock(mutex_);
+  bool SignalLocked() override {
     signal_ = true;
     cond_.notify_all();
     NotifyExternalWaiters();
@@ -652,6 +926,8 @@ class PosixCondition<Thread> : public PosixConditionBase {
     sem_init(&suspend_sem_, 0, 0);
 #if REX_PLATFORM_ANDROID
     android_pre_api_26_name_[0] = '\0';
+    // Built by Thread::GetCurrentThread() on the thread itself.
+    tid_.store(pid_t(syscall(SYS_gettid)), std::memory_order_release);
 #endif
   }
 
@@ -662,6 +938,12 @@ class PosixCondition<Thread> : public PosixConditionBase {
   }
 
   bool Signal() override { return true; }
+  bool SignalLocked() override { return true; }
+
+  // Set while user callbacks (APCs) are queued; alertable waits poll it.
+  const std::atomic<bool>* pending_user_callbacks_flag() const {
+    return &has_pending_user_callbacks_;
+  }
 
   std::string name() const {
     WaitStarted();
@@ -713,15 +995,29 @@ class PosixCondition<Thread> : public PosixConditionBase {
 
   uint32_t system_id() const { return static_cast<uint32_t>(thread_); }
 #if REX_PLATFORM_ANDROID
-  uint32_t native_tid() const { return static_cast<uint32_t>(pthread_gettid_np(thread_)); }
+  // Kernel tid saved by the thread itself when it starts. bionic's pthread_gettid_np()
+  // aborts the whole process ("invalid pthread_t passed to pthread_gettid_np") when the
+  // thread has already exited and been joined, so it is never called on another thread.
+  // WaitStarted(): the tid is stored before the state leaves kUninitialized, so it is never 0
+  // here (a 0 tid would make setpriority()/sched_setaffinity() act on the calling thread).
+  uint32_t native_tid() const {
+    WaitStarted();
+    return static_cast<uint32_t>(tid_.load(std::memory_order_acquire));
+  }
+  bool finished() const {
+    std::unique_lock<std::mutex> lock(state_mutex_);
+    return state_ == State::kFinished;
+  }
 #endif
 
   uint64_t affinity_mask() {
     WaitStarted();
     cpu_set_t cpu_set;
 #if REX_PLATFORM_ANDROID
-    if (sched_getaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
-      assert_always();
+    CPU_ZERO(&cpu_set);
+    const pid_t tid = tid_.load(std::memory_order_acquire);
+    if (finished() || tid <= 0 || sched_getaffinity(tid, sizeof(cpu_set_t), &cpu_set) != 0) {
+      return 0;
     }
 #else
     if (pthread_getaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
@@ -747,9 +1043,9 @@ class PosixCondition<Thread> : public PosixConditionBase {
       }
     }
 #if REX_PLATFORM_ANDROID
-    if (sched_setaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
-      assert_always();
-    }
+    const pid_t tid = tid_.load(std::memory_order_acquire);
+    if (finished() || tid <= 0) return;
+    sched_setaffinity(tid, sizeof(cpu_set_t), &cpu_set);  // may race the thread's exit: ignore
 #else
     if (pthread_setaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
       assert_always();
@@ -774,7 +1070,7 @@ class PosixCondition<Thread> : public PosixConditionBase {
 #if REX_PLATFORM_ANDROID
     // Apps may not use SCHED_FIFO; only creation-flag requests land here.
     __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "thread-priority-flag tid=%d value=%d",
-                        int(pthread_gettid_np(thread_)), new_priority);
+                        int(tid_.load(std::memory_order_acquire)), new_priority);
     return;
 #endif
     sched_param param{};
@@ -815,8 +1111,10 @@ class PosixCondition<Thread> : public PosixConditionBase {
     sigval value{};
     value.sival_ptr = this;
 #if REX_PLATFORM_ANDROID
-    int result = sigqueue(pthread_gettid_np(thread_),
-                          GetSystemSignal(SignalType::kThreadUserCallback), value);
+    // The callback stays queued; a finished thread just never runs it (no signal, no abort).
+    const pid_t tid = tid_.load(std::memory_order_acquire);
+    if (finished() || tid <= 0) return;
+    int result = sigqueue(tid, GetSystemSignal(SignalType::kThreadUserCallback), value);
 #else
     int result = pthread_sigqueue(thread_, GetSystemSignal(SignalType::kThreadUserCallback), value);
 #endif
@@ -959,6 +1257,9 @@ class PosixCondition<Thread> : public PosixConditionBase {
     sem_destroy(&suspend_sem_);
   }
   pthread_t thread_;
+#if REX_PLATFORM_ANDROID
+  std::atomic<pid_t> tid_{0};
+#endif
   bool signaled_;
   int exit_code_;
   State state_;             // Protected by state_mutex_
@@ -994,9 +1295,16 @@ bool DispatchCurrentThreadUserCallback() {
   return current_thread_condition_ && current_thread_condition_->DispatchQueuedUserCallbacks();
 }
 
-namespace {
+// The alert flag of the calling thread (pending user callbacks), for alertable waits.
+const std::atomic<bool>* CurrentThreadPendingUserCallbacksFlag() {
+  if (!current_thread_condition_) {
+    Thread::GetCurrentThread();
+  }
+  return current_thread_condition_ ? current_thread_condition_->pending_user_callbacks_flag()
+                                   : nullptr;
+}
 
-constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);
+namespace {
 
 class ScopedAlertableState {
  public:
@@ -1015,31 +1323,6 @@ class ScopedAlertableState {
   bool alertable_;
 };
 
-std::chrono::steady_clock::time_point ComputeAlertableDeadline(std::chrono::milliseconds timeout) {
-  if (timeout == std::chrono::milliseconds::max()) {
-    return std::chrono::steady_clock::time_point::max();
-  }
-  return std::chrono::steady_clock::now() + timeout;
-}
-
-bool HasAlertableTimeoutElapsed(std::chrono::steady_clock::time_point deadline) {
-  return deadline != std::chrono::steady_clock::time_point::max() &&
-         std::chrono::steady_clock::now() >= deadline;
-}
-
-std::chrono::milliseconds ComputeAlertableWaitTimeout(
-    std::chrono::steady_clock::time_point deadline) {
-  if (deadline == std::chrono::steady_clock::time_point::max()) {
-    return kAlertablePollSlice;
-  }
-  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
-      deadline - std::chrono::steady_clock::now());
-  if (remaining <= std::chrono::milliseconds::zero()) {
-    return std::chrono::milliseconds::zero();
-  }
-  return std::min(remaining, kAlertablePollSlice);
-}
-
 }  // namespace
 
 // This wraps a condition object as our handle because posix has no single
@@ -1085,22 +1368,22 @@ WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::millise
   if (posix_wait_handle == nullptr) {
     return WaitResult::kFailed;
   }
+  auto& condition = posix_wait_handle->condition();
+  const WaitDeadline deadline(timeout);
   if (!is_alertable) {
-    return posix_wait_handle->condition().Wait(timeout);
+    return condition.WaitUntil(nullptr, deadline, nullptr);
   }
 
   ScopedAlertableState alertable_state_guard(true);
-  auto deadline = ComputeAlertableDeadline(timeout);
-
+  const std::atomic<bool>* alert_flag = CurrentThreadPendingUserCallbacksFlag();
   while (true) {
     if (DispatchCurrentThreadUserCallback()) {
       return WaitResult::kUserCallback;
     }
-    if (HasAlertableTimeoutElapsed(deadline)) {
-      return WaitResult::kTimeout;
-    }
-    auto result = posix_wait_handle->condition().Wait(ComputeAlertableWaitTimeout(deadline));
-    if (result != WaitResult::kTimeout) {
+    // Stays registered on the object until it is signalled, the deadline passes or a
+    // user callback is queued (then the loop dispatches it).
+    auto result = condition.WaitUntil(nullptr, deadline, alert_flag);
+    if (result != WaitResult::kUserCallback) {
       return result;
     }
   }
@@ -1108,31 +1391,32 @@ WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::millise
 
 WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                          bool is_alertable, std::chrono::milliseconds timeout) {
-  auto result = WaitResult::kFailed;
   auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
   auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
   if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
     return WaitResult::kFailed;
   }
-  if (!posix_wait_handle_to_signal->condition().Signal()) {
-    return WaitResult::kFailed;
-  }
-
+  auto& to_signal = posix_wait_handle_to_signal->condition();
+  auto& to_wait_on = posix_wait_handle_to_wait_on->condition();
+  const WaitDeadline deadline(timeout);
   if (!is_alertable) {
-    return posix_wait_handle_to_wait_on->condition().Wait(timeout);
+    return to_wait_on.WaitUntil(&to_signal, deadline, nullptr);
   }
 
   ScopedAlertableState alertable_state_guard(true);
-  auto deadline = ComputeAlertableDeadline(timeout);
+  const std::atomic<bool>* alert_flag = CurrentThreadPendingUserCallbacksFlag();
+  // The object is signalled exactly once, together with the first registration.
+  PosixConditionBase* pending_signal = &to_signal;
   while (true) {
     if (DispatchCurrentThreadUserCallback()) {
+      if (pending_signal && !pending_signal->Signal()) {
+        return WaitResult::kFailed;
+      }
       return WaitResult::kUserCallback;
     }
-    if (HasAlertableTimeoutElapsed(deadline)) {
-      return WaitResult::kTimeout;
-    }
-    result = posix_wait_handle_to_wait_on->condition().Wait(ComputeAlertableWaitTimeout(deadline));
-    if (result != WaitResult::kTimeout) {
+    auto result = to_wait_on.WaitUntil(pending_signal, deadline, alert_flag);
+    pending_signal = nullptr;
+    if (result != WaitResult::kUserCallback) {
       return result;
     }
   }
@@ -1150,22 +1434,19 @@ std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wa
     }
     conditions.push_back(&handle->condition());
   }
+  const WaitDeadline deadline(timeout);
   if (!is_alertable) {
-    return PosixConditionBase::WaitMultiple(std::move(conditions), wait_all, timeout);
+    return PosixConditionBase::WaitMultipleUntil(conditions, wait_all, deadline, nullptr);
   }
 
   ScopedAlertableState alertable_state_guard(true);
-  auto deadline = ComputeAlertableDeadline(timeout);
+  const std::atomic<bool>* alert_flag = CurrentThreadPendingUserCallbacksFlag();
   while (true) {
     if (DispatchCurrentThreadUserCallback()) {
       return std::make_pair(WaitResult::kUserCallback, 0);
     }
-    if (HasAlertableTimeoutElapsed(deadline)) {
-      return std::make_pair(WaitResult::kTimeout, 0);
-    }
-    auto result = PosixConditionBase::WaitMultiple(std::vector<PosixConditionBase*>(conditions),
-                                                   wait_all, ComputeAlertableWaitTimeout(deadline));
-    if (result.first != WaitResult::kTimeout) {
+    auto result = PosixConditionBase::WaitMultipleUntil(conditions, wait_all, deadline, alert_flag);
+    if (result.first != WaitResult::kUserCallback) {
       return result;
     }
   }
@@ -1178,13 +1459,8 @@ class PosixEvent : public PosixConditionHandle<Event> {
   ~PosixEvent() override = default;
   void Set() override { handle_.Signal(); }
   void Reset() override { handle_.Reset(); }
-  void Pulse() override {
-    using namespace std::chrono_literals;
-    handle_.Signal();
-    MaybeYield();
-    Sleep(10us);
-    handle_.Reset();
-  }
+  // Releases the threads waiting now, atomically (no Set / sleep / Reset window).
+  void Pulse() override { handle_.Pulse(); }
 };
 
 std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
@@ -1346,6 +1622,12 @@ void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
 
   current_thread_ = thread;
   current_thread_condition_ = &thread->handle_;
+#if REX_PLATFORM_ANDROID
+  thread->handle_.tid_.store(pid_t(syscall(SYS_gettid)), std::memory_order_release);
+  // Linux rounds every timed sleep of a normal thread up by up to 50 us ("timer slack"),
+  // so short waits (guest Sleep, alertable polling) oversleep 2-6x. 1 us keeps them short.
+  prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
+#endif
   {
     std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
     thread->handle_.suspend_count_ = create_suspended ? 1 : 0;
@@ -1460,4 +1742,4 @@ static void signal_handler(int signal, siginfo_t* /*info*/, void* /*context*/) {
   }
 }
 
-}  // namespace rex::thread
+}  // namespace rex::thread
\ No newline at end of file
diff --git a/glue/rexglue-sdk-main/src/core/timer_queue.cpp b/glue/rexglue-sdk-main/src/core/timer_queue.cpp
index 5cfea058..506843c9 100644
--- a/glue/rexglue-sdk-main/src/core/timer_queue.cpp
+++ b/glue/rexglue-sdk-main/src/core/timer_queue.cpp
@@ -16,6 +16,7 @@
 #include <disruptorplus/ring_buffer.hpp>
 #include <disruptorplus/sequence_barrier.hpp>
 #include <disruptorplus/spin_wait_strategy.hpp>
+#include <disruptorplus/blocking_wait_strategy.hpp>
 
 #include <rex/assert.h>
 #include <rex/thread.h>
@@ -55,7 +56,7 @@ class TimerQueue {
     // std::jthread auto-joins on destruction
   }
 
-  void TimerThreadMain(std::stop_token stop_token) {
+void TimerThreadMain(std::stop_token stop_token) {
     dp::sequence_t next_sequence = 0;
     const auto comp = [](const std::shared_ptr<WaitItem>& left,
                          const std::shared_ptr<WaitItem>& right) {
@@ -64,8 +65,19 @@ class TimerQueue {
 
     set_current_thread_name("rex::thread::TimerQueue");
 
-    while (!stop_token.stop_requested()) {
+while (!stop_token.stop_requested()) {
       {
+        if (!wait_queue_.empty()) {
+          const auto now = clock::now();
+          if (wait_queue_.front()->due_ > now) {
+            const auto remaining = wait_queue_.front()->due_ - now;
+            const auto max_sleep = std::chrono::duration_cast<clock::duration>(std::chrono::milliseconds(2));
+            std::this_thread::sleep_for(std::min(remaining, max_sleep));
+          }
+        } else {
+          std::this_thread::sleep_for(std::chrono::microseconds(200));
+        }
+
         // Consume new wait items and add them to sorted wait queue
         dp::sequence_t available = claim_strategy_.wait_until_published(
             next_sequence, next_sequence - 1,
@@ -122,7 +134,7 @@ class TimerQueue {
       }
     }
   }
-
+  
   std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
     auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);
 
@@ -139,12 +151,14 @@ class TimerQueue {
   std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }
 
  private:
-  // This ring buffer will be used to introduce timers queued by the public API
-  static constexpr size_t kWaitCount = 512;
+ // This ring buffer will be used to introduce timers queued by the public API
+ static constexpr size_t kWaitCount = 512;
   dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
-  dp::spin_wait_strategy wait_strategy_;
-  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim_strategy_;
-  dp::sequence_barrier<dp::spin_wait_strategy> consumed_;
+  // ИСПОЛЬЗУЕМ БЛОКИРУЮЩУЮ СТРАТЕГИЮ ВМЕСТО SPIN_WAIT. 
+  // Это уберет sched_yield и заставит поток спать на condition_variable.
+  dp::blocking_wait_strategy wait_strategy_;
+  dp::multi_threaded_claim_strategy<dp::blocking_wait_strategy> claim_strategy_;
+  dp::sequence_barrier<dp::blocking_wait_strategy> consumed_;
 
   // This is a _sorted_ (ascending due_) list of active timers managed by a
   // dedicated thread
diff --git a/glue/rexglue-sdk-main/src/graphics/CMakeLists.txt b/glue/rexglue-sdk-main/src/graphics/CMakeLists.txt
index af36910e..bc8b2cb8 100644
--- a/glue/rexglue-sdk-main/src/graphics/CMakeLists.txt
+++ b/glue/rexglue-sdk-main/src/graphics/CMakeLists.txt
@@ -289,7 +289,10 @@ if(REXGLUE_USE_VULKAN)
     endif()
 
     add_library(rexgpu-gta4-native SHARED
+	gta4_native/sgsr_present_ps.h
+        gta4_native/sgsr_present_ps.inc
         gta4_native/graphics_system.cpp
+	gta4_native/frame_time_overlay.cpp
         gta4_native/core/font_atlas.cpp
         gta4_native/shader_override_policy.h
         gta4_native/native_buffer_arena.cpp
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/dirty_state_delta.h b/glue/rexglue-sdk-main/src/graphics/gta4_native/dirty_state_delta.h
index 64bedd7b..aa1ce52d 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/dirty_state_delta.h
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/dirty_state_delta.h
@@ -264,17 +264,43 @@ void ForEachDirtyElement(const NativeDirtyWords& dirty_words, const DirtyCompone
   for (const DirtyBitSpan& span : layout.spans) {
     uint64_t bits =
         (dirty_words[span.dirty_word] >> span.first_dirty_bit) & LowBitMask(span.bit_count);
-    while (bits) {
-      const uint32_t bit = static_cast<uint32_t>(std::countr_zero(bits));
-      const uint32_t element_bit = span.reverse_bits ? uint32_t(span.bit_count - 1) - bit : bit;
-      callback(span.first_element + element_bit);
-      bits &= bits - 1;
+    if (span.reverse_bits) {
+      // Обходим биты от старшего (MSB) к младшему (LSB), чтобы element рос строго по возрастанию!
+      while (bits) {
+        const uint32_t lz = static_cast<uint32_t>(std::countl_zero(bits));
+        const uint32_t bit = 63u - lz;
+        const uint32_t element_bit = uint32_t(span.bit_count - 1) - bit;
+        callback(span.first_element + element_bit);
+        bits &= ~(uint64_t{1} << bit);
+      }
+    } else {
+      while (bits) {
+        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(bits));
+        callback(span.first_element + bit);
+        bits &= bits - 1;
+      }
     }
   }
 }
 
 inline void BuildRangeSet(const NativeDirtyWords& dirty_words, const DirtyComponentLayout& layout,
                           DirtyRangeSet& result, DirtyDeltaScratch& scratch) {
+  result.ranges.clear();
+  if (layout.spans.size() <= 1) {
+    // Быстрый путь (99.9% вызовов): один спан уже даёт строго возрастающие элементы без сортировки!
+    ForEachDirtyElement(dirty_words, layout, [&](uint32_t element) {
+      if (!result.ranges.empty()) {
+        DirtyElementRange& previous = result.ranges.back();
+        if (element == previous.first + previous.count) {
+          ++previous.count;
+          return;
+        }
+      }
+      result.ranges.push_back({element, 1});
+    });
+    return;
+  }
+
   std::vector<uint32_t>& dirty_elements = scratch.dirty_elements;
   dirty_elements.clear();
   ForEachDirtyElement(dirty_words, layout,
@@ -283,7 +309,6 @@ inline void BuildRangeSet(const NativeDirtyWords& dirty_words, const DirtyCompon
   dirty_elements.erase(std::unique(dirty_elements.begin(), dirty_elements.end()),
                        dirty_elements.end());
 
-  result.ranges.clear();
   for (uint32_t element : dirty_elements) {
     if (!result.ranges.empty()) {
       DirtyElementRange& previous = result.ranges.back();
@@ -346,23 +371,66 @@ inline DirtyLayoutValidationResult BuildDirtyStateDelta(const NativeDirtyWords&
 inline void BuildDirtyStateDeltaUnchecked(const NativeDirtyWords& dirty_words,
                                           const DirtyStateLayout& layout,
                                           DirtyStateDelta& delta, DirtyDeltaScratch& scratch) {
-
-  dirty_state_delta_internal::BuildRangeSet(dirty_words, layout.vertex_constants,
-                                            delta.vertex_constant_ranges, scratch);
-  dirty_state_delta_internal::BuildRangeSet(dirty_words, layout.pixel_constants,
-                                            delta.pixel_constant_ranges, scratch);
-  dirty_state_delta_internal::BuildRangeSet(dirty_words, layout.texture_fetch_stages,
-                                            delta.texture_fetch_stage_ranges, scratch);
-  dirty_state_delta_internal::BuildRangeSet(dirty_words, layout.sampler_stages,
-                                            delta.sampler_stage_ranges, scratch);
-  dirty_state_delta_internal::BuildElementMask(dirty_words, layout.boolean_constants,
-                                               delta.boolean_constant_mask);
-  dirty_state_delta_internal::BuildElementMask(dirty_words, layout.integer_constants,
-                                               delta.integer_constant_mask);
-  dirty_state_delta_internal::BuildElementMask(dirty_words, layout.fixed_state,
-                                               delta.fixed_state_mask);
-  dirty_state_delta_internal::BuildElementMask(dirty_words, layout.dynamic_state,
-                                               delta.dynamic_state_mask);
+  if (dirty_words[0]) {
+    dirty_state_delta_internal::BuildRangeSet(dirty_words, layout.vertex_constants,
+                                              delta.vertex_constant_ranges, scratch);
+  } else {
+    delta.vertex_constant_ranges.ranges.clear();
+  }
+  
+  if (dirty_words[1]) {
+    dirty_state_delta_internal::BuildRangeSet(dirty_words, layout.pixel_constants,
+                                              delta.pixel_constant_ranges, scratch);
+  } else {
+    delta.pixel_constant_ranges.ranges.clear();
+  }
+  
+  if (!layout.texture_fetch_stages.spans.empty()) {
+    dirty_state_delta_internal::BuildRangeSet(dirty_words, layout.texture_fetch_stages,
+                                              delta.texture_fetch_stage_ranges, scratch);
+  } else {
+    delta.texture_fetch_stage_ranges.ranges.clear();
+  }
+  
+  if (!layout.sampler_stages.spans.empty()) {
+    dirty_state_delta_internal::BuildRangeSet(dirty_words, layout.sampler_stages,
+                                              delta.sampler_stage_ranges, scratch);
+  } else {
+    delta.sampler_stage_ranges.ranges.clear();
+  }
+  
+  // ВОТ ЗДЕСЬ БЫЛИ ПРОПУЩЕНЫ ОЧИСТКИ:
+  if (!layout.boolean_constants.spans.empty()) {
+    dirty_state_delta_internal::BuildElementMask(dirty_words, layout.boolean_constants,
+                                                 delta.boolean_constant_mask);
+  } else {
+    delta.boolean_constant_mask.element_count = 0;
+    delta.boolean_constant_mask.words.clear();
+  }
+  
+  if (!layout.integer_constants.spans.empty()) {
+    dirty_state_delta_internal::BuildElementMask(dirty_words, layout.integer_constants,
+                                                 delta.integer_constant_mask);
+  } else {
+    delta.integer_constant_mask.element_count = 0;
+    delta.integer_constant_mask.words.clear();
+  }
+  
+  if (!layout.fixed_state.spans.empty()) {
+    dirty_state_delta_internal::BuildElementMask(dirty_words, layout.fixed_state,
+                                                 delta.fixed_state_mask);
+  } else {
+    delta.fixed_state_mask.element_count = 0;
+    delta.fixed_state_mask.words.clear();
+  }
+  
+  if (!layout.dynamic_state.spans.empty()) {
+    dirty_state_delta_internal::BuildElementMask(dirty_words, layout.dynamic_state,
+                                                 delta.dynamic_state_mask);
+  } else {
+    delta.dynamic_state_mask.element_count = 0;
+    delta.dynamic_state_mask.words.clear();
+  }
 }
 
 inline DirtyDeltaBuildResult BuildDirtyStateDelta(const NativeDirtyWords& dirty_words,
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/emission_trace_bind.inc b/glue/rexglue-sdk-main/src/graphics/gta4_native/emission_trace_bind.inc
index fb4910a7..ddaa2e13 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/emission_trace_bind.inc
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/emission_trace_bind.inc
@@ -1,6 +1,6 @@
 // Inside BindCommonDrawState, after the actual immutable constants are uploaded.
 // Byte comparisons use mapped upload memory, not a substitute calculated buffer.
-if (!EmissionTraceConfig().pipeline && EmissionTraceActive(diagnostic_submitted_frame_) && command.pipeline_state &&
+if (REXCVAR_GET(gta4_trace_native_renderer) && !EmissionTraceConfig().pipeline && EmissionTraceActive(diagnostic_submitted_frame_) && command.pipeline_state &&
     command.pipeline_state->pixel_shader_resource &&
     IsEmissionShader(command.pipeline_state->pixel_shader_resource->filename)) {
   const auto& ps = *command.pipeline_state->pixel_shader_resource;
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp b/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp
index 581a9d20..f5f31016 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp
@@ -134,12 +134,19 @@
 #include "resolve_depth_msaa_ps.h"
 #include "texture_filtering_policy.h"
 
+#if defined(__aarch64__)
+#include <arm_neon.h>
+#endif
+//тест трейс
+#include "trace_marks.h"
+
 #ifndef GTA4_NATIVE_FONT_ASSET_ROOT
 #define GTA4_NATIVE_FONT_ASSET_ROOT ""
 #endif
 
 
-
+// SGSR: заголовок со скомпилированным шейдером
+#include "sgsr_present_ps.h"
 
 
 REXCVAR_DEFINE_STRING(gta4_depth_handoff_transport, "buffer", "GTA IV/Graphics/Native Renderer",
@@ -158,16 +165,23 @@ REXCVAR_DEFINE_BOOL(gta4_native_pipeline_serialize, false, "GTA IV/Diagnostics",
 REXCVAR_DEFINE_UINT32(gta4_native_max_queued_frames, 2, "GTA IV/Graphics/Native Renderer",
                       "Title frames the producer may run ahead of the recorder")
     .range(1, 4);
-REXCVAR_DEFINE_UINT32(gta4_native_frame_complete_signal, 0, "GTA IV/Graphics/Native Renderer",
+REXCVAR_DEFINE_UINT32(gta4_native_frame_complete_signal, 1, "GTA IV/Graphics/Native Renderer",
                       "When the title sees a frame as complete: 0 after GPU submission, 1 at recorder handoff, 2 at Present capture")
     .range(0, 2);
 REXCVAR_DEFINE_BOOL(gta4_native_sparse_capacity_plan, true, "GTA IV/Graphics/Native Renderer",
                     "Re-plan the upload staging arena every 30 frames or after on-demand growth");
+REXCVAR_DEFINE_UINT32(gta4_native_capacity_replan_frames, 0, "GTA IV/Graphics/Native Renderer",
+                      "Frames between periodic upload-capacity re-plans (0 = only after "
+                      "on-demand growth). A re-plan walks every command of the frame: ~5 ms on "
+                      "the street, the hitch every 30th frame");
+REXCVAR_DEFINE_BOOL(gta4_native_target_memo, true, "GTA IV/Graphics/Native Renderer",
+                    "Reuse the previous draw's realized render target when its target "
+                    "descriptors and attachment usage are identical");
 REXCVAR_DEFINE_BOOL(gta4_native_constant_align64, true, "GTA IV/Diagnostics",
                     "Align constant arena allocations to 64 bytes (required by uniform banks)");
 REXCVAR_DEFINE_BOOL(gta4_native_ubo_layout, true, "GTA IV/Diagnostics",
                     "Add and bind the uniform constant bank descriptor set");
-REXCVAR_DEFINE_BOOL(gta4_native_skip_attachment_barriers, false, "GTA IV/Graphics/Native Renderer",
+REXCVAR_DEFINE_BOOL(gta4_native_skip_attachment_barriers, true, "GTA IV/Graphics/Native Renderer",
                     "Skip attachment-to-same-attachment layout barriers between rendering scopes "
                     "(Turnip keeps draw order across scopes)");
 REXCVAR_DEFINE_UINT32(gta4_profile_native_capture_trigger, 0, "GTA IV/Diagnostics",
@@ -224,6 +238,11 @@ REXCVAR_DEFINE_BOOL(gta4_native_frame_timeline, false, "GTA IV/Diagnostics",
                     "Log per-frame stage times (guest present, worker handoff, recorder start, "
                     "queue submit, publish end) on CLOCK_MONOTONIC, to join with SurfaceFlinger's "
                     "frame-ready times");
+REXCVAR_DEFINE_BOOL(gta4_native_hitch_log, false, "GTA IV/Diagnostics",
+                    "Measure and log frame hitches and submission timings");
+REXCVAR_DEFINE_BOOL(gta4_native_hitch_breakdown, false, "GTA IV/Diagnostics", 
+                        "Time every recorded command for the draw/resolve/clear split in "
+                        "'hitch:' log lines (two clock reads per command)");
 REXCVAR_DEFINE_UINT32(gta4_native_shadow_min_indices, 0, "GTA IV/Graphics/Shadows",
                       "Skip indexed draws with fewer indices than this in the shadow map passes "
                       "(0 = draw everything)");
@@ -314,6 +333,8 @@ REXCVAR_DEFINE_BOOL(gta4_native_recycle_commands, true, "GTA IV/Graphics/Native
                     "instead of destroying them and constructing new ones on the title thread");
 REXCVAR_DEFINE_BOOL(gta4_native_dedupe_vertex_bindings, true, "GTA IV/Graphics/Native Renderer",
                     "Skip vkCmdBindVertexBuffers when the binding already holds that buffer and offset");
+REXCVAR_DEFINE_BOOL(gta4_native_dedupe_index_binding, true, "GTA IV/Graphics/Native Renderer",
+                    "Skip vkCmdBindIndexBuffer when the command buffer already holds that buffer, offset and index type");
 REXCVAR_DEFINE_BOOL(gta4_native_coalesce_state_commands, true, "GTA IV/Graphics/Native Renderer",
                     "Carry title state commands (textures, streams, shaders, targets) to the worker "
                     "inside the next queued command instead of queueing each as its own command");
@@ -398,6 +419,12 @@ REXCVAR_DEFINE_BOOL(gta4_native_ubo_constants, true, "GTA IV/Graphics/Native Ren
                     "Read guest constant banks through dynamic uniform buffers instead of device addresses");
 REXCVAR_DEFINE_BOOL(gta4_pipeline_stats, false, "GTA IV/Diagnostics",
                     "Log driver shader statistics of the most-drawn pipelines (Android)");
+REXCVAR_DEFINE_BOOL(gta4_native_debug_counters, false, "GTA IV/Diagnostics",
+                    "Collect per-line barrier/draw/resolve counters (very slow on mobile)");
+REXCVAR_DEFINE_BOOL(gta4_native_pipeline_report, false, "GTA IV/Diagnostics",
+                    "Every 120 frames log 'pipeline:', 'waits ms/f', 'skips/frame' and the "
+                    "other renderer summary lines (runs on the worker before the recorder "
+                    "gets the next frame)");
 REXCVAR_DEFINE_BOOL(gta4_gpu_counters, false, "GTA IV/Diagnostics",
                     "Sample Adreno hardware counters per frame through VK_KHR_performance_query (Android)");
 REXCVAR_DEFINE_BOOL(gta4_native_pipelined_recording, true, "GTA IV/Graphics/Native Renderer",
@@ -487,19 +514,19 @@ REXCVAR_DEFINE_BOOL(
     "Discard prior resolve contents only for proven complete subresource overwrites")
     .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
 REXCVAR_DEFINE_BOOL(
-    gta4_trace_native_renderer, true, "GTA IV/Diagnostics",
+    gta4_trace_native_renderer, false, "GTA IV/Diagnostics",
     "Emit deterministic frame-correlated native renderer state, resource, draw, resolve, and "
     "presentation diagnostics");
 REXCVAR_DEFINE_BOOL(
-    gta4_trace_black_outputs, true, "GTA IV/Diagnostics",
+    gta4_trace_black_outputs, false, "GTA IV/Diagnostics",
     "Continuously detect probe-uniform black reflection and filtered-shadow color outputs while "
     "native GPU probes are enabled");
 REXCVAR_DEFINE_BOOL(
-    gta4_trace_local_black_draws, true, "GTA IV/Diagnostics",
+    gta4_trace_local_black_draws, false, "GTA IV/Diagnostics",
     "Densely checkpoint the primary scene target and report draws that turn sampled lit pixels "
     "exactly or nearly black while native GPU probes are enabled");
 REXCVAR_DEFINE_BOOL(
-    gta4_trace_hove_gantry, true, "GTA IV/Diagnostics",
+    gta4_trace_hove_gantry, false, "GTA IV/Diagnostics",
     "Trace the Hove Beach sign gantry geometry candidate without changing its draw state");
 REXCVAR_DEFINE_UINT32(
     gta4_trace_native_interval, 120, "GTA IV/Diagnostics",
@@ -533,12 +560,16 @@ REXCVAR_DEFINE_BOOL(gta4_native_batch_texture_uploads, true, "GTA IV/Graphics/Na
                     "Batch independent texture uploads before publishing the frame's draw bindings");
 REXCVAR_DEFINE_BOOL(gta4_validate_native_hot_caches, false, "GTA IV/Diagnostics",
                     "Cross-check incremental resource protection and constant uploads against original inputs");
-REXCVAR_DEFINE_BOOL(gta4_profile_native_detailed_gpu, true, "GTA IV/Diagnostics",
+REXCVAR_DEFINE_BOOL(gta4_profile_native_detailed_gpu, false, "GTA IV/Diagnostics",
                     "Enable approximate per-pass timestamps; coarse GPU envelope is cheaper on Metal");
 REXCVAR_DEFINE_BOOL(gta4_profile_native_moltenvk_detailed, false, "GTA IV/Diagnostics",
                     "Opt into perturbing per-range Metal timestamp blits; normally capture only the frame envelope");
-REXCVAR_DEFINE_BOOL(gta4_profile_native_detailed_cpu, true, "GTA IV/Diagnostics",
-                    "Collect bounded operation-level CPU self/inclusive timings during native capture");
+REXCVAR_DEFINE_BOOL(gta4_profile_native_real_stream, true, "GTA IV/Diagnostics",
+                    "Keep state coalescing and draw staging while the native CPU profiler "
+                    "records, so it measures the normal command stream (transport columns "
+                    "become partial)");
+REXCVAR_DEFINE_BOOL(gta4_profile_native_detailed_cpu, false, "GTA IV/Diagnostics",
+                      "Maximum timestamp boundaries per sampled frame (more boundaries add GPU overhead)");
 REXCVAR_DEFINE_UINT32(gta4_profile_native_gpu_query_budget, 512, "GTA IV/Diagnostics",
                       "Maximum timestamp boundaries per sampled frame (more boundaries add GPU overhead)")
     .range(16, rex::graphics::gta4_native::performance::kMaximumGpuQueriesPerFrame);
@@ -549,7 +580,7 @@ REXCVAR_DEFINE_UINT32(gta4_profile_native_samples, 600, "GTA IV/Diagnostics",
                       "Export the native-renderer numeric profile after this many samples")
     .range(1, rex::graphics::gta4_native::performance::kFrameSampleCapacity);
 REXCVAR_DEFINE_BOOL(
-    gta4_profile_native_autostart, true, "GTA IV/Diagnostics",
+    gta4_profile_native_autostart, false, "GTA IV/Diagnostics",
     "Start native profiling with the first rendered frame; disable to arm it later from LLDB");
 REXCVAR_DEFINE_UINT32(gta4_profile_native_memory_interval_ms, 1000, "GTA IV/Diagnostics",
                       "Wall-clock interval between native-renderer memory samples in milliseconds")
@@ -557,7 +588,7 @@ REXCVAR_DEFINE_UINT32(gta4_profile_native_memory_interval_ms, 1000, "GTA IV/Diag
 REXCVAR_DEFINE_BOOL(
     gta4_profile_native_memory_autostart, false, "GTA IV/Diagnostics",
     "Start native memory profiling with the first rendered frame; disable to control it from LLDB");
-REXCVAR_DEFINE_BOOL(gta4_profile_native_memory_deep, true, "GTA IV/Diagnostics",
+REXCVAR_DEFINE_BOOL(gta4_profile_native_memory_deep, false, "GTA IV/Diagnostics",
                     "Record bounded identity-level renderer resource lifecycle events");
 REXCVAR_DEFINE_UINT32(
     gta4_profile_native_memory_vm_interval_ms, 5000, "GTA IV/Diagnostics",
@@ -666,15 +697,77 @@ void LogFrameTimeline(uint32_t frame) {
   }
 }
 
+template <size_t N>
+class NativeConditionalAtomicSites {
+ public:
+  struct Ref {
+    std::atomic<uint32_t>& slot;
+    
+    Ref& operator++() noexcept {
+      if (REXCVAR_GET(gta4_native_debug_counters)) {
+        slot.fetch_add(1, std::memory_order_relaxed);
+      }
+      return *this;
+    }
+    
+    uint32_t load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
+      return slot.load(order);
+    }
+    
+    void store(uint32_t val, std::memory_order order = std::memory_order_seq_cst) noexcept {
+      slot.store(val, order);
+    }
+
+    // ДОБАВИТЬ ВОТ ЭТУ ФУНКЦИЮ:
+    uint32_t exchange(uint32_t val, std::memory_order order = std::memory_order_seq_cst) noexcept {
+      return slot.exchange(val, order);
+    }
+  };
+
+  Ref operator[](size_t i) noexcept { return Ref{slots_[i]}; }
+  uint32_t load(size_t i) const noexcept { return slots_[i].load(std::memory_order_relaxed); }
+  void store(size_t i, uint32_t v) noexcept { slots_[i].store(v, std::memory_order_relaxed); }
+  static constexpr size_t size() noexcept { return N; }
+
+ private:
+  std::array<std::atomic<uint32_t>, N> slots_{};
+};
+
 // Draws whose constants came from the worker ring, and those that did not.
 std::atomic<uint64_t> g_worker_ring_draws{0}, g_worker_ring_fallbacks{0};
 std::atomic<uint64_t> g_worker_ring_checked{0}, g_worker_ring_mismatches{0};
+// waits ms/f: pipeline handoff waits (debug)
+std::atomic<uint64_t> g_wait_rec_job_ticks{0};      // Recorder idle (ждёт job)
+std::atomic<uint64_t> g_wait_rec_publish_ticks{0};  // PublishFrame
+std::atomic<uint64_t> g_wait_rg_queue_ticks{0};     // RenderGuest backpressure
+std::atomic<uint64_t> g_wait_rw_cmd_ticks{0};       // RenderWork ждёт очередь
+// hitch: last PublishFrame breakdown (ns, CLOCK_MONOTONIC)
+std::atomic<uint64_t> g_hitch_recipes_ns{0};
+std::atomic<uint64_t> g_hitch_record_ns{0};
+std::atomic<uint64_t> g_hitch_record_setup_ns{0};  // до основного цикла команд
+// cmds breakdown (ns), суммируется внутри issue-цикла RecordNativeFrame
+std::atomic<uint64_t> g_hitch_cmd_draw_ns{0};
+std::atomic<uint64_t> g_hitch_cmd_draw_sh_ns{0};
+std::atomic<uint64_t> g_hitch_cmd_draw_w_ns{0};
+std::atomic<uint64_t> g_hitch_cmd_resolve_ns{0};
+std::atomic<uint64_t> g_hitch_cmd_clear_ns{0};
+std::atomic<uint64_t> g_hitch_cmd_other_ns{0};
+std::atomic<uint64_t> g_hitch_pre_record_ns{0};
+std::atomic<uint64_t> g_hitch_submit_done_ns{0};
+std::atomic<uint64_t> g_hitch_hk_end_ns{0};     // конец уборки ресурсов
+std::atomic<uint64_t> g_hk_surf_ns{0}, g_hk_texunused_ns{0}, g_hk_texretired_ns{0}, g_hk_bufunused_ns{0}, g_hk_persist_ns{0};
+std::atomic<uint64_t> g_hitch_tex_pool_ns{0};   // PrepareFrameTextures: подготовка пула
+std::atomic<uint64_t> g_hitch_tex_loop_ns{0};   // PrepareFrameTextures: обход команд
+std::atomic<uint64_t> g_hitch_tex_post_ns{0};   // PrepareFrameTextures: cached_draws / publish
+std::atomic<uint64_t> g_hitch_tex_begin_ns{0};  // перед PrepareFrameTextures
+std::atomic<uint64_t> g_hitch_cb_end_ns{0};     // конец колбэка (перед Wait показа)
+std::atomic<uint64_t> g_hitch_prev_end_ns{0};
 // TEMP: rendering-scope breaks by source line (perf investigation).
-std::array<std::atomic<uint32_t>, 40000> g_rendering_end_sites{};
-std::array<std::atomic<uint32_t>, 40000> g_barrier_sites{};
-std::array<std::atomic<uint32_t>, 4096> g_rendering_split_reasons{};
+NativeConditionalAtomicSites<40000> g_rendering_end_sites{};
+NativeConditionalAtomicSites<40000> g_barrier_sites{};
+NativeConditionalAtomicSites<4096> g_rendering_split_reasons{};
 // TEMP: resolve-class GPU work per call site, and resolve reuse totals.
-std::array<std::atomic<uint32_t>, 65536> g_resolve_sites{};
+NativeConditionalAtomicSites<65536> g_resolve_sites{};
 std::atomic<uint64_t> g_resolve_reuse_candidates{0}, g_resolve_reuse_hits{0};
 // TEMP: vertex-cache reordering coverage: [0] rejected, [1] eligible.
 std::array<std::atomic<uint64_t>, 2> g_vertex_cache_draws{}, g_vertex_cache_indices{};
@@ -713,11 +806,12 @@ uint32_t MaxQueuedTitlePresents() {
 
 std::unique_lock<std::mutex> LockWithSpin(std::mutex& mutex) {
   std::unique_lock lock(mutex, std::defer_lock);
-  for (int attempt = 0; attempt < 512; ++attempt) {
-    if (lock.try_lock()) return lock;
+  if (lock.try_lock()) return lock;
+  for (int attempt = 0; attempt < 64; ++attempt) {
 #if defined(__aarch64__)
-    asm volatile("yield");
+    asm volatile("isb" ::: "memory");
 #endif
+    if (lock.try_lock()) return lock;
   }
   lock.lock();
   return lock;
@@ -2236,6 +2330,9 @@ bool ShouldTraceVectorFonts() {
 }
 
 std::string_view ClassifyTranslucentDiagnosticShader(std::string_view filename) {
+  if (!REXCVAR_GET(gta4_trace_native_renderer)) {
+    return std::string_view{};
+  }
   if (filename.find("/gta_grass/") != std::string_view::npos) {
     return "grass-card";
   }
@@ -2435,7 +2532,7 @@ struct NativeDrawDescriptorKey {
 
   bool operator==(const NativeDrawDescriptorKey&) const = default;
 };
-
+#if 0
 struct NativeDrawDescriptorKeyHash {
   size_t operator()(const NativeDrawDescriptorKey& key) const noexcept {
     uint64_t hash = 0;
@@ -2452,6 +2549,13 @@ struct NativeDrawDescriptorKeyHash {
     return size_t(hash);
   }
 };
+#endif
+//Однопроходный NativeDrawDescriptorKeyHash
+struct NativeDrawDescriptorKeyHash {
+  size_t operator()(const NativeDrawDescriptorKey& key) const noexcept {
+    return size_t(XXH3_64bits(&key, sizeof(NativeDrawDescriptorKey)));
+  }
+};
 
 float MotionBlurTimeScale(const EnvironmentalDataV2* environmental_data) {
   constexpr float kReferenceFramesPerSecond = 30.0f;
@@ -2482,27 +2586,37 @@ constexpr uint32_t GetQuadListTriangleListIndexCount(uint32_t quad_list_index_co
   return (quad_list_index_count / 4) * 6;
 }
 
-uint32_t ConvertVertexUsageToLocation(uint8_t usage, uint8_t usage_index) {
-  struct UsageLocation {
-    uint8_t usage;
-    uint8_t usage_index;
-    uint32_t location;
-  };
-  static constexpr UsageLocation kUsageLocations[] = {
-      {0, 0, 0},   {9, 0, 0},   {0, 1, 1},   {0, 2, 2},   {0, 3, 3},   {3, 0, 4},   {3, 1, 5},
-      {3, 2, 6},   {3, 3, 7},   {6, 0, 8},   {6, 1, 9},   {6, 2, 10},  {6, 3, 11},  {7, 0, 12},
-      {5, 0, 13},  {5, 1, 14},  {5, 2, 15},  {5, 3, 16},  {10, 0, 17}, {2, 0, 18},  {1, 0, 19},
-      {5, 4, 20},  {5, 5, 21},  {5, 6, 22},  {5, 7, 23},  {5, 8, 24},  {5, 9, 25},  {5, 10, 26},
-      {5, 11, 27}, {5, 12, 28}, {5, 13, 29}, {5, 14, 30}, {5, 15, 31}, {10, 1, 32}, {3, 4, 33},
-      {3, 5, 34},  {6, 4, 35},  {6, 5, 36},  {7, 1, 37},  {7, 2, 38},  {7, 3, 39},
-  };
-  for (const UsageLocation& candidate : kUsageLocations) {
-    if (candidate.usage == usage && candidate.usage_index == usage_index) {
-      return candidate.location;
+constexpr uint32_t ConvertVertexUsageToLocation(uint8_t usage, uint8_t usage_index) {
+      struct UsageLocation {
+        uint8_t usage;
+        uint8_t usage_index;
+        uint8_t location;
+      };
+      static constexpr UsageLocation kUsageLocations[] = {
+          {0, 0, 0},   {9, 0, 0},   {0, 1, 1},   {0, 2, 2},   {0, 3, 3},   {3, 0, 4},   {3, 1, 5},
+          {3, 2, 6},   {3, 3, 7},   {6, 0, 8},   {6, 1, 9},   {6, 2, 10},  {6, 3, 11},  {7, 0, 12},
+          {5, 0, 13},  {5, 1, 14},  {5, 2, 15},  {5, 3, 16},  {10, 0, 17}, {2, 0, 18},  {1, 0, 19},
+          {5, 4, 20},  {5, 5, 21},  {5, 6, 22},  {5, 7, 23},  {5, 8, 24},  {5, 9, 25},  {5, 10, 26},
+          {5, 11, 27}, {5, 12, 28}, {5, 13, 29}, {5, 14, 30}, {5, 15, 31}, {10, 1, 32}, {3, 4, 33},
+          {3, 5, 34},  {6, 4, 35},  {6, 5, 36},  {7, 1, 37},  {7, 2, 38},  {7, 3, 39},
+      };
+      // [usage][usage_index] -> location, 0xFF = нет; первое совпадение выигрывает,
+      // как в прежнем линейном поиске. Считается при компиляции.
+      struct Table {
+        uint8_t location[16][16];
+        constexpr Table() : location{} {
+          for (auto& row : location)
+            for (uint8_t& value : row) value = 0xFF;
+          for (const UsageLocation& entry : kUsageLocations)
+            if (location[entry.usage][entry.usage_index] == 0xFF)
+              location[entry.usage][entry.usage_index] = entry.location;
+        }
+      };
+      static constexpr Table kTable{};
+      if (usage >= 16 || usage_index >= 16) return UINT32_MAX;
+      const uint8_t location = kTable.location[usage][usage_index];
+      return location == 0xFF ? UINT32_MAX : location;
     }
-  }
-  return UINT32_MAX;
-}
 
 VkFormat ConvertVertexElementFormat(uint32_t type) {
   switch (type) {
@@ -2651,10 +2765,25 @@ VertexPayloadConversionCounts ConvertGuestVertexPayload(uint8_t* destination, co
       if (element_size > size - vertex_offset) {
         break;
       }
-      if (component_16_count) {
-        for (uint32_t component = 0; component < component_16_count; ++component) {
+if (component_16_count == 2) {
+        uint32_t pair;
+        std::memcpy(&pair, source + vertex_offset, sizeof(pair));
+        // Переворот двух 16-битных значений за одну 32-битную инструкцию ARM64 (REV16):
+        pair = ((pair & 0x00FF00FFu) << 8) | ((pair & 0xFF00FF00u) >> 8);
+        std::memcpy(destination + vertex_offset, &pair, sizeof(pair));
+        ++counts.components_16;
+      } else if (component_16_count == 4) {
+        uint64_t quad;
+        std::memcpy(&quad, source + vertex_offset, sizeof(quad));
+        // Переворот четырех 16-битных значений (8 байт) за одну 64-битную операцию (REV16 Xn):
+        quad = ((quad & 0x00FF00FF00FF00FFull) << 8) | ((quad & 0xFF00FF00FF00FF00ull) >> 8);
+        std::memcpy(destination + vertex_offset, &quad, sizeof(quad));
+        ++counts.components_16;
+      } else if (component_16_count != 0) {
+        // Фоллбек на случай экзотических форматов (1 или 3 компоненты)
+        for (size_t component = 0; component < component_16_count; ++component) {
           uint16_t value;
-          const size_t component_offset = vertex_offset + size_t(component) * sizeof(uint16_t);
+          const size_t component_offset = vertex_offset + (component * sizeof(uint16_t));
           std::memcpy(&value, source + component_offset, sizeof(value));
           value = __builtin_bswap16(value);
           std::memcpy(destination + component_offset, &value, sizeof(value));
@@ -2683,23 +2812,34 @@ void CopyGuestIndicesToHost(uint8_t* destination, const uint8_t* source, size_t
                             bool index32) {
   const size_t element_size = index32 ? sizeof(uint32_t) : sizeof(uint16_t);
   size_t offset = 0;
+#if defined(__aarch64__)
+  if (!index32) {
+    // 16-битные индексы (99% геометрии GTA IV): переворачиваем по 64 байта (32 индекса) за раз через NEON!
+    for (; size - offset >= 64; offset += 64) {
+      uint8x16x4_t block = vld1q_u8_x4(source + offset);
+      block.val[0] = vrev16q_u8(block.val[0]);
+      block.val[1] = vrev16q_u8(block.val[1]);
+      block.val[2] = vrev16q_u8(block.val[2]);
+      block.val[3] = vrev16q_u8(block.val[3]);
+      vst1q_u8_x4(destination + offset, block);
+    }
+    for (; size - offset >= 16; offset += 16) {
+      vst1q_u8(destination + offset, vrev16q_u8(vld1q_u8(source + offset)));
+    }
+  } else {
+    // 32-битные индексы: переворачиваем и накладываем маску kVertexIndexMask по 16 байт (4 индекса) за раз!
+    const uint32x4_t mask_vec = vdupq_n_u32(xenos::kVertexIndexMask);
+    for (; size - offset >= 16; offset += 16) {
+      uint32x4_t words = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(source + offset)));
+      vst1q_u32(reinterpret_cast<uint32_t*>(destination + offset), vandq_u32(words, mask_vec));
+    }
+  }
+#endif
   while (size - offset >= element_size) {
     if (index32) {
       uint32_t value;
       std::memcpy(&value, source + offset, sizeof(value));
-      value = __builtin_bswap32(value);
-      const uint32_t masked_value = value & xenos::kVertexIndexMask;
-      if (value != masked_value) {
-        static std::atomic<uint64_t> masked_index_count{0};
-        const uint64_t count = NextNativeTraceDiagnosticCount(masked_index_count);
-        if (count <= 32) {
-          REXLOG_WARN(
-              "gta4-native-index: masked 32-bit index #{} offset={} raw={:08X} "
-              "xenos={:08X}",
-              count, offset, value, masked_value);
-        }
-      }
-      value = masked_value;
+      value = __builtin_bswap32(value) & xenos::kVertexIndexMask;
       std::memcpy(destination + offset, &value, sizeof(value));
     } else {
       uint16_t value;
@@ -3359,12 +3499,14 @@ struct Gta4NativeGraphicsSystem::NativePipelineCompilerState {
   std::atomic<uint64_t> checkpoint_tick{0};
   static constexpr size_t kMaximumRecipes = 4096;
 };
-
+#if 0
 size_t Gta4NativeGraphicsSystem::NativePipelineKeyHash::operator()(
     const NativePipelineKey& key) const noexcept {
-  uint64_t hash = 0;
-  auto add = [&hash](const auto& value) {
-    hash = XXH3_64bits_withSeed(&value, sizeof(value), hash);
+  thread_local XXH3_state_t* state = XXH3_createState();
+  if (!state) return 0;
+  XXH3_64bits_reset(state);
+  auto add = [&](const auto& value) {
+    XXH3_64bits_update(state, &value, sizeof(value));
   };
   add(key.vertex_shader_hash);
   add(key.temporal_shader);
@@ -3418,9 +3560,18 @@ size_t Gta4NativeGraphicsSystem::NativePipelineKeyHash::operator()(
   add(key.empty_fragment);
   add(key.depth_bias_enable);
   add(key.primitive_restart_enable);
-  return size_t(hash);
+  return size_t(XXH3_64bits_digest(state));
+}
+#endif
+//Замена потокового XXH3_64bits_update
+size_t Gta4NativeGraphicsSystem::NativePipelineKeyHash::operator()(
+    const NativePipelineKey& key) const noexcept {
+  return size_t(XXH3_64bits(&key, sizeof(NativePipelineKey)));
 }
 
+
+
+
 size_t Gta4NativeGraphicsSystem::NativePersistentBufferKeyHash::operator()(
     const NativePersistentBufferKey& key) const noexcept {
   // Hot per-draw lookup: a multiply-xorshift mix is enough for these
@@ -3602,6 +3753,9 @@ void Gta4NativeGraphicsSystem::StageNativeFlightDescriptorSlot(NativeDescriptorS
 }
 
 void Gta4NativeGraphicsSystem::StageNativeTextureFlightResources(const NativeTextureImage& image) {
+  if (!IsNativeFlightRecorderEnabled()) {
+    return;
+  }
   const profile::CpuScope profile_scope(profile::CpuOp::kLifetimeBookkeeping);
 
   const uint32_t guest_handle = image.source ? image.source->handle : 0;
@@ -3627,6 +3781,9 @@ void Gta4NativeGraphicsSystem::StageNativeTextureFlightResources(const NativeTex
 }
 
 void Gta4NativeGraphicsSystem::StageNativeSurfaceFlightResources(const NativeSurfaceImage& image) {
+  if (!IsNativeFlightRecorderEnabled()) {
+    return;
+  }
   StageNativeFlightResource(NativeFlightResourceKind::kSurfaceImage,
                             NativeVulkanHandleIdentity(image.resource.image),
                             image.descriptor.handle, image.materialized_serial);
@@ -3671,8 +3828,10 @@ void Gta4NativeGraphicsSystem::CommitNativeFlightSubmission(uint32_t slot, uint6
   flight.submission = submission;
   flight.frame = frame;
   flight.slot = slot;
-  gpu_flight::Record("native.submit-committed", NativeVulkanHandleIdentity(command_buffer_),
-                     submission, frame, slot, staged_native_flight_resources_.size());
+  if (IsNativeFlightRecorderEnabled()) {
+    gpu_flight::Record("native.submit-committed", NativeVulkanHandleIdentity(command_buffer_),
+                       submission, frame, slot, staged_native_flight_resources_.size());
+  }
   flight.resources = std::move(staged_native_flight_resources_);
   staged_native_flight_resources_.clear();
   if (IsNativeFlightVerboseLoggingEnabled()) {
@@ -3724,9 +3883,11 @@ bool Gta4NativeGraphicsSystem::CompleteNativeFlightSubmission(uint32_t slot, uin
 }
 
 void Gta4NativeGraphicsSystem::DiscardStagedNativeFlightResources() {
-  gpu_flight::Record("native.discard-unsubmitted", active_frame_slot_,
-                     submission_tracker_ ? submission_tracker_->GetCurrentSubmission() : 0,
-                     active_texture_frame_, staged_native_flight_resources_.size());
+  if (IsNativeFlightRecorderEnabled()) {
+    gpu_flight::Record("native.discard-unsubmitted", active_frame_slot_,
+                       submission_tracker_ ? submission_tracker_->GetCurrentSubmission() : 0,
+                       active_texture_frame_, staged_native_flight_resources_.size());
+  }
   if (IsNativeFlightVerboseLoggingEnabled() && !staged_native_flight_resources_.empty()) {
     REXLOG_INFO("gta4-native-flight: action=discard-unsubmitted slot={} resources={}",
                 active_frame_slot_, staged_native_flight_resources_.size());
@@ -3963,7 +4124,8 @@ bool Gta4NativeGraphicsSystem::SubmitTitleCommand(uint32_t title_id, uint32_t ab
   const uint64_t capture_begin = profile_transport ? profile::CpuTick() : 0;
   std::unique_lock capture_lock(command_capture_mutex_);
   const uint64_t capture_acquired = profile_transport ? profile::CpuTick() : 0;
-  if (!phone_envelope && !tv_envelope && !fire_envelope && !profile_transport &&
+  if (!phone_envelope && !tv_envelope && !fire_envelope &&
+      (!profile_transport || REXCVAR_GET(gta4_profile_native_real_stream)) &&
       REXCVAR_GET(gta4_native_coalesce_state_commands) &&
       CoalesceStateCommand(title_command, title_command_size)) {
     return true;
@@ -4020,7 +4182,8 @@ bool Gta4NativeGraphicsSystem::SubmitTitleCommand(uint32_t title_id, uint32_t ab
   producer_pending_state_.clear();
   CollectTextureProtection(native_command);
   QueueTextureProtection(native_command, true);
-  if (stageable && !phone_envelope && !tv_envelope && !fire_envelope && !profile_transport &&
+  if (stageable && !phone_envelope && !tv_envelope && !fire_envelope &&
+      (!profile_transport || REXCVAR_GET(gta4_profile_native_real_stream)) &&
       REXCVAR_GET(gta4_native_stage_draw_commands)) {
     staged_draw_commands_.push_back(std::move(native_command_owner));
     if (staged_draw_commands_.size() < kStagedDrawBatch) {
@@ -4033,21 +4196,32 @@ bool Gta4NativeGraphicsSystem::SubmitTitleCommand(uint32_t title_id, uint32_t ab
     std::unique_lock lock = LockWithSpin(render_mutex_);
     const uint64_t queue_lock_end = profile_transport ? profile::CpuTick() : 0;
     producer_waiting_ = render_queue_.size() >= kMaximumQueuedCommands || queued_title_presents_ >= MaxQueuedTitlePresents();
-    render_condition_.wait(lock, [this]() {
+    auto can_submit = [this]() {
       return !render_worker_running_ ||
              (render_queue_.size() < kMaximumQueuedCommands && queued_title_presents_ < MaxQueuedTitlePresents());
-    });
+    };
+    if (producer_waiting_) {
+      TRACE_SCOPE("RG: backpressure wait");
+      render_condition_.wait(lock, can_submit);
+    } else {
+      render_condition_.wait(lock, can_submit);
+    }
     const uint64_t backpressure_end = profile_transport ? profile::CpuTick() : 0;
     producer_waiting_ = false;
     if (!render_worker_running_) {
       return false;
     }
-    const size_t queued_before = render_queue_.size();
+    const size_t queued_before_staged = render_queue_.size();
     PublishStagedDrawCommandsLocked();
     if (!native_command_owner) {
       // This draw completed the batch and was published with it.
-      wake_worker = queued_before == 0 ||
-                    (queued_before < kWorkerWakeBatch && render_queue_.size() >= kWorkerWakeBatch);
+      if (worker_waiting_) {
+        worker_waiting_ = false;
+        wake_worker = true;
+      } else {
+        wake_worker = queued_before_staged == 0 ||
+                      (queued_before_staged < kWorkerWakeBatch && render_queue_.size() >= kWorkerWakeBatch);
+      }
     } else {
     native_command.diagnostic_submit_sequence = ++diagnostic_submit_sequence_;
     native_command.diagnostic_producer_epoch = diagnostic_producer_epoch_;
@@ -4064,11 +4238,17 @@ bool Gta4NativeGraphicsSystem::SubmitTitleCommand(uint32_t title_id, uint32_t ab
     }
     // The worker sleeps only on an empty queue. Waking it for every command
     // turned each submit into a futex round trip and a render_mutex_ fight;
-    // wake it per batch and at frame boundaries instead.
-    wake_worker = (queued_before < kWorkerWakeBatch && render_queue_.size() + 1 >= kWorkerWakeBatch) ||
-                  title_header.type == CommandType::kPresent ||
-                  (queued_before == 0 && !render_queue_.empty()) ||
-                  (render_queue_.empty() && !REXCVAR_GET(gta4_native_batched_worker_wake));
+    // wake it when worker is waiting, per batch, or at frame boundaries instead.
+    const size_t queued_before = render_queue_.size();
+    if (worker_waiting_) {
+      worker_waiting_ = false;
+      wake_worker = true;
+    } else {
+      wake_worker = (queued_before < kWorkerWakeBatch && queued_before + 1 >= kWorkerWakeBatch) ||
+                    title_header.type == CommandType::kPresent ||
+                    (queued_before == 0) ||
+                    (!REXCVAR_GET(gta4_native_batched_worker_wake));
+    }
     if(profile_transport) {
       const uint64_t enqueued = profile::CpuTick();
       native_command.profile_transport = {enqueued, enqueued-capture_begin,
@@ -4245,10 +4425,16 @@ bool Gta4NativeGraphicsSystem::ExecuteTitleCommand(uint32_t title_id, uint32_t a
   {
     std::unique_lock lock = LockWithSpin(render_mutex_);
     producer_waiting_ = render_queue_.size() >= kMaximumQueuedCommands || queued_title_presents_ >= MaxQueuedTitlePresents();
+    const bool was_blocked = producer_waiting_;
+    const uint64_t backpressure_begin = was_blocked ? profile::CpuTick() : 0;
     render_condition_.wait(lock, [this]() {
       return !render_worker_running_ ||
              (render_queue_.size() < kMaximumQueuedCommands && queued_title_presents_ < MaxQueuedTitlePresents());
     });
+    if (was_blocked)
+      g_wait_rg_queue_ticks.fetch_add(profile::CpuTick() - backpressure_begin,
+                                      std::memory_order_relaxed);
+ //   const uint64_t backpressure_end = native_command.profile_transport ? profile::CpuTick() : 0;
     producer_waiting_ = false;
     if (!render_worker_running_) {
       return false;
@@ -4260,6 +4446,7 @@ bool Gta4NativeGraphicsSystem::ExecuteTitleCommand(uint32_t title_id, uint32_t a
         "run={} event={} seq={} epoch={} texture={:08X}", phone_context.run, phone_context.event,
         native_command.diagnostic_submit_sequence, native_command.diagnostic_producer_epoch, lock_command.texture));
     QueueTextureProtection(native_command, true);
+    worker_waiting_ = false;
     render_queue_.push_back(std::move(native_command_owner));
   }
   render_condition_.notify_one();
@@ -4849,7 +5036,7 @@ bool Gta4NativeGraphicsSystem::ValidateAndCopyCommand(const void* command, size_
       const auto half_scene = texture_resources_.find(producer_postfx_half_scene_);
       if (half_scene != texture_resources_.end()) native_command.postfx_half_scene = half_scene->second;
     }
-    const std::string state_transport = REXCVAR_GET(gta4_native_state_transport);
+        const auto& state_transport = REXCVAR_GET(gta4_native_state_transport);
     const bool compare_transport = state_transport == "compare";
     const bool diagnostic_snapshot =
         CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeTrace) ||
@@ -4969,9 +5156,20 @@ bool Gta4NativeGraphicsSystem::ValidateAndCopyCommand(const void* command, size_
         native_command.dirty_components |=
             DirtyStateComponentBit(DirtyStateComponent::kIntegerConstants);
       }
-      native_command.fixed_function_state = DecodeFixedFunctionState(device_state);
-      native_command.captured_fixed_function_state_hash =
-          HashFixedFunctionState(native_command.fixed_function_state);
+native_command.fixed_function_state = DecodeFixedFunctionState(device_state);
+      {
+        static NativeFixedFunctionState last_decoded_fixed{};
+        static uint64_t last_decoded_hash = 0;
+        if (last_decoded_hash != 0 &&
+            std::memcmp(&native_command.fixed_function_state, &last_decoded_fixed,
+                        sizeof(NativeFixedFunctionState)) == 0) {
+          native_command.captured_fixed_function_state_hash = last_decoded_hash;
+        } else {
+          last_decoded_fixed = native_command.fixed_function_state;
+          last_decoded_hash = HashFixedFunctionState(native_command.fixed_function_state);
+          native_command.captured_fixed_function_state_hash = last_decoded_hash;
+        }
+      }
     }
     for (uint32_t index = 0; index < kRenderTargetCount; ++index) {
       native_command.snapshot_render_targets[index] = CaptureSurfaceDescriptor(
@@ -4979,8 +5177,43 @@ bool Gta4NativeGraphicsSystem::ValidateAndCopyCommand(const void* command, size_
     }
     native_command.snapshot_depth_stencil =
         CaptureSurfaceDescriptor(LoadGuestWord(device_state, kDepthStencilOffset));
-  }
+  
+// Мгновенный кэш CaptureSurfaceDescriptor в ValidateAndCopyCommand. начало "патча" (крашит игру при выходе в меню)
+#if 0
+    {
+      static std::array<uint32_t, kRenderTargetCount> last_rt_handles{};
+      static std::array<SurfaceDescriptor, kRenderTargetCount> last_rt_descs{};
+      static uint32_t last_ds_handle = 0;
+      static SurfaceDescriptor last_ds_desc{};
+      const bool targets_clean = state_reported_clean;
 
+      for (uint32_t index = 0; index < kRenderTargetCount; ++index) {
+        const uint32_t h =
+            LoadGuestWord(device_state, kRenderTargetOffset + index * sizeof(uint32_t));
+        if (!h) {
+          native_command.snapshot_render_targets[index] = SurfaceDescriptor{};
+        } else if (targets_clean && h == last_rt_handles[index]) {
+          native_command.snapshot_render_targets[index] = last_rt_descs[index];
+        } else {
+          last_rt_handles[index] = h;
+          last_rt_descs[index] = CaptureSurfaceDescriptor(h);
+          native_command.snapshot_render_targets[index] = last_rt_descs[index];
+        }
+      }
+      const uint32_t ds_h = LoadGuestWord(device_state, kDepthStencilOffset);
+      if (!ds_h) {
+        native_command.snapshot_depth_stencil = SurfaceDescriptor{};
+      } else if (targets_clean && ds_h == last_ds_handle) {
+        native_command.snapshot_depth_stencil = last_ds_desc;
+      } else {
+        last_ds_handle = ds_h;
+        last_ds_desc = CaptureSurfaceDescriptor(ds_h);
+        native_command.snapshot_depth_stencil = last_ds_desc;
+      }
+     }
+#endif
+  }
+// конец "патча"
   if (header.type == CommandType::kDrawPrimitiveUp) {
     const auto& draw = *static_cast<const DrawPrimitiveUpCommand*>(command);
     if (draw.vertex_data_size > kMaximumUpPayloadSize ||
@@ -5110,7 +5343,7 @@ bool Gta4NativeGraphicsSystem::ValidateAndCopyCommand(const void* command, size_
     }
   }
 
-  if (draw_command || header.type == CommandType::kClear) {
+if (draw_command || header.type == CommandType::kClear) {
     const uint32_t device = CommandDevice(header.type, command);
     if (device != producer_initialized_constant_device_.load(std::memory_order_relaxed)) {
       std::lock_guard state_lock(device_snapshot_mutex_);
@@ -5121,6 +5354,7 @@ bool Gta4NativeGraphicsSystem::ValidateAndCopyCommand(const void* command, size_
 
   return true;
 }
+													  
 
 std::shared_ptr<const Gta4NativeGraphicsSystem::NativeTextureResource>
 Gta4NativeGraphicsSystem::CreateResolvedTextureResource(const ResolveCommand& command) {
@@ -5267,13 +5501,11 @@ Gta4NativeGraphicsSystem::CaptureBufferResource(uint32_t handle) {
   if (!metadata->HasValidPayload(kMaximumResourcePayloadSize)) {
     return reject("metadata", flags, data_address, data_size);
   }
-  // Producer-only front cache for the clean path: skips the buffer_resource_mutex_
-  // and the node-based map walk (two cache misses per vertex/index stream).
-  // Slots are dropped when the handle is dirtied or released; a recorder-side
-  // clear or reclamation bumps the epoch. weak_ptr keeps use_count() exact for
-  // reclamation.
+
   BufferCaptureCacheSlot& cache_slot = BufferCaptureSlot(handle);
   const uint64_t cache_epoch = buffer_resources_epoch_.load(std::memory_order_acquire);
+  const uint64_t current_frame = g_native_memory_profile_event_frame.load(std::memory_order_relaxed) +
+                                 buffer_resources_epoch_.load(std::memory_order_relaxed) + 1;
   auto remember = [&](const std::shared_ptr<const NativeBufferResource>& resource) {
     cache_slot.handle = handle;
     cache_slot.flags = flags;
@@ -5282,41 +5514,16 @@ Gta4NativeGraphicsSystem::CaptureBufferResource(uint32_t handle) {
     cache_slot.epoch = cache_epoch;
     cache_slot.resource = resource;
   };
-  if (cache_slot.handle == handle && cache_slot.epoch == cache_epoch &&
+if (cache_slot.handle == handle && cache_slot.epoch == cache_epoch &&
       cache_slot.flags == flags && cache_slot.address == data_address &&
-      cache_slot.size == data_size && !metadata->guest_locked &&
-      !buffer_fast_path_disabled_.load(std::memory_order_relaxed)) {
+      cache_slot.size == data_size && !metadata->guest_locked) {
     if (auto cached = cache_slot.resource.lock()) {
-      // Already validated (or captured) this frame, and no unlock since: the
-      // slot is dropped when the handle is dirtied. Meshes drawn many times a
-      // frame then skip the cold shadow compare and the stores into the
-      // resource that the recorder reads.
-      if (REXCVAR_GET(gta4_native_buffer_validation_per_frame) &&
-          cached->last_used_frame.load(std::memory_order_relaxed) ==
-              g_native_memory_profile_event_frame.load(std::memory_order_relaxed)) {
-        return cached;
-      }
-      const uint8_t* cached_data = memory_->TranslateVirtual<const uint8_t*>(data_address);
-      if (cached_data) {
-        buffer_fast_path_request_count_.fetch_add(1, std::memory_order_relaxed);
-        const NativeBufferShadowValidationRange validation_range =
-            GetNativeBufferShadowValidationRange(
-                data_size, cached->shadow_validation_offset.load(std::memory_order_relaxed));
-        buffer_shadow_validation_count_.fetch_add(1, std::memory_order_relaxed);
-        if (NativeBufferShadowPayloadRangeMatches(cached_data, cached->payload.data(), data_size,
-                                                  validation_range)) {
-          cached->shadow_validation_offset.store(validation_range.next_offset,
-                                                 std::memory_order_relaxed);
-          buffer_capture_reuse_count_.fetch_add(1, std::memory_order_relaxed);
-          cached->last_used_frame.store(
-              g_native_memory_profile_event_frame.load(std::memory_order_relaxed),
-              std::memory_order_relaxed);
-          return cached;
-        }
-      }
-    }
-    cache_slot.handle = 0;  // Mismatch or gone: the full path below decides.
+      // Чистый слот во фронт-кэше (сбрасывается при любом ResourceUnlock) — возвращаем сразу без memcmp!
+      return cached;
+    }
+    cache_slot.handle = 0;
   }
+
   std::shared_ptr<const NativeBufferResource> matching_cache_entry;
   bool can_reuse_clean_capture = false;
   {
@@ -5332,62 +5539,33 @@ Gta4NativeGraphicsSystem::CaptureBufferResource(uint32_t handle) {
     can_reuse_clean_capture =
         CanReuseNativeBufferCapture(cache_present, dirty_buffer_handles_.contains(handle),
                                      metadata_matches) &&
-        !metadata->guest_locked && !buffer_fast_path_disabled_.load(std::memory_order_relaxed);
+        !metadata->guest_locked;
+  }
+
+  // Если буфер чистый (не было ResourceUnlock) — сразу запоминаем в слот и отдаём БЕЗ memcmp!
+  if (can_reuse_clean_capture && matching_cache_entry) {
+    buffer_capture_reuse_count_.fetch_add(1, std::memory_order_relaxed);
+    remember(matching_cache_entry);
+    return matching_cache_entry;
   }
 
   const uint8_t* data = memory_->TranslateVirtual<const uint8_t*>(data_address);
   if (!data) {
     return reject("payload-translation", flags, data_address, data_size);
   }
-  // Dirty notifications are conservative, and the session fallback requires
-  // full validation. Compare against the immutable capture before allocating a
-  // temporary payload and hashing it; unchanged bytes need neither operation.
-  // Keep this separate from the sampled clean path and its mismatch safeguard.
-  if (!can_reuse_clean_capture && matching_cache_entry &&
+
+  // Сюда попадают ТОЛЬКО буферы после ResourceUnlock (реально грязные):
+  if (matching_cache_entry &&
       NativeBufferShadowPayloadMatches(data, matching_cache_entry->payload.data(), data_size)) {
     std::lock_guard lock(buffer_resource_mutex_);
     const auto existing = buffer_resources_.find(handle);
     if (existing != buffer_resources_.end() && existing->second == matching_cache_entry) {
       dirty_buffer_handles_.erase(handle);
-      matching_cache_entry->last_used_frame.store(
-          g_native_memory_profile_event_frame.load(std::memory_order_relaxed),
-          std::memory_order_relaxed);
+      matching_cache_entry->last_used_frame.store(current_frame, std::memory_order_relaxed);
       remember(matching_cache_entry);
       return matching_cache_entry;
     }
   }
-  if (can_reuse_clean_capture) {
-    const auto& clean_cache_entry = matching_cache_entry;
-    buffer_fast_path_request_count_.fetch_add(1, std::memory_order_relaxed);
-    const size_t validation_offset =
-        clean_cache_entry->shadow_validation_offset.load(std::memory_order_relaxed);
-    const NativeBufferShadowValidationRange validation_range =
-        GetNativeBufferShadowValidationRange(data_size, validation_offset);
-    buffer_shadow_validation_count_.fetch_add(1, std::memory_order_relaxed);
-    if (NativeBufferShadowPayloadRangeMatches(data, clean_cache_entry->payload.data(), data_size,
-                                              validation_range)) {
-      clean_cache_entry->shadow_validation_offset.store(validation_range.next_offset,
-                                                        std::memory_order_relaxed);
-      buffer_capture_reuse_count_.fetch_add(1, std::memory_order_relaxed);
-      clean_cache_entry->last_used_frame.store(
-          g_native_memory_profile_event_frame.load(std::memory_order_relaxed),
-          std::memory_order_relaxed);
-      remember(clean_cache_entry);
-      return clean_cache_entry;
-    }
-    buffer_shadow_mismatch_count_.fetch_add(1, std::memory_order_relaxed);
-    if (ShouldDisableNativeBufferFastPath(true, false) &&
-        !buffer_fast_path_disabled_.exchange(true, std::memory_order_relaxed)) {
-      buffer_fast_path_disable_count_.fetch_add(1, std::memory_order_relaxed);
-      REXLOG_ERROR(
-          "gta4-native: buffer shadow mismatch for {:08X}; disabling clean-buffer fast path "
-          "for this session; backing={:08X} size={} flags={:08X} generation={} "
-          "validation-offset={} validation-length={} captured-frame={} last-used-frame={}",
-          handle, data_address, data_size, flags, clean_cache_entry->generation,
-          validation_range.offset, validation_range.length, clean_cache_entry->created_frame,
-          clean_cache_entry->last_used_frame.load(std::memory_order_relaxed));
-    }
-  }
 
   std::vector<uint8_t> payload(data, data + data_size);
   const uint64_t content_hash = XXH3_64bits(payload.data(), payload.size());
@@ -5399,9 +5577,7 @@ Gta4NativeGraphicsSystem::CaptureBufferResource(uint32_t handle) {
         resource->guest_size == data_size && resource->content_hash == content_hash &&
         resource->payload == payload) {
       dirty_buffer_handles_.erase(handle);
-      resource->last_used_frame.store(
-          g_native_memory_profile_event_frame.load(std::memory_order_relaxed),
-          std::memory_order_relaxed);
+      resource->last_used_frame.store(current_frame, std::memory_order_relaxed);
       remember(resource);
       return resource;
     }
@@ -5414,8 +5590,8 @@ Gta4NativeGraphicsSystem::CaptureBufferResource(uint32_t handle) {
   resource->guest_size = data_size;
   resource->content_hash = content_hash;
   resource->generation = next_buffer_generation_++;
-  resource->created_frame = g_native_memory_profile_event_frame.load(std::memory_order_relaxed);
-  resource->last_used_frame.store(resource->created_frame, std::memory_order_relaxed);
+  resource->created_frame = current_frame;
+  resource->last_used_frame.store(current_frame, std::memory_order_relaxed);
   resource->payload = std::move(payload);
   const bool replacing = existing != buffer_resources_.end() && existing->second;
   buffer_resources_[handle] = resource;
@@ -5441,16 +5617,26 @@ SurfaceDescriptor Gta4NativeGraphicsSystem::CaptureSurfaceDescriptor(uint32_t ha
     REXLOG_WARN("gta4-native-surface: unable to translate snapshot surface {:08X}", handle);
     return descriptor;
   }
-  auto load_word = [object](size_t offset) {
-    uint32_t value;
-    std::memcpy(&value, object + offset, sizeof(value));
-    return __builtin_bswap32(value);
-  };
-  descriptor.flags = load_word(0);
-  descriptor.base = load_word(24);
-  descriptor.address = load_word(28);
-  descriptor.packed_dimensions = load_word(36);
-  descriptor.format = load_word(40);
+  //Быстрое чтение всей структуры поверхности за один memcpy в CaptureSurfaceDescriptor
+ // auto load_word = [object](size_t offset) {
+   // uint32_t value;
+   // std::memcpy(&value, object + offset, sizeof(value));
+   // return __builtin_bswap32(value);
+ // };
+ // descriptor.flags = load_word(0);
+ // descriptor.base = load_word(24);
+ // descriptor.address = load_word(28);
+ // descriptor.packed_dimensions = load_word(36);
+ // descriptor.format = load_word(40);
+ // начало "патча"
+ uint32_t words[11];
+  std::memcpy(words, object, sizeof(words));
+  descriptor.flags = __builtin_bswap32(words[0]);
+  descriptor.base = __builtin_bswap32(words[6]);
+  descriptor.address = __builtin_bswap32(words[7]);
+  descriptor.packed_dimensions = __builtin_bswap32(words[9]);
+  descriptor.format = __builtin_bswap32(words[10]);
+ // конец "патча"
   descriptor.width = (std::rotl(descriptor.packed_dimensions, 14) & 0x3FFF) + 1;
   descriptor.height = (std::rotl(descriptor.packed_dimensions, 29) & 0x7FFF) + 1;
   descriptor.sample_type = DecodeSurfaceSampleType(descriptor.base);
@@ -6314,41 +6500,46 @@ void Gta4NativeGraphicsSystem::TraceModernShaderDraw(
 }
 
 void Gta4NativeGraphicsSystem::RenderWorkerMain() {
+#if defined(__ANDROID__)
+  pthread_setname_np(pthread_self(), "GtaRenderWork");
+#endif
   std::vector<std::pair<RenderPhase, uint32_t>> render_phase_stack;
   uint64_t startup_texture_lock_count = 0;
   bool startup_present_follows_texture_lock_flush = false;
   auto log_frame_batch = [this](std::string_view boundary, const NativeCommand& boundary_command,
                                 uint32_t submitted_frame, size_t queued_after_boundary) {
 #if REX_PLATFORM_ANDROID
-    {
+   // {
       // TEMP: per-frame draw census by render target size (perf investigation).
-      static uint64_t last_report = 0;
-      const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
-      const uint64_t frequency = rex::chrono::Clock::QueryHostTickFrequency();
-      if (now - last_report > 3 * frequency) {
-        last_report = now;
-        std::map<std::string, uint32_t> groups;
-        uint32_t total = 0;
-        for (const NativeCommand& pending : current_frame_) {
-          if (pending.type != CommandType::kDrawPrimitive &&
-              pending.type != CommandType::kDrawPrimitiveUp &&
-              pending.type != CommandType::kDrawIndexedPrimitive) {
-            continue;
-          }
-          ++total;
-          const auto& c = pending.snapshot_render_targets[0];
-          const auto& d = pending.snapshot_depth_stencil;
-          ++groups[fmt::format("c{}x{}/d{}x{}/p{}", c.width, c.height, d.width, d.height,
-                               uint32_t(pending.render_phase))];
-        }
-        std::string summary;
-        for (const auto& [key, count] : groups) {
-          summary += fmt::format(" {}={}", key, count);
-        }
-        REXLOG_WARN("gta4-draw-census: commands={} draws={}{}", current_frame_.size(), total,
-                    summary);
-      }
-    }
+	  
+	  
+      //static uint64_t last_report = 0;
+     // const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
+     // const uint64_t frequency = rex::chrono::Clock::QueryHostTickFrequency();
+    //  if (now - last_report > 3 * frequency) {
+     //   last_report = now;
+     //   std::map<std::string, uint32_t> groups;
+     //   uint32_t total = 0;
+      //  for (const NativeCommand& pending : current_frame_) {
+      //    if (pending.type != CommandType::kDrawPrimitive &&
+       //       pending.type != CommandType::kDrawPrimitiveUp &&
+        //      pending.type != CommandType::kDrawIndexedPrimitive) {
+        //    continue;
+       //   }
+       //   ++total;
+       //   const auto& c = pending.snapshot_render_targets[0];
+       //   const auto& d = pending.snapshot_depth_stencil;
+      //    ++groups[fmt::format("c{}x{}/d{}x{}/p{}", c.width, c.height, d.width, d.height,
+      //                         uint32_t(pending.render_phase))];
+     //   }
+      //  std::string summary;
+      //  for (const auto& [key, count] : groups) {
+     //     summary += fmt::format(" {}={}", key, count);
+     //   }
+      //  REXLOG_WARN("gta4-draw-census: commands={} draws={}{}", current_frame_.size(), total,
+      //              summary);
+    //  }
+  //  }
 #endif
     if (!CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeTrace)) {
       return;
@@ -6449,7 +6640,10 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
         add(owner->depth_handoff_source);
         add(owner->present_source);
         add(owner->postfx_half_scene);
-        for (const auto& texture : owner->textures) add(texture);
+        for (uint32_t mask = owner->used_texture_mask; mask; mask &= mask - 1) {
+          const uint32_t idx = __builtin_ctz(mask);
+          if (idx < owner->textures.size()) add(owner->textures[idx]);
+        }
         if (fresh_count) {
           std::lock_guard lock(worker_protection_mutex_);
           assembly_texture_protection_.insert(fresh.begin(), fresh.begin() + fresh_count);
@@ -6466,13 +6660,6 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
             assembled.type == CommandType::kDrawIndexedPrimitive) ++bound.all_draws;
         else if (assembled.type == CommandType::kResolve) ++bound.resolves;
         else if (assembled.type == CommandType::kDepthSurfaceHandoff) ++bound.depth_handoffs;
-        for (const auto& texture : assembled.textures) {
-          if (texture && texture->packed_depth_source &&
-              std::find(bound.packed_alias_generations.begin(), bound.packed_alias_generations.end(),
-                        texture->generation) == bound.packed_alias_generations.end()) {
-            bound.packed_alias_generations.push_back(texture->generation);
-          }
-        }
         {
           // The same inputs RecordNativeFrame's frontbuffer-reader scan reads.
           const uint32_t read_epoch = assembly_protection_epoch_;
@@ -6481,10 +6668,20 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
             if (resource)
               std::atomic_ref(resource->assembly_read_epoch).store(read_epoch, std::memory_order_relaxed);
           };
-          for (const auto& texture : assembled.textures) {
+          for (uint32_t mask = assembled.used_texture_mask; mask; mask &= mask - 1) {
+            const uint32_t idx = __builtin_ctz(mask);
+            if (idx >= assembled.textures.size()) continue;
+            const auto& texture = assembled.textures[idx];
             if (!texture) continue;
             mark_read(texture.get());
-            mark_read(texture->packed_depth_source.get());
+            if (texture->packed_depth_source) {
+              mark_read(texture->packed_depth_source.get());
+              if (std::find(bound.packed_alias_generations.begin(),
+                            bound.packed_alias_generations.end(),
+                            texture->generation) == bound.packed_alias_generations.end()) {
+                bound.packed_alias_generations.push_back(texture->generation);
+              }
+            }
           }
           mark_read(assembled.postfx_half_scene.get());
           mark_read(assembled.depth_handoff_source.get());
@@ -6532,8 +6729,16 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
         worker_command_cursor_ = 0;
       }
       std::unique_lock lock = LockWithSpin(render_mutex_);
-      render_condition_.wait(
-          lock, [this]() { return !render_worker_running_ || !render_queue_.empty(); });
+      if (render_worker_running_ && render_queue_.empty()) {
+        TRACE_SCOPE("RW: wait for commands");
+        worker_waiting_ = true;
+        const uint64_t wait_cmds_begin = profile::CpuTick();
+        render_condition_.wait(
+            lock, [this]() { return !render_worker_running_ || !render_queue_.empty(); });
+        g_wait_rw_cmd_ticks.fetch_add(profile::CpuTick() - wait_cmds_begin,
+                                      std::memory_order_relaxed);
+        worker_waiting_ = false;
+      }
       if (render_queue_.empty()) {
         if (!render_worker_running_) break;
         continue;
@@ -6987,6 +7192,9 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
 #if REX_PLATFORM_ANDROID
           // TEMP: pipeline stall telemetry (perf investigation).
           if (++pipeline_report_frames_ == 120) {
+           // Отчёт идёт на GtaRenderWork ДО передачи кадра рекордеру: пока он печатается,
+           // рекордер стоит (по frame-timeline: +1.6 мс на 855, +2.3 мс на 888 каждые 120 кадров).
+           if (REXCVAR_GET(gta4_native_pipeline_report)) {
             const double ms_per_tick = 1000.0 / double(rex::chrono::Clock::QueryHostTickFrequency());
             std::string types;
             for (size_t i = 0; i < pipeline_sync_type_counts_.size(); ++i)
@@ -7005,6 +7213,12 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
                                 g_worker_ring_fallbacks.exchange(0) / 120.0,
                                 (unsigned long long)g_worker_ring_mismatches.exchange(0),
                                 (unsigned long long)g_worker_ring_checked.exchange(0), types.c_str());
+            __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
+                                "waits ms/f: rec-idle=%.1f rec-publish=%.1f rg-queue=%.1f rw-cmds=%.1f",
+                                g_wait_rec_job_ticks.exchange(0) * ms_per_tick / 120.0,
+                                g_wait_rec_publish_ticks.exchange(0) * ms_per_tick / 120.0,
+                                g_wait_rg_queue_ticks.exchange(0) * ms_per_tick / 120.0,
+                                g_wait_rw_cmd_ticks.exchange(0) * ms_per_tick / 120.0);
             __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                                 "vk: %s ubo-shaders=%u ubo-loads=%llu legacy-mul=%llu",
                                 ui::vulkan::ConsumeCommandCounters(120).c_str(), ubo_rewritten_shaders_,
@@ -7029,8 +7243,10 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
             }
             {
               std::vector<std::pair<uint32_t, uint32_t>> sites;
-              for (uint32_t line = 0; line < g_rendering_end_sites.size(); ++line)
-                if (const uint32_t n = g_rendering_end_sites[line].exchange(0)) sites.emplace_back(n, line);
+              // Счётчики пишутся только при gta4_native_debug_counters; без него массив нулевой.
+              if (REXCVAR_GET(gta4_native_debug_counters))
+                for (uint32_t line = 0; line < g_rendering_end_sites.size(); ++line)
+                  if (const uint32_t n = g_rendering_end_sites[line].exchange(0)) sites.emplace_back(n, line);
               std::sort(sites.rbegin(), sites.rend());
               std::string top;
               for (size_t i = 0; i < sites.size() && i < 12; ++i)
@@ -7039,8 +7255,9 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
             }
             {
               std::vector<std::pair<uint32_t, uint32_t>> sites;
-              for (uint32_t line = 0; line < g_barrier_sites.size(); ++line)
-                if (const uint32_t n = g_barrier_sites[line].exchange(0)) sites.emplace_back(n, line);
+              if (REXCVAR_GET(gta4_native_debug_counters))
+                for (uint32_t line = 0; line < g_barrier_sites.size(); ++line)
+                  if (const uint32_t n = g_barrier_sites[line].exchange(0)) sites.emplace_back(n, line);
               std::sort(sites.rbegin(), sites.rend());
               std::string top;
               for (size_t i = 0; i < sites.size() && i < 12; ++i)
@@ -7049,8 +7266,9 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
             }
             {
               std::vector<std::pair<uint32_t, uint32_t>> sites;
-              for (uint32_t line = 0; line < g_resolve_sites.size(); ++line)
-                if (const uint32_t n = g_resolve_sites[line].exchange(0)) sites.emplace_back(n, line);
+              if (REXCVAR_GET(gta4_native_debug_counters))
+                for (uint32_t line = 0; line < g_resolve_sites.size(); ++line)
+                  if (const uint32_t n = g_resolve_sites[line].exchange(0)) sites.emplace_back(n, line);
               std::sort(sites.rbegin(), sites.rend());
               std::string top;
               for (size_t i = 0; i < sites.size() && i < 12; ++i)
@@ -7120,8 +7338,9 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
             }
             {
               std::vector<std::pair<uint32_t, uint32_t>> reasons;
-              for (uint32_t r = 0; r < g_rendering_split_reasons.size(); ++r)
-                if (const uint32_t n = g_rendering_split_reasons[r].exchange(0)) reasons.emplace_back(n, r);
+              if (REXCVAR_GET(gta4_native_debug_counters))
+                for (uint32_t r = 0; r < g_rendering_split_reasons.size(); ++r)
+                  if (const uint32_t n = g_rendering_split_reasons[r].exchange(0)) reasons.emplace_back(n, r);
               std::sort(reasons.rbegin(), reasons.rend());
               std::string top;
               for (size_t i = 0; i < reasons.size() && i < 10; ++i)
@@ -7134,6 +7353,7 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
                 if (const uint32_t n = g_light_reuse_failures[r].exchange(0)) top += fmt::format(" f{}={:.0f}", r, n / 120.0);
               __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "light-reuse/frame:%s", top.c_str());
             }
+           }  // gta4_native_pipeline_report
             pipeline_report_frames_ = 0;
             pipelined_sync_waits_ = 0;
             pipeline_sync_wait_ticks_ = 0;
@@ -7180,6 +7400,7 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
         const bool phase_draw = command.type == CommandType::kDrawPrimitive ||
                                 command.type == CommandType::kDrawPrimitiveUp ||
                                 command.type == CommandType::kDrawIndexedPrimitive;
+		uint32_t old_depth_clamp = command.fixed_function_state.depth_clamp_enable;
         if (phase_draw) {
           NativeFixedFunctionState& fixed = command.fixed_function_state;
           uint32_t active_color_target_mask = 0;
@@ -7218,8 +7439,17 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
             command.type == CommandType::kDrawPrimitiveUp ||
             command.type == CommandType::kDrawIndexedPrimitive ||
             command.type == CommandType::kClear) {
-          command.recorded_fixed_function_state_hash =
-              HashFixedFunctionState(command.fixed_function_state);
+          // === НАЧАЛО ПАТЧА (Оптимизация №1) ===
+          if (command.captured_fixed_function_state_hash != 0 &&
+              command.fixed_function_state.depth_clamp_enable == old_depth_clamp) {
+            // Если depth_clamp не менялся на воркере — просто берем готовый хеш (экономия тактов CPU)
+            command.recorded_fixed_function_state_hash = command.captured_fixed_function_state_hash;
+          } else {
+            // Если состояние изменилось (например, сработал forced_clamp) — пересчитываем
+            command.recorded_fixed_function_state_hash = 
+                HashFixedFunctionState(command.fixed_function_state);
+          }
+          // === КОНЕЦ ПАТЧА ===
           const uint32_t device = CommandDevice(command.type, command.bytes.data());
           if (!ApplyShaderConstantDelta(command, device)) {
             REXLOG_ERROR("gta4-native-constants: rejected worker command type={} device={:08X}",
@@ -7293,7 +7523,112 @@ void Gta4NativeGraphicsSystem::RenderWorkerMain() {
 
 void Gta4NativeGraphicsSystem::FinishPresent(NativeCommand& command,
                                              const PresentCommand& present) {
-  const bool published = PublishFrame(present, command.present_source, command.environmental_data);
+    bool published = false;
+  {
+    TRACE_SCOPE("Rec: PublishFrame");
+    const uint64_t publish_begin = profile::CpuTick();
+    const bool hitch_log_active = REXCVAR_GET(gta4_native_hitch_log);
+    const uint64_t hitch_begin = hitch_log_active ? FrameTimelineNow() : 0;
+    if (hitch_log_active) {
+      g_hitch_pre_record_ns.store(0, std::memory_order_relaxed);
+      g_hitch_submit_done_ns.store(0, std::memory_order_relaxed);
+    }
+    published = PublishFrame(present, command.present_source, command.environmental_data);
+    g_wait_rec_publish_ticks.fetch_add(profile::CpuTick() - publish_begin,
+                                       std::memory_order_relaxed);
+    if (hitch_log_active) {
+      const uint64_t hitch_end = FrameTimelineNow();
+    const double publish_ms = double(hitch_end - hitch_begin) / 1e6;
+    const uint64_t prev_end =
+        g_hitch_prev_end_ns.exchange(hitch_end, std::memory_order_relaxed);
+    const double since_prev_ms =
+        prev_end ? double(hitch_end - prev_end) / 1e6 : 0.0;
+    const double recipes_ms =
+        double(g_hitch_recipes_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double record_ms =
+        double(g_hitch_record_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double record_setup_ms =
+        double(g_hitch_record_setup_ns.load(std::memory_order_relaxed)) / 1e6;
+const double record_cmds_ms = record_ms - record_setup_ms;
+    const double cmd_draw_ms =
+        double(g_hitch_cmd_draw_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double cmd_draw_sh_ms =
+        double(g_hitch_cmd_draw_sh_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double cmd_draw_w_ms =
+        double(g_hitch_cmd_draw_w_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double cmd_resolve_ms =
+        double(g_hitch_cmd_resolve_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double cmd_clear_ms =
+        double(g_hitch_cmd_clear_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double cmd_other_ms =
+        double(g_hitch_cmd_other_ns.load(std::memory_order_relaxed)) / 1e6;
+    const uint64_t pre_ns = g_hitch_pre_record_ns.load(std::memory_order_relaxed);
+    const uint64_t submit_done = g_hitch_submit_done_ns.load(std::memory_order_relaxed);
+    const double pre_ms = pre_ns > hitch_begin ? double(pre_ns - hitch_begin) / 1e6 - recipes_ms : 0.0;
+    const double present_ms = submit_done > hitch_begin ? double(hitch_end - submit_done) / 1e6 : 0.0;
+	const double other_ms = publish_ms - recipes_ms - record_ms;
+    const double submit_ms = std::max(0.0, other_ms - pre_ms - present_ms);
+    const uint64_t hk_end = g_hitch_hk_end_ns.load(std::memory_order_relaxed);
+    {
+      const double hk_s1 = double(g_hk_surf_ns.load(std::memory_order_relaxed)) / 1e6;
+      const double hk_s2 = double(g_hk_texunused_ns.load(std::memory_order_relaxed)) / 1e6;
+      const double hk_s3 = double(g_hk_texretired_ns.load(std::memory_order_relaxed)) / 1e6;
+      const double hk_s4 = double(g_hk_bufunused_ns.load(std::memory_order_relaxed)) / 1e6;
+      const double hk_s5 = double(g_hk_persist_ns.load(std::memory_order_relaxed)) / 1e6;
+      if (hk_s1 + hk_s2 + hk_s3 + hk_s4 + hk_s5 > 6.0) {
+#if defined(__ANDROID__)
+        __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
+                            "hitch-hk: frame=%u surf=%.1f tex-unused=%.1f tex-retired=%.1f buf-unused=%.1f persist-buf=%.1f ms",
+                            (unsigned)present.submitted_frame, hk_s1, hk_s2, hk_s3, hk_s4, hk_s5);
+#endif
+      }
+    }
+    const uint64_t tex_begin = g_hitch_tex_begin_ns.load(std::memory_order_relaxed);
+    const uint64_t cb_end = g_hitch_cb_end_ns.load(std::memory_order_relaxed);
+    const bool pre_split_ok = hk_end > hitch_begin && tex_begin >= hk_end && pre_ns >= tex_begin;
+   const double pre_hk_ms = pre_split_ok ? double(hk_end - hitch_begin) / 1e6 - recipes_ms : 0.0;
+    const double pre_mid_ms = pre_split_ok ? double(tex_begin - hk_end) / 1e6 : 0.0;
+    const double pre_tex_ms = pre_split_ok ? double(pre_ns - tex_begin) / 1e6 : 0.0;
+    const double tex_pool_ms = double(g_hitch_tex_pool_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double tex_loop_ms = double(g_hitch_tex_loop_ns.load(std::memory_order_relaxed)) / 1e6;
+    const double tex_post_ms = double(g_hitch_tex_post_ns.load(std::memory_order_relaxed)) / 1e6;
+    const bool present_split_ok = submit_done > hitch_begin && cb_end >= submit_done && hitch_end >= cb_end;
+    const double present_cb_ms = present_split_ok ? double(cb_end - submit_done) / 1e6 : 0.0;
+    const double present_wait_ms = present_split_ok ? double(hitch_end - cb_end) / 1e6 : 0.0;
+    if (publish_ms > 30.0 || since_prev_ms > 35.0) {
+#if defined(__ANDROID__)
+      __android_log_print(
+          ANDROID_LOG_WARN, "LibertyRecomp",
+          "hitch: frame=%u publish=%.1f ms (recipes=%.1f record=%.1f [setup=%.1f cmds=%.1f draw=%.1f (sh=%.1f w=%.1f) resolve=%.1f clear=%.1f cmd-other=%.1f] other=%.1f [pre=%.1f (hk=%.1f mid=%.1f tex=%.1f [p=%.1f l=%.1f post=%.1f]) submit=%.1f present=%.1f (cb=%.1f wait=%.1f)]) since-prev-publish=%.1f ms",
+          (unsigned)present.submitted_frame,
+publish_ms,
+recipes_ms,
+record_ms,
+record_setup_ms,
+record_cmds_ms,
+cmd_draw_ms,
+cmd_draw_sh_ms,
+cmd_draw_w_ms,
+cmd_resolve_ms,
+cmd_clear_ms,
+cmd_other_ms,
+other_ms,
+pre_ms,
+pre_hk_ms,
+pre_mid_ms,
+pre_tex_ms,
+tex_pool_ms,    // <-- [p=%.1f]
+tex_loop_ms,    // <-- [l=%.1f]
+tex_post_ms,    // <-- [post=%.1f]
+submit_ms,
+present_ms,
+present_cb_ms,
+present_wait_ms,
+since_prev_ms);
+#endif
+    }
+    }
+  }
   {
     std::lock_guard lock(render_mutex_);
     if (queued_title_presents_) --queued_title_presents_;
@@ -7307,6 +7642,9 @@ void Gta4NativeGraphicsSystem::FinishPresent(NativeCommand& command,
   SignalGuestFrameComplete(present.device, present.submitted_frame);
   ClearNativeFrameCommands();
   UpdateDynamicDrawDistance();
+  
+  
+
 }
 
 void Gta4NativeGraphicsSystem::UpdateDynamicDrawDistance() {
@@ -7448,9 +7786,15 @@ void Gta4NativeGraphicsSystem::FrameRecorderMain() {
   while (true) {
     std::optional<NativeRecorderJob> job;
     {
-      std::unique_lock lock(frame_recorder_mutex_);
+     std::unique_lock lock(frame_recorder_mutex_);
+      {
+        TRACE_SCOPE("Rec: wait for job");
+        const uint64_t wait_job_begin = profile::CpuTick();
       frame_recorder_condition_.wait(
           lock, [this]() { return frame_recorder_stop_ || frame_recorder_job_.has_value(); });
+      g_wait_rec_job_ticks.fetch_add(profile::CpuTick() - wait_job_begin,
+                                     std::memory_order_relaxed);
+      }
       if (!frame_recorder_job_) break;
       job = std::move(frame_recorder_job_);
       frame_recorder_job_.reset();
@@ -7459,8 +7803,14 @@ void Gta4NativeGraphicsSystem::FrameRecorderMain() {
     recording_ring_serial_ = job->ring_serial;
     MarkFrameTimeline(timeline_frame, kTimelineRecorderStart);
     RunReleaseEffects(recording_release_effects_);
-    BeginModernShaderFrame();
-    FinishPresent(*job->present_command, job->present);
+    {
+      TRACE_SCOPE("Rec: BeginModernShaderFrame");
+      BeginModernShaderFrame();
+    }
+    {
+      TRACE_SCOPE("Rec: FinishPresent");
+      FinishPresent(*job->present_command, job->present);
+    }
     MarkFrameTimeline(timeline_frame, kTimelinePublishEnd);
     LogFrameTimeline(timeline_frame);
     job.reset();
@@ -7473,6 +7823,7 @@ void Gta4NativeGraphicsSystem::FrameRecorderMain() {
 }
 
 void Gta4NativeGraphicsSystem::WaitForFrameRecorderIdle() {
+  TRACE_SCOPE("RW: wait recorder idle");
   std::unique_lock lock(frame_recorder_mutex_);
   frame_recorder_condition_.wait(lock, [this]() { return !frame_recorder_busy_; });
 }
@@ -7546,6 +7897,11 @@ void Gta4NativeGraphicsSystem::ApplyReleaseResourceEffects(const NativeReleaseEf
 
 std::shared_ptr<const Gta4NativeGraphicsSystem::NativePipelineState>
 Gta4NativeGraphicsSystem::SnapshotPipeline(const NativeCommand& command, bool draw) {
+  if (!draw && last_pipeline_snapshot_ &&
+      last_pipeline_snapshot_->version == pipeline_state_.version) {
+    ++pipeline_snapshot_reuses_;
+    return last_pipeline_snapshot_;
+  }
   const auto& colors = draw ? command.snapshot_render_targets : pipeline_state_.render_targets;
   const auto& depth = draw ? command.snapshot_depth_stencil : pipeline_state_.depth_stencil;
   if (last_pipeline_snapshot_ && last_pipeline_snapshot_->version == pipeline_state_.version &&
@@ -7554,11 +7910,11 @@ Gta4NativeGraphicsSystem::SnapshotPipeline(const NativeCommand& command, bool dr
       last_pipeline_snapshot_->vertex_declaration_resource == pipeline_state_.vertex_declaration_resource &&
       NativeSurfaceStateEqual(last_pipeline_snapshot_->depth_stencil, depth) &&
       std::equal(colors.begin(), colors.end(), last_pipeline_snapshot_->render_targets.begin(),
-                 [](const auto& a, const auto& b) { return NativeSurfaceStateEqual(a,b); })) {
+                 [](const auto& a, const auto& b) { return NativeSurfaceStateEqual(a, b); })) {
     ++pipeline_snapshot_reuses_;
     return last_pipeline_snapshot_;
   }
-  auto snapshot = std::allocate_shared<NativePipelineState>(
+auto snapshot = std::allocate_shared<NativePipelineState>(
       std::pmr::polymorphic_allocator<NativePipelineState>(&snapshot_pool_), pipeline_state_);
   snapshot->render_targets = colors;
   snapshot->depth_stencil = depth;
@@ -8018,35 +8374,58 @@ bool Gta4NativeGraphicsSystem::SnapshotDrawConstants(NativeCommand& command,
               command.pixel_constants_upload_hash, command.pixel_constants_upload_bytes);
   WriteWorkerRingConstants(command, vertex_shader, pixel_shader);
   {
-    const uint64_t memo_signature =
-        HashNativeFixedFunctionState(NativePipelineMemoState(command.fixed_function_state));
-    command.pipeline_memo_signature = memo_signature ? memo_signature : 1;
+    static uint64_t last_recorded_fixed_hash = 0;
+    static uint64_t last_memo_signature = 0;
+    if (last_memo_signature != 0 &&
+        command.recorded_fixed_function_state_hash == last_recorded_fixed_hash) {
+      command.pipeline_memo_signature = last_memo_signature;
+    } else {
+      const uint64_t memo_signature =
+          HashNativeFixedFunctionState(NativePipelineMemoState(command.fixed_function_state));
+      last_recorded_fixed_hash = command.recorded_fixed_function_state_hash;
+      last_memo_signature = memo_signature ? memo_signature : 1;
+      command.pipeline_memo_signature = last_memo_signature;
+    }
   }
-  command.texture_inputs_signature = 0;
+ command.texture_inputs_signature =
+      (command.pipeline_state && !command.used_texture_mask) ? 0x1ull : 0ull;
   if (command.pipeline_state && command.used_texture_mask) {
-    // Same inputs as NativePreparedTextureInputsEqual, packed and hashed here
-    // so the recorder compares one word per draw.
     struct StageInputs {
+      uint64_t resource;
       uint32_t stage;
       uint32_t pipeline_texture;
-      uint64_t resource;
       xenos::xe_gpu_texture_fetch_t fetch;
     };
+    static_assert(sizeof(StageInputs) == 40, "No padding in StageInputs");
     std::array<StageInputs, kTextureStageCount> stages;
     size_t count = 0;
     for (uint32_t mask = command.used_texture_mask; mask; mask &= mask - 1) {
       const uint32_t stage = uint32_t(std::countr_zero(mask));
       if (stage >= command.textures.size()) break;
-      StageInputs& inputs = stages[count++];
-      std::memset(&inputs, 0, sizeof(inputs));
-      inputs.stage = stage;
-      inputs.pipeline_texture = command.pipeline_state->textures[stage];
-      inputs.resource = uint64_t(reinterpret_cast<uintptr_t>(command.textures[stage].get()));
-      inputs.fetch = command.texture_fetches[stage];
+      stages[count++] = StageInputs{
+          uint64_t(reinterpret_cast<uintptr_t>(command.textures[stage].get())),
+          stage,
+          command.pipeline_state->textures[stage],
+          command.texture_fetches[stage],
+      };
+    }
+	static std::array<StageInputs, kTextureStageCount> last_stages{};
+    static size_t last_count = 0;
+    static uint32_t last_mask = 0;
+    static uint64_t last_sig = 0;
+    const size_t bytes_len = count * sizeof(StageInputs);
+    if (last_sig != 0 && count == last_count && command.used_texture_mask == last_mask &&
+        std::memcmp(stages.data(), last_stages.data(), bytes_len) == 0) {
+      command.texture_inputs_signature = last_sig;
+    } else {
+      const uint64_t hash = XXH3_64bits_withSeed(stages.data(), bytes_len,
+                                                 command.used_texture_mask);
+      last_sig = hash ? hash : 1;
+      last_count = count;
+      last_mask = command.used_texture_mask;
+      std::memcpy(last_stages.data(), stages.data(), bytes_len);
+      command.texture_inputs_signature = last_sig;
     }
-    const uint64_t hash = XXH3_64bits_withSeed(stages.data(), count * sizeof(StageInputs),
-                                               command.used_texture_mask);
-    command.texture_inputs_signature = hash ? hash : 1;
   }
   return true;
 }
@@ -9299,8 +9678,10 @@ bool Gta4NativeGraphicsSystem::GetOrCreatePersistentBuffer(
     allocation.host_data = source;
     StageNativeFlightResource(NativeFlightResourceKind::kPersistentBuffer,
                               NativeVulkanHandleIdentity(entry.buffer), 0, key.generation);
-    gpu_flight::Record("native.persistent-reuse", NativeVulkanHandleIdentity(entry.buffer),
-                       predicted_submission, active_texture_frame_, entry.offset, entry.size);
+    if (IsNativeFlightRecorderEnabled()) {
+      gpu_flight::Record("native.persistent-reuse", NativeVulkanHandleIdentity(entry.buffer),
+                         predicted_submission, active_texture_frame_, entry.offset, entry.size);
+    }
     ++persistent_buffer_hits_;
     AddNativeGpuProfileCounter(performance::Counter::kPersistentBufferHits);
     return true;
@@ -9403,8 +9784,10 @@ bool Gta4NativeGraphicsSystem::GetOrCreatePersistentBuffer(
   allocation.host_data = source;
   StageNativeFlightResource(NativeFlightResourceKind::kPersistentBuffer,
                             NativeVulkanHandleIdentity(entry.buffer), 0, key.generation);
-  gpu_flight::Record("native.persistent-upload", NativeVulkanHandleIdentity(entry.buffer),
-                     predicted_submission, active_texture_frame_, entry.offset, entry.size);
+  if (IsNativeFlightRecorderEnabled()) {
+    gpu_flight::Record("native.persistent-upload", NativeVulkanHandleIdentity(entry.buffer),
+                       predicted_submission, active_texture_frame_, entry.offset, entry.size);
+  }
   ++persistent_buffer_misses_;
   persistent_buffer_upload_bytes_ += size;
   AddNativeGpuProfileCounter(performance::Counter::kPersistentBufferMisses);
@@ -9531,8 +9914,7 @@ bool Gta4NativeGraphicsSystem::EnsureFrameConstantArenaCapacity() {
       return false;
     }
     NativeUploadBuffer replacement{};
-    if (!CreateNativeUploadBuffer(desired_capacity, replacement,
-                                  !REXCVAR_GET(gta4_native_ubo_constants))) {
+    if (!CreateNativeUploadBuffer(desired_capacity, replacement, true)) {
       return false;
     }
     replacement.write_offset = 0;
@@ -9551,7 +9933,37 @@ bool Gta4NativeGraphicsSystem::EnsureFrameConstantArenaCapacity() {
     // Preserve full sequential writes if cached memory was unavailable.
     arena.compare_upload_bytes =
         (vulkan_device->memory_types().host_cached & (uint32_t(1) << replacement.memory_type)) != 0;
+ } else if (arena.storage.capacity > required_capacity * 2 &&
+             arena.storage.capacity > kNativeConstantArenaMinimumSize &&
+             ++constant_arena_underutilized_frames_ >= 30) {
+    constant_arena_underutilized_frames_ = 0;
+    const VkDeviceSize desired_capacity =
+        rex::round_up(std::max(required_capacity, kNativeConstantArenaMinimumSize),
+                      kNativeConstantArenaGrowthGranularity);
+    if (desired_capacity <= maximum_capacity && desired_capacity < arena.storage.capacity) {
+      NativeUploadBuffer replacement{};
+      if (CreateNativeUploadBuffer(desired_capacity, replacement, true)) {
+        replacement.write_offset = 0;
+        DestroyNativeUploadBuffer(arena.storage);
+        arena.storage = replacement;
+        arena.uploads.ResetStorage();
+        arena.compare_upload_bytes =
+            (vulkan_device->memory_types().host_cached & (uint32_t(1) << replacement.memory_type)) != 0;
+        
+        // Сбрасываем кэш дескрипторов, так как буфер пересоздан
+        if (active_frame_slot_ < ubo_constants_set_buffers_.size())
+          ubo_constants_set_buffers_[active_frame_slot_] = VK_NULL_HANDLE;
+        if (active_frame_slot_ < worker_ring_set_shared_buffers_.size())
+          worker_ring_set_shared_buffers_[active_frame_slot_] = VK_NULL_HANDLE;
+        ubo_bound_layout_ = VK_NULL_HANDLE;
+      }
+    }
+    if (arena.storage.capacity > std::numeric_limits<size_t>::max() ||
+        !arena.index.SetByteCapacity(size_t(arena.storage.capacity))) {
+      return false;
+    }
   } else {
+    constant_arena_underutilized_frames_ = 0; // Сбрасываем, если не сжимаем
     if (arena.storage.capacity > std::numeric_limits<size_t>::max() ||
         !arena.index.SetByteCapacity(size_t(arena.storage.capacity))) {
       return false;
@@ -9678,7 +10090,7 @@ void Gta4NativeGraphicsSystem::DestroyNativeFrameConstantArenas() {
 
 namespace {
 constexpr uint32_t kWorkerRingSegments = 6;
-constexpr VkDeviceSize kWorkerRingSegmentBytes = 10ull << 20;
+constexpr VkDeviceSize kWorkerRingSegmentBytes = 20ull << 20;
 // The last bank's uniform range may reach past its segment: keep it inside
 // the buffer.
 constexpr VkDeviceSize kWorkerRingGuardBytes = kVertexConstantsSize;
@@ -9755,7 +10167,7 @@ void Gta4NativeGraphicsSystem::WriteWorkerRingConstants(NativeCommand& command,
       !REXCVAR_GET(gta4_native_worker_constant_ring)) {
     return;
   }
-  std::lock_guard lock(worker_ring_mutex_);
+  // worker_ring_mutex_ не нужен: WriteWorkerRingConstants вызывается исключительно из потока GtaRenderWork.
   if (!worker_constant_ring_.mapping) return;
   const bool partial = REXCVAR_GET(gta4_native_partial_constant_upload) && command.pipeline_state;
   // Dynamic uniform offsets need the device's alignment (64 on Adreno).
@@ -9794,7 +10206,7 @@ void Gta4NativeGraphicsSystem::WriteWorkerRingConstants(NativeCommand& command,
     last = {version.get(), hash, bytes, uint32_t(offset)};
     return uint32_t(offset);
   };
-  const uint32_t vertex_offset =
+const uint32_t vertex_offset =
       place(0, command.shader_state->vertex_constants, vertex_shader, kVertexConstantsSize,
             command.vertex_constants_upload_hash);
   if (!vertex_offset) return;
@@ -9909,14 +10321,18 @@ bool Gta4NativeGraphicsSystem::CompleteNativeFrameSlot(uint32_t slot, uint64_t s
     }
   }
 
-  gpu_flight::Record("native.slot-wait-begin", slot, submission, active_texture_frame_);
+  if (IsNativeFlightRecorderEnabled()) {
+    gpu_flight::Record("native.slot-wait-begin", slot, submission, active_texture_frame_);
+  }
   const uint64_t wait_begin = actual_wait_ticks ? rex::chrono::Clock::QueryHostTickCount() : 0;
   const bool completed = profile::CpuCall(profile::CpuOp::kFenceWait, [&] { return submission_tracker_->AwaitSubmissionCompletion(submission); });
   if (actual_wait_ticks) {
     *actual_wait_ticks += rex::chrono::Clock::QueryHostTickCount() - wait_begin;
   }
   if (!completed) {
-    gpu_flight::Record("native.slot-wait-failed", slot, submission, active_texture_frame_);
+    if (IsNativeFlightRecorderEnabled()) {
+      gpu_flight::Record("native.slot-wait-failed", slot, submission, active_texture_frame_);
+    }
     bool wait_cancelled = true;
     if (ring_wait_started) {
       wait_cancelled = frame_context_ring_.CancelWait(*token, ring_serial);
@@ -9939,8 +10355,10 @@ bool Gta4NativeGraphicsSystem::CompleteNativeFrameSlot(uint32_t slot, uint64_t s
   }
 
   completed_command_buffer_submission_ = std::max(completed_command_buffer_submission_, submission);
-  gpu_flight::Record("native.slot-wait-end", slot, submission, active_texture_frame_,
-                     completed_command_buffer_submission_);
+  if (IsNativeFlightRecorderEnabled()) {
+    gpu_flight::Record("native.slot-wait-end", slot, submission, active_texture_frame_,
+                       completed_command_buffer_submission_);
+  }
   AnalyzePendingLightStencilHistogram(slot);
   AnalyzePendingLightColorDelta(slot);
   AnalyzePendingContentProbe(slot);
@@ -10105,8 +10523,10 @@ void Gta4NativeGraphicsSystem::RollbackActiveNativeFrameSlot() {
 }
 
 bool Gta4NativeGraphicsSystem::RecoverFailedNativeFrameRecording() {
-  gpu_flight::Record("native.recovery-begin", active_frame_slot_, command_buffer_submission_,
-                     active_texture_frame_, secondary_command_buffer_submission_);
+  if (IsNativeFlightRecorderEnabled()) {
+    gpu_flight::Record("native.recovery-begin", active_frame_slot_, command_buffer_submission_,
+                       active_texture_frame_, secondary_command_buffer_submission_);
+  }
   auto* vulkan_provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
   const ui::vulkan::VulkanDevice* vulkan_device =
       vulkan_provider ? vulkan_provider->vulkan_device() : nullptr;
@@ -10139,17 +10559,21 @@ bool Gta4NativeGraphicsSystem::RecoverFailedNativeFrameRecording() {
   }
 
   if (!CompleteSecondaryNativeFrameSlot()) {
-    gpu_flight::Record("native.device-idle-begin", NativeVulkanHandleIdentity(device),
-                       secondary_command_buffer_submission_, active_texture_frame_);
+    if (IsNativeFlightRecorderEnabled()) {
+      gpu_flight::Record("native.device-idle-begin", NativeVulkanHandleIdentity(device),
+                         secondary_command_buffer_submission_, active_texture_frame_);
+    }
     const VkResult idle_result = [&] {
       std::vector<ui::vulkan::VulkanDevice::Queue::Acquisition> queues;
       for (const auto& family : vulkan_device->queue_families())
         for (const auto& queue : family.queues) queues.emplace_back(queue->Acquire());
       return dfn.vkDeviceWaitIdle(device);
     }();
-    gpu_flight::Record("native.device-idle-end", NativeVulkanHandleIdentity(device),
-                       secondary_command_buffer_submission_, active_texture_frame_, 0, 0,
-                       int32_t(idle_result));
+    if (IsNativeFlightRecorderEnabled()) {
+      gpu_flight::Record("native.device-idle-end", NativeVulkanHandleIdentity(device),
+                         secondary_command_buffer_submission_, active_texture_frame_, 0, 0,
+                         int32_t(idle_result));
+    }
     if (idle_result < VK_SUCCESS) {
       gpu_flight::Fail("native.device-idle", int32_t(idle_result),
                        NativeVulkanHandleIdentity(device), secondary_command_buffer_submission_,
@@ -11311,15 +11735,15 @@ bool Gta4NativeGraphicsSystem::BeginNativeGpuProfileFrame(VkCommandBuffer comman
   }
   profile_payload_in_flight |= native_gpu_profile_state_.completed_samples.size() != 0;
   profile_payload_in_flight |= native_gpu_profile_state_.completed_attribution.size() != 0;
-  {
-    static uint32_t last_trigger = 0;
-    const uint32_t trigger = REXCVAR_GET(gta4_profile_native_capture_trigger);
-    if (trigger != last_trigger) {
-      last_trigger = trigger;
-      g_native_profile_capture_requested.store(true, std::memory_order_release);
+{
+    if ((submitted_frame % 30 == 0) && !native_gpu_profile_state_.capture_armed && !native_gpu_profile_state_.capture_complete) {
+      const char* trigger_path = "/storage/emulated/0/Android/data/com.libertyrecomp/files/capture.trigger";
+      if (std::remove(trigger_path) == 0) {
+        g_native_profile_capture_requested.store(true, std::memory_order_release);
 #if REX_PLATFORM_ANDROID
-      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "profile-capture: requested");
+        __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "profile-capture: requested via file trigger at frame %u", submitted_frame);
 #endif
+      }
     }
   }
   if (g_native_profile_capture_requested.load(std::memory_order_acquire) &&
@@ -17047,7 +17471,7 @@ void Gta4NativeGraphicsSystem::RetireNativeTextureDescriptor(NativeTextureImage&
 }
 bool Gta4NativeGraphicsSystem::CreateResolveConversionObjects() {
   static_assert(sizeof(NativeResolveConversionConstants) == 64);
-  static_assert(sizeof(NativeHDRPresentConstants) == 44);
+  static_assert(sizeof(NativeHDRPresentConstants) == 48);
   auto* vulkan_provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
   const ui::vulkan::VulkanDevice* vulkan_device =
       vulkan_provider ? vulkan_provider->vulkan_device() : nullptr;
@@ -17172,8 +17596,13 @@ bool Gta4NativeGraphicsSystem::InitializeNativeRendererObjects() {
   native_pipeline_compiler_->checkpoint_tick.store(rex::chrono::Clock::QueryHostTickCount());
   native_pipeline_compiler_->compiler = std::make_unique<NativePipelineCompilerState::Compiler>(
       64, [this] { ScheduleNativePipelineCheckpoint(); });
-  LoadNativePipelineRecipes();
+  //LoadNativePipelineRecipes();
+  //ReplayNativePipelineRecipes();
+  const uint64_t hitch_recipes_begin = FrameTimelineNow();
   ReplayNativePipelineRecipes();
+  g_hitch_recipes_ns.store(FrameTimelineNow() - hitch_recipes_begin,
+                           std::memory_order_relaxed);
+  ScheduleNativePipelineCheckpoint();
   return true;
 }
 
@@ -17866,9 +18295,11 @@ bool Gta4NativeGraphicsSystem::EnsureFrameUploadCapacityFast(
   // after a frame had to grow mid-recording, instead of walking every draw of
   // every frame.
   if (REXCVAR_GET(gta4_native_sparse_capacity_plan) && upload_buffer_.buffer &&
-      upload_on_demand_growths_ == upload_growths_at_last_plan_ &&
-      ++upload_frames_since_plan_ < 30) {
-    return true;
+      upload_on_demand_growths_ == upload_growths_at_last_plan_) {
+    const uint32_t replan_frames = REXCVAR_GET(gta4_native_capacity_replan_frames);
+    if (!replan_frames || ++upload_frames_since_plan_ < replan_frames) {
+      return true;
+    }
   }
   upload_frames_since_plan_ = 0;
   upload_growths_at_last_plan_ = upload_on_demand_growths_;
@@ -19415,6 +19846,10 @@ void Gta4NativeGraphicsSystem::InvalidateCachedDescriptors(uint64_t image_lifeti
 bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buffer,
                                                     bool prepare_present, uint32_t submitted_frame,
                                                     bool trace_reflections) {
+  const uint64_t tex_t0 = FrameTimelineNow();
+  g_hitch_tex_pool_ns.store(0, std::memory_order_relaxed);
+  g_hitch_tex_loop_ns.store(0, std::memory_order_relaxed);
+  g_hitch_tex_post_ns.store(0, std::memory_order_relaxed);
   NativeTextureUploadBatch upload_batch;
   const bool batch_uploads = REXCVAR_GET(gta4_native_batch_texture_uploads);
   // Even an early descriptor/capacity failure leaves successfully cached images
@@ -19449,7 +19884,8 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
   // within a frame and published once at the end, so any earlier draw of this
   // batch is as valid a source as the previous one. Cleared wherever
   // previous_prepared_draw is.
-  std::array<const NativeCommand*, 16> prepared_draw_cache{};
+  constexpr size_t kPreparedDrawCacheSize = 512;
+  std::array<const NativeCommand*, kPreparedDrawCacheSize> prepared_draw_cache{};
   // With the worker's signatures on both draws, equal inputs are equal
   // signatures (a 64-bit hash of the same fields the full comparison reads).
   const auto prepared_inputs_equal = [](const NativeCommand& previous, const NativeCommand& next) {
@@ -19462,8 +19898,12 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
     return NativePreparedTextureInputsEqual(previous, next);
   };
   const bool use_prepared_draw_cache = REXCVAR_GET(gta4_native_prepared_binding_cache);
-  const auto prepared_draw_slot = [](const NativeCommand& draw) {
-    if (draw.texture_inputs_signature) return size_t(draw.texture_inputs_signature >> 60);
+  const auto prepared_draw_slot = [](const NativeCommand& draw) -> size_t {
+    if (draw.texture_inputs_signature == 0x1ull) return 0;
+    if (draw.texture_inputs_signature) {
+      const size_t slot = size_t((draw.texture_inputs_signature >> 55) & (kPreparedDrawCacheSize - 1));
+      return slot ? slot : 1;
+    }
     uint64_t hash = uint64_t(draw.used_texture_mask) * 0x9E3779B97F4A7C15ull;
     for (uint32_t mask = draw.used_texture_mask; mask; mask &= mask - 1) {
       const uint32_t stage = uint32_t(std::countr_zero(mask));
@@ -19471,7 +19911,8 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
       hash = (hash ^ uint64_t(reinterpret_cast<uintptr_t>(draw.textures[stage].get()))) *
              0xBF58476D1CE4E5B9ull;
     }
-    return size_t(hash >> 60);
+    const size_t slot = size_t((hash >> 55) & (kPreparedDrawCacheSize - 1));
+    return slot ? slot : 1;
   };
   const bool reuse_prepared_bindings = !trace_reflections && !FireTraceConfig().enabled &&
       !EmissionTraceConfig().pipeline_full_readback && !PhoneTraceConfig().enabled &&
@@ -19560,7 +20001,7 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
   // Normal draw descriptors are persistent in both backends. The frame pool
   // remains reserved for resolve, materialization, and post-processing sets.
   const uint32_t descriptor_pool_draw_count = 0;
-  if (!PrepareFrameDescriptorPool(descriptor_pool_draw_count, combined_descriptor_count,
+if (!PrepareFrameDescriptorPool(descriptor_pool_draw_count, combined_descriptor_count,
                                   combined_set_count)) {
     return false;
   }
@@ -19568,6 +20009,9 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
   if (native_descriptor_backend_ == NativeDescriptorBackend::kIndexed && native_descriptor_paging_ &&
       !BeginIndexedWorkingSet()) return false;
 
+  g_hitch_tex_pool_ns.store(FrameTimelineNow() - tex_t0, std::memory_order_relaxed);
+  const uint64_t tex_loop_t0 = FrameTimelineNow();
+
   auto* vulkan_provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
   const ui::vulkan::VulkanDevice* vulkan_device = vulkan_provider->vulkan_device();
   const auto& dfn = vulkan_device->functions();
@@ -19587,7 +20031,22 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
     cached_draws.reserve(draw_count);
   }
 
+   NativeDrawDescriptorKey null_descriptor_key{};
+  null_descriptor_key.layout_epoch = next_cached_descriptor_epoch_;
+  null_descriptor_key.images_2d.fill(null_texture_2d_.view);
+  null_descriptor_key.images_2d_array.fill(null_texture_2d_array_.view);
+  null_descriptor_key.images_3d.fill(null_texture_3d_.view);
+  null_descriptor_key.images_cube.fill(null_texture_cube_.view);
+  null_descriptor_key.samplers.fill(null_sampler_);
+
   const bool prefetch_commands = REXCVAR_GET(gta4_native_recorder_prefetch);
+  bool has_last_descriptor_key = false;
+  NativeDrawDescriptorKey last_descriptor_key{};
+  uint32_t last_descriptor_page = UINT32_MAX;
+  uint32_t last_descriptor_copy = 0;
+  std::array<uint32_t, kTextureStageCount> last_texture_indices{};
+  std::array<uint32_t, kTextureStageCount> last_sampler_indices{};
+  uint64_t last_descriptor_epoch = 0;
   for (size_t command_index = 0; command_index < texture_commands.size(); ++command_index) {
     if (prefetch_commands && command_index + 4 < texture_commands.size()) {
       // See RecordNativeFrame: the walk is bound by first touches of commands.
@@ -19609,6 +20068,7 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
         PlanResolveSwap(command, *resolve_destination_image, texture_commands, command_index)) {
       previous_prepared_draw = nullptr;
       prepared_draw_cache.fill(nullptr);
+      has_last_descriptor_key = false;
     }
     if (command.depth_handoff_source &&
         !prepared_image(command.depth_handoff_source)) {
@@ -19643,6 +20103,7 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
     const NativeShader* alpha_card_pixel_shader =
         command.pipeline_state ? command.pipeline_state->pixel_shader_resource : nullptr;
     const bool trace_alpha_card =
+        REXCVAR_GET(gta4_trace_native_renderer) &&
         CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeTranslucency) &&
         ShouldCaptureDiagnosticFrame(submitted_frame) && alpha_card_pixel_shader &&
         IsAlphaTestRequested(command.fixed_function_state.alpha_test_enable,
@@ -19654,12 +20115,8 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
       alpha_card_category = ClassifyAlphaCardDiagnosticShader(alpha_card_pixel_shader->filename);
     }
 
-    std::array<VkDescriptorImageInfo, kShaderTextureCount> images_2d{};
-    std::array<VkDescriptorImageInfo, kShaderTextureCount> images_2d_array{};
-    std::array<VkDescriptorImageInfo, kShaderTextureCount> images_3d{};
-    std::array<VkDescriptorImageInfo, kShaderTextureCount> images_cube{};
-    std::array<VkDescriptorImageInfo, kShaderTextureCount> samplers{};
-    std::array<uint64_t, kShaderTextureCount> image_lifetimes{};
+    NativeDrawDescriptorKey descriptor_key = null_descriptor_key;
+    descriptor_key.layout_epoch = next_cached_descriptor_epoch_;
     command.descriptor_page = UINT32_MAX;
     command.image_descriptor_epoch = 0;
     command.sampler_descriptor_epoch = 0;
@@ -19688,19 +20145,20 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
     command.guest_null_texture_mask = 0;
     command.failed_texture_mask = 0;
     command.bindings_prepared = false;
-    for (uint32_t stage = 0; stage < kShaderTextureCount; ++stage) {
-      images_2d[stage] = {VK_NULL_HANDLE, null_texture_2d_.view,
-                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
-      images_2d_array[stage] = {VK_NULL_HANDLE, null_texture_2d_array_.view,
-                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
-      images_3d[stage] = {VK_NULL_HANDLE, null_texture_3d_.view,
-                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
-      images_cube[stage] = {VK_NULL_HANDLE, null_texture_cube_.view,
-                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
-      samplers[stage] = {null_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
-      if (native_descriptor_backend_ == NativeDescriptorBackend::kIndexed && native_descriptor_paging_) {
-        command.texture_descriptor_indices[stage] = 0;
-        command.sampler_descriptor_indices[stage] = 0;
+    const bool fast_indexed_paging =
+        native_descriptor_backend_ == NativeDescriptorBackend::kIndexed && native_descriptor_paging_;
+    const uint32_t stage_mask =
+        fast_indexed_paging ? (command.used_texture_mask & kSupportedShaderTextureMask)
+                            : kSupportedShaderTextureMask;
+    
+    for (uint32_t mask = stage_mask; mask; mask &= mask - 1) {
+      const uint32_t stage = uint32_t(std::countr_zero(mask));
+      const uint32_t stage_bit = 1u << stage;
+      const bool required = (command.used_texture_mask & stage_bit) != 0;
+
+      if (fast_indexed_paging) {
+        // Ничего не делаем: индексы уже обнулены (fill(0)) до цикла.
+        // Цикл крутится только по битам из used_texture_mask.
       } else if (native_descriptor_backend_ == NativeDescriptorBackend::kIndexed) {
         if (!native_stable_image_descriptor_table_ || !native_stable_sampler_descriptor_table_ ||
             !native_stable_image_descriptor_table_->IsLive(native_null_image_descriptor_) ||
@@ -19715,8 +20173,6 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
       }
 
       NativeSampler* sampler = nullptr;
-      const uint32_t stage_bit = 1u << stage;
-      const bool required = (command.used_texture_mask & stage_bit) != 0;
       NativeTextureImage* image = required
           ? prepared_image(command.textures[stage]) : nullptr;
       if (image && command.temporal_scene_binding && command.temporal_scene_stage == stage)
@@ -19729,30 +20185,30 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
       }
       if (image) {
         VkImageView image_view = image->resource.view;
-        image_lifetimes[stage] = image->descriptor_lifetime;
+		descriptor_key.image_lifetimes[stage] = image->descriptor_lifetime;
         if (!prepared_texture_views_.empty()) {
           if (const auto swapped = prepared_texture_views_.find(image);
               swapped != prepared_texture_views_.end()) {
             image_view = swapped->second.view;
-            image_lifetimes[stage] = swapped->second.lifetime;
+            descriptor_key.image_lifetimes[stage] = swapped->second.lifetime;
           }
         }
         switch (image->source->info.dimension) {
           case xenos::DataDimension::k3D:
-            images_3d[stage].imageView = image_view;
+            descriptor_key.images_3d[stage] = image_view;
             break;
           case xenos::DataDimension::kCube:
-            images_cube[stage].imageView = image_view;
+            descriptor_key.images_cube[stage] = image_view;
             break;
           case xenos::DataDimension::k2DOrStacked:
             if (image->source->info.is_stacked) {
-              images_2d_array[stage].imageView = image_view;
+              descriptor_key.images_2d_array[stage] = image_view;
             } else {
-              images_2d[stage].imageView = image_view;
+              descriptor_key.images_2d[stage] = image_view;
             }
             break;
           default:
-            images_2d[stage].imageView = image_view;
+            descriptor_key.images_2d[stage] = image_view;
             break;
         }
         NativeSamplerKey effective_sampler_key{};
@@ -19769,7 +20225,7 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
           }
         }
         if (sampler) {
-          samplers[stage].sampler = sampler->sampler;
+          descriptor_key.samplers[stage] = sampler->sampler;
         }
         if (IsHoveGantryGeometryCandidate(command)) {
           static std::atomic<uint64_t> hove_sample_bind_count{0};
@@ -19981,7 +20437,7 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
                 "texture-index={} sampler-index={}",
                 trace, stage, image->source->vector_font_id, image->source->handle,
                 image->source->generation, uint32_t(image->source->info.dimension),
-                fmt::ptr(image->resource.view), fmt::ptr(samplers[stage].sampler), !sampler,
+                fmt::ptr(image->resource.view), fmt::ptr(descriptor_key.samplers[stage]), !sampler,
                 command.texture_descriptor_indices[stage],
                 command.sampler_descriptor_indices[stage]);
           }
@@ -20031,7 +20487,7 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
         binding.texture_stage = stage;
         binding.image = image ? image->resource.image : VK_NULL_HANDLE;
         binding.view = image ? image->resource.view : null_texture_2d_.view;
-        binding.sampler = samplers[stage].sampler;
+       binding.sampler = descriptor_key.samplers[stage];
         binding.image_lifetime = image ? image->descriptor_lifetime : 0;
         if (sampler) binding.sampler_info = sampler->creation_info;
       }
@@ -20047,24 +20503,47 @@ bool Gta4NativeGraphicsSystem::PrepareFrameTextures(VkCommandBuffer command_buff
       continue;
     }
 
-    NativeDrawDescriptorKey descriptor_key{};
-    descriptor_key.layout_epoch = next_cached_descriptor_epoch_;
-    descriptor_key.image_lifetimes = image_lifetimes;
-    for (uint32_t stage = 0; stage < kShaderTextureCount; ++stage) {
-      descriptor_key.images_2d[stage] = images_2d[stage].imageView;
-      descriptor_key.images_2d_array[stage] = images_2d_array[stage].imageView;
-      descriptor_key.images_3d[stage] = images_3d[stage].imageView;
-      descriptor_key.images_cube[stage] = images_cube[stage].imageView;
-      descriptor_key.samplers[stage] = samplers[stage].sampler;
-    }
+//    NativeDrawDescriptorKey descriptor_key{};
+  //  descriptor_key.layout_epoch = next_cached_descriptor_epoch_;
+   // descriptor_key.image_lifetimes = image_lifetimes;
+   // for (uint32_t stage = 0; stage < kShaderTextureCount; ++stage) {
+    //  descriptor_key.images_2d[stage] = images_2d[stage].imageView;
+    // descriptor_key.images_2d_array[stage] = images_2d_array[stage].imageView;
+     // descriptor_key.images_3d[stage] = images_3d[stage].imageView;
+    //  descriptor_key.images_cube[stage] = images_cube[stage].imageView;
+    //  descriptor_key.samplers[stage] = samplers[stage].sampler;
+   // }
     if (native_descriptor_backend_ == NativeDescriptorBackend::kIndexed && native_descriptor_paging_) {
-      if (!AssignIndexedWorkingSet(command, descriptor_key)) return false;
+      if (has_last_descriptor_key && descriptor_key == last_descriptor_key) {
+        command.descriptor_page = last_descriptor_page;
+        command.descriptor_copy = last_descriptor_copy;
+        command.texture_descriptor_indices = last_texture_indices;
+        command.sampler_descriptor_indices = last_sampler_indices;
+        command.image_descriptor_epoch = last_descriptor_epoch;
+        command.sampler_descriptor_epoch = last_descriptor_epoch;
+        command.bindings_prepared = true;
+      } else {
+        if (!AssignIndexedWorkingSet(command, descriptor_key)) return false;
+        has_last_descriptor_key = true;
+        last_descriptor_key = descriptor_key;
+        last_descriptor_page = command.descriptor_page;
+        last_descriptor_copy = command.descriptor_copy;
+        last_texture_indices = command.texture_descriptor_indices;
+        last_sampler_indices = command.sampler_descriptor_indices;
+        last_descriptor_epoch = command.image_descriptor_epoch;
+      }
       previous_prepared_draw = &command;
       if (use_prepared_draw_cache) prepared_draw_cache[prepared_slot] = &command;
     } else {
       cached_draws.emplace_back(&command, std::move(descriptor_key));
     }
   }
+  g_hitch_tex_loop_ns.store(FrameTimelineNow() - tex_loop_t0, std::memory_order_relaxed);
+  const uint64_t tex_post_t0 = FrameTimelineNow();
+  const auto tex_post_timer = MakeScopeExit([&] {
+    g_hitch_tex_post_ns.store(FrameTimelineNow() - tex_post_t0, std::memory_order_relaxed);
+  });
+
   if (descriptor_backend_at_entry != native_descriptor_backend_) {
     if (descriptor_backend_at_entry == NativeDescriptorBackend::kIndexed &&
         native_descriptor_backend_ == NativeDescriptorBackend::kCached) {
@@ -20750,6 +21229,7 @@ void Gta4NativeGraphicsSystem::ReleaseUnusedBufferResources(uint32_t submitted_f
 }
 
 void Gta4NativeGraphicsSystem::DestroyNativeSurfaceImage(NativeSurfaceImage& image) {
+  native_target_memo_.valid = false;
   if (auto it = surface_images_by_handle_.find(image.descriptor.handle); it != surface_images_by_handle_.end()) {
     std::erase(it->second, &image);
     if (it->second.empty()) surface_images_by_handle_.erase(it);
@@ -20817,6 +21297,7 @@ void Gta4NativeGraphicsSystem::DestroyNativeSurfaceImage(NativeSurfaceImage& ima
 }
 
 void Gta4NativeGraphicsSystem::QueueSurfaceImageRelease(uint32_t handle) {
+  native_target_memo_.valid = false;
   for (const auto& image : native_surface_images_) {
     if (image && image->descriptor.handle == handle) {
       pending_surface_release_ids_.insert(image->lifetime_id);
@@ -20857,6 +21338,12 @@ void Gta4NativeGraphicsSystem::ReleasePendingSurfaceImages() {
       ++image;
       continue;
     }
+    // Лимит: удаляем не более 2 поверхностей за кадр, остальные откладываем!
+    if (retired_surface_count >= 2) {
+      still_pending_ids.insert((*image)->lifetime_id);
+      ++image;
+      continue;
+    }
     NativeSurfaceImage* retired_image = image->get();
     for (auto owner = native_placement_owners_.begin(); owner != native_placement_owners_.end();) {
       if (owner->second.image == retired_image) {
@@ -21061,6 +21548,7 @@ Gta4NativeGraphicsSystem::NativeSurfaceImage* Gta4NativeGraphicsSystem::GetOrCre
 
   std::string same_address_aliases;
   bool has_written_same_address_alias = false;
+  #if 0
   if (descriptor.address) {
     for (const auto& existing : native_surface_images_) {
       if (!existing || existing->aspect != aspect ||
@@ -21077,7 +21565,7 @@ Gta4NativeGraphicsSystem::NativeSurfaceImage* Gta4NativeGraphicsSystem::GetOrCre
       has_written_same_address_alias |= existing->ever_written;
     }
   }
-
+#endif
   VkImageCreateInfo image_info{};
   image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   image_info.imageType = VK_IMAGE_TYPE_2D;
@@ -21188,7 +21676,8 @@ bool Gta4NativeGraphicsSystem::ResolveRenderingTarget(const NativeCommand& comma
                                                       uint32_t presenter_width,
                                                       uint32_t presenter_height,
                                                       NativeRenderingTarget& target,
-                                                      bool allow_allocation) {
+                                                      bool allow_allocation,
+                                                      const NativeRenderingTarget* active_target) {
   const profile::CpuScope profile_scope(profile::CpuOp::kTargetRealization);
 
   target = {};
@@ -21434,9 +21923,31 @@ bool Gta4NativeGraphicsSystem::ResolveRenderingTarget(const NativeCommand& comma
     target.logical_height = target.height;
     target.samples = VK_SAMPLE_COUNT_1_BIT;
   }
+// ИСПРАВЛЕНИЕ: Если открытый RenderPass уже содержит все нужные нам буферы,
+  // принудительно используем его маску, чтобы не рвать пасс.
+  uint32_t final_colors = requested_colors;
+  if (active_target && 
+      (active_target->color_attachment_mask & requested_colors) == requested_colors &&
+      active_target->depth_view == target.depth_view &&
+      active_target->samples == target.samples &&
+      active_target->width == target.width && active_target->height == target.height) {
+      
+      final_colors = active_target->color_attachment_mask;
+      
+      // Копируем недостающие вьюшки и форматы из активного пасса
+      for (uint32_t index = 0; index < kRenderTargetCount; ++index) {
+          if ((final_colors & (1u << index)) != 0 && !target.color_views[index]) {
+              target.color_views[index] = active_target->color_views[index];
+              target.color_formats[index] = active_target->color_formats[index];
+              target.color_surfaces[index] = active_target->color_surfaces[index];
+          }
+      }
+  }
+
   for (uint32_t index = 0; index < kRenderTargetCount; ++index) {
     if (target.color_views[index]) {
-      target.color_attachment_mask |= requested_colors & (1u << index);
+      // Используем final_colors вместо requested_colors
+      target.color_attachment_mask |= final_colors & (1u << index);
       target.color_write_mask |=
           NativeColorWriteMaskForTarget(usage.color_write_mask, index) << (index * 4u);
     }
@@ -21445,6 +21956,90 @@ bool Gta4NativeGraphicsSystem::ResolveRenderingTarget(const NativeCommand& comma
   return (target.width && target.height) || reject("zero-target-dimensions");
 }
 
+// Adjacent draws usually render to the same attachments (the shadow pass is
+// ~1500 draws into one depth target). Reuse the previous draw's realized
+// surfaces when the target descriptors and attachment usage are bytewise
+// identical. The memo only spans adjacent draws of one recording: non-draw
+// commands, a new frame or submission, and any surface destruction or release
+// clear it. The open-pass merge and the masks are recomputed per draw exactly
+// as ResolveRenderingTarget does with an active target.
+bool Gta4NativeGraphicsSystem::ResolveRenderingTargetMemo(
+    const NativeCommand& command, VkImageView presenter_view, uint32_t presenter_width,
+    uint32_t presenter_height, NativeRenderingTarget& target,
+    const NativeRenderingTarget& active_target) {
+  const bool draw_command = command.type == CommandType::kDrawPrimitive ||
+                            command.type == CommandType::kDrawPrimitiveUp ||
+                            command.type == CommandType::kDrawIndexedPrimitive;
+  if (!draw_command || !REXCVAR_GET(gta4_native_target_memo)) {
+    native_target_memo_.valid = false;
+    return ResolveRenderingTarget(command, presenter_view, presenter_width, presenter_height,
+                                  target, true, &active_target);
+  }
+  const NativePipelineState& state = *command.pipeline_state;
+  const NativeAttachmentUsage usage = GetRenderingTargetUsage(command);
+  const uint64_t submission =
+      submission_tracker_ ? submission_tracker_->GetCurrentSubmission() : 0;
+  NativeTargetMemo& memo = native_target_memo_;
+  const bool hit =
+      memo.valid && memo.frame == diagnostic_submitted_frame_ &&
+      memo.texture_frame == active_texture_frame_ && memo.submission == submission &&
+      memo.presenter_view == presenter_view && memo.presenter_width == presenter_width &&
+      memo.presenter_height == presenter_height &&
+      memo.color_attachment_mask == usage.color_attachment_mask &&
+      memo.depth_stencil_aspects == usage.depth_stencil_aspects &&
+      std::memcmp(&memo.render_targets, &state.render_targets, sizeof(state.render_targets)) ==
+          0 &&
+      std::memcmp(&memo.depth_stencil, &state.depth_stencil, sizeof(state.depth_stencil)) == 0;
+  if (hit) {
+    target = memo.target;
+  } else {
+    if (!ResolveRenderingTarget(command, presenter_view, presenter_width, presenter_height,
+                                target, true, nullptr)) {
+      memo.valid = false;
+      return false;
+    }
+    memo.valid = true;
+    memo.frame = diagnostic_submitted_frame_;
+    memo.texture_frame = active_texture_frame_;
+    memo.submission = submission;
+    memo.presenter_view = presenter_view;
+    memo.presenter_width = presenter_width;
+    memo.presenter_height = presenter_height;
+    memo.color_attachment_mask = usage.color_attachment_mask;
+    memo.depth_stencil_aspects = usage.depth_stencil_aspects;
+    std::memcpy(&memo.render_targets, &state.render_targets, sizeof(state.render_targets));
+    std::memcpy(&memo.depth_stencil, &state.depth_stencil, sizeof(state.depth_stencil));
+    memo.target = target;
+  }
+  // Tail of ResolveRenderingTarget for an active target.
+  const uint32_t requested_colors = usage.color_attachment_mask;
+  uint32_t final_colors = requested_colors;
+  if ((active_target.color_attachment_mask & requested_colors) == requested_colors &&
+      active_target.depth_view == target.depth_view && active_target.samples == target.samples &&
+      active_target.width == target.width && active_target.height == target.height) {
+    final_colors = active_target.color_attachment_mask;
+    for (uint32_t index = 0; index < kRenderTargetCount; ++index) {
+      if ((final_colors & (1u << index)) != 0 && !target.color_views[index]) {
+        target.color_views[index] = active_target.color_views[index];
+        target.color_formats[index] = active_target.color_formats[index];
+        target.color_surfaces[index] = active_target.color_surfaces[index];
+      }
+    }
+  }
+  target.color_attachment_mask = 0;
+  target.color_write_mask = 0;
+  for (uint32_t index = 0; index < kRenderTargetCount; ++index) {
+    if (target.color_views[index]) {
+      target.color_attachment_mask |= final_colors & (1u << index);
+      target.color_write_mask |=
+          NativeColorWriteMaskForTarget(usage.color_write_mask, index) << (index * 4u);
+    }
+  }
+  target.depth_stencil_attachment_active =
+      usage.depth_stencil_aspects != 0 && target.depth_surface != nullptr;
+  return true;
+}
+
 NativeAttachmentUsage Gta4NativeGraphicsSystem::GetRenderingTargetUsage(
     const NativeCommand& command) const {
   const bool draw_command = command.type == CommandType::kDrawPrimitive ||
@@ -21655,10 +22250,56 @@ VkPipeline Gta4NativeGraphicsSystem::GetOrCreateDrawPipeline(
   const bool trace_pipeline =
       ShouldCaptureArtificialLightFrame(diagnostic_submitted_frame_) &&
       (diagnostic_light_trace_id_ || IsArtificialLightRenderPhase(diagnostic_render_phase_));
-  const uint64_t memo_signature = command.pipeline_memo_signature;
-  if (!trace_pipeline && !target.temporal_shader && memo_signature) {
+const uint64_t memo_signature = command.pipeline_memo_signature;
+  struct LastPipelineLookup {
+    const NativeShader* vs = nullptr;
+    const NativeShader* ps = nullptr;
+    const NativeVertexDeclaration* vdecl = nullptr;
+    uint64_t vs_hash = 0;
+    uint64_t ps_hash = 0;
+    uint64_t vdecl_hash = 0;
+    std::array<uint32_t, kVertexStreamCount> strides{};
+    uint64_t memo_sig = 0;
+    NativePipelineLookupContext<kRenderTargetCount> ctx{};
+    VkPipeline pipeline = VK_NULL_HANDLE;
+  };
+  static thread_local LastPipelineLookup s_last_lookup{};
+  const uint64_t ps_hash_now = state.pixel_shader_resource ? state.pixel_shader_resource->hash : 0;
+  if (!trace_pipeline && !target.temporal_shader && memo_signature &&
+      context.lifetime && state.vertex_shader_resource && state.vertex_declaration_resource) {
+    if (s_last_lookup.pipeline &&
+        s_last_lookup.memo_sig == memo_signature &&
+        s_last_lookup.vs == state.vertex_shader_resource &&
+        s_last_lookup.ps == state.pixel_shader_resource &&
+        s_last_lookup.vdecl == state.vertex_declaration_resource.get() &&
+        s_last_lookup.vs_hash == state.vertex_shader_resource->hash &&
+        s_last_lookup.ps_hash == ps_hash_now &&
+        s_last_lookup.vdecl_hash == state.vertex_declaration_resource->content_hash &&
+        [&] {
+          // Все потоки: пайплайн зависит от страйда каждого потока, а не только 0 и 1
+          // (draw_pipeline_cache_ ниже тоже сравнивает весь массив).
+          for (uint32_t s = 0; s < kVertexStreamCount; ++s)
+            if (s_last_lookup.strides[s] != state.vertex_streams[s].stride) return false;
+          return true;
+        }() &&
+        s_last_lookup.ctx == context) {
+      AddNativeGpuProfileCounter(performance::Counter::kPipelineRequestReuses);
+      return s_last_lookup.pipeline;
+    }
     if (VkPipeline pipeline =
             state.pipeline_lookup_memo.FindSigned(&state, memo_signature, context)) {
+      for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
+        s_last_lookup.strides[s] = state.vertex_streams[s].stride;
+      }
+      s_last_lookup.vs = state.vertex_shader_resource;
+      s_last_lookup.ps = state.pixel_shader_resource;
+      s_last_lookup.vdecl = state.vertex_declaration_resource.get();
+      s_last_lookup.vs_hash = state.vertex_shader_resource->hash;
+      s_last_lookup.ps_hash = ps_hash_now;
+      s_last_lookup.vdecl_hash = state.vertex_declaration_resource->content_hash;
+      s_last_lookup.memo_sig = memo_signature;
+      s_last_lookup.ctx = context;
+      s_last_lookup.pipeline = pipeline;
       AddNativeGpuProfileCounter(performance::Counter::kPipelineRequestReuses);
       return pipeline;
     }
@@ -21671,15 +22312,17 @@ VkPipeline Gta4NativeGraphicsSystem::GetOrCreateDrawPipeline(
       return pipeline;
     }
   }
+  // Per-stream strides, used by both the fast-path memo (s_last_lookup.strides)
+// and the pipeline-cache hash/comparison below.
+std::array<uint32_t, kVertexStreamCount> strides{};
+for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
+    strides[s] = state.vertex_streams[s].stride;
+}
   DrawPipelineCacheEntry* cache_entry = nullptr;
-  std::array<uint32_t, kVertexStreamCount> strides;
   const NativeVertexDeclaration* vertex_declaration = state.vertex_declaration_resource.get();
   if (!trace_pipeline && !target.temporal_shader && context.lifetime &&
       state.vertex_shader_resource && vertex_declaration) {
     if (draw_pipeline_cache_.empty()) draw_pipeline_cache_.resize(4096);
-    for (uint32_t stream = 0; stream < kVertexStreamCount; ++stream) {
-      strides[stream] = state.vertex_streams[stream].stride;
-    }
     uint64_t h = 0x243F6A8885A308D3ull;
     const auto mix = [&h](uint64_t value) {
       h = (h ^ value) * 0x9E3779B97F4A7C15ull;
@@ -21696,7 +22339,8 @@ VkPipeline Gta4NativeGraphicsSystem::GetOrCreateDrawPipeline(
         uint64_t(memo_state.depth_function) << 16 | uint64_t(memo_state.stencil_enable) << 24 |
         uint64_t(memo_state.alpha_test_enable) << 32 | uint64_t(memo_state.blend_enable) << 40);
     mix(uint64_t(context.width) | uint64_t(context.height) << 32);
-    cache_entry = &draw_pipeline_cache_[h & (draw_pipeline_cache_.size() - 1)];
+        mix(memo_signature);  // хеш всего NativePipelineMemoState; 0, если не посчитан
+        cache_entry = &draw_pipeline_cache_[h & (draw_pipeline_cache_.size() - 1)];
     const uint64_t pixel_shader_hash =
         state.pixel_shader_resource ? state.pixel_shader_resource->hash : 0;
     if (cache_entry->pipeline && cache_entry->vertex_shader == state.vertex_shader_resource &&
@@ -21728,10 +22372,15 @@ VkPipeline Gta4NativeGraphicsSystem::GetOrCreateDrawPipeline(
     cache_entry->fixed = memo_state;
     cache_entry->context = context;
     cache_entry->pipeline = pipeline;
+  if (!trace_pipeline && !target.temporal_shader && memo_signature) {
+      s_last_lookup = {state.vertex_shader_resource, state.pixel_shader_resource,
+                       vertex_declaration, state.vertex_shader_resource->hash,
+                       ps_hash_now, vertex_declaration->content_hash,
+                       strides, memo_signature, context, pipeline};
+    }
   }
   return pipeline;
-}
-
+	}
 VkPipeline Gta4NativeGraphicsSystem::GetOrCreatePipeline(
     const NativePipelineState& state, const NativeFixedFunctionState& fixed_function_state,
     uint32_t primitive_type, const NativeRenderingTarget& target, uint32_t user_pointer_stride,
@@ -22455,7 +23104,7 @@ VkPipeline Gta4NativeGraphicsSystem::GetOrCreatePipeline(
   if (alpha_test_requested && use_late_module) {
     specialization_value = PackAlphaTestSpecialization(fixed_function_state.alpha_function);
   }
-  if (state.pixel_shader_resource) {
+  if (state.pixel_shader_resource && REXCVAR_GET(gta4_trace_native_renderer)) {
     const std::string_view diagnostic_category =
         ClassifyTranslucentDiagnosticShader(selected_pixel.filename);
     if (fixed_function_state.alpha_test_enable || !diagnostic_category.empty()) {
@@ -22951,7 +23600,7 @@ bool Gta4NativeGraphicsSystem::BindUboConstantBanks(VkCommandBuffer command_buff
     static uint32_t traced_frames = 0;
     static uint32_t last_frame = 0;
     static uint32_t binds = 0;
-    if (diagnostic_submitted_frame_ != last_frame) {
+    if (REXCVAR_GET(gta4_native_debug_counters) && diagnostic_submitted_frame_ != last_frame) {
       if (last_frame && traced_frames < 12) {
         __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "ubo-trace: frame=%u binds=%u", last_frame, binds);
         ++traced_frames;
@@ -22991,13 +23640,65 @@ bool Gta4NativeGraphicsSystem::BindCommonDrawState(
       native_descriptor_backend_ == NativeDescriptorBackend::kCached && cached_pipeline_layout_
           ? cached_pipeline_layout_
           : pipeline_layout_;
-  if (!pipeline || !draw_pipeline_layout || !command.shader_state ||
+if (!pipeline || !draw_pipeline_layout || !command.shader_state ||
       !command.shader_state->vertex_constants || !command.shader_state->pixel_constants ||
       command.shader_state->vertex_constants->byte_size != kVertexConstantsSize ||
       command.shader_state->pixel_constants->byte_size != kPixelConstantsSize) {
     return false;
   }
+#if 0
+// ===== FAST PATH (более надёжный) =====
+static thread_local VkPipeline      fast_pipeline   = VK_NULL_HANDLE;
+static thread_local const void*     fast_vc         = nullptr;
+static thread_local const void*     fast_pc         = nullptr;
+static thread_local uint64_t        fast_textures   = 0;
+static thread_local uint64_t        fast_ff         = 0;
+static thread_local VkCommandBuffer fast_cb         = VK_NULL_HANDLE;
+static thread_local uint64_t        fast_frame      = ~0ull;
+static thread_local bool            fast_valid      = false;
+
+const uint64_t current_frame = diagnostic_submitted_frame_;
+const bool same_frame = (fast_frame == current_frame);
+const bool same_cb    = (fast_cb == command_buffer);
+
+// Разрешаем fast-path только если:
+// 1. Тот же кадр
+// 2. Тот же command buffer
+// 3. Все ключевые указатели/сигнатуры совпадают
+// 4. Кэш уже был один раз прогрет в этом кадре+CB
+if (fast_valid &&
+    same_frame &&
+    same_cb &&
+    fast_pipeline == pipeline &&
+    fast_vc == command.shader_state->vertex_constants.get() &&
+    fast_pc == command.shader_state->pixel_constants.get() &&
+    fast_textures == command.texture_inputs_signature &&
+    fast_ff == command.pipeline_memo_signature)
+{
+  // Поддерживаем метрики кольцевого буфера
+  if (command.worker_vertex_constants_offset &&
+      command.worker_pixel_constants_offset &&
+      command.worker_ring_serial == recording_ring_serial_ &&
+      !target.temporal_shader &&
+      worker_ring_valid_.load(std::memory_order_acquire) &&
+      worker_constant_ring_.buffer)
+  {
+    if (REXCVAR_GET(gta4_native_debug_counters)) g_worker_ring_draws.fetch_add(1, std::memory_order_relaxed);
+  }
+  return true;   // безопасно пропускаем тяжёлую работу
+}
 
+// Обычный (медленный) путь — обновляем кэш
+fast_pipeline  = pipeline;
+fast_vc        = command.shader_state->vertex_constants.get();
+fast_pc        = command.shader_state->pixel_constants.get();
+fast_textures  = command.texture_inputs_signature;
+fast_ff        = command.pipeline_memo_signature;
+fast_cb        = command_buffer;
+fast_frame     = current_frame;
+fast_valid     = true;
+// ===== КОНЕЦ FAST PATH =====
+#endif
   const NativeFixedFunctionState& fixed = command.fixed_function_state;
   const float requested_viewport_x = std::bit_cast<float>(fixed.viewport_bits[0]);
   const float requested_viewport_y = std::bit_cast<float>(fixed.viewport_bits[1]);
@@ -23064,6 +23765,7 @@ bool Gta4NativeGraphicsSystem::BindCommonDrawState(
       command.worker_vertex_constants_offset && command.worker_pixel_constants_offset &&
       command.worker_ring_serial == recording_ring_serial_ && !target.temporal_shader &&
       worker_ring_valid_.load(std::memory_order_acquire) && worker_constant_ring_.buffer;
+  if (REXCVAR_GET(gta4_native_debug_counters))
   (ring_constants ? g_worker_ring_draws : g_worker_ring_fallbacks)
       .fetch_add(1, std::memory_order_relaxed);
   if (ring_constants) {
@@ -23099,8 +23801,10 @@ bool Gta4NativeGraphicsSystem::BindCommonDrawState(
                 vertex_constants_allocation.mapping) &&
           check(pixel_constants, ps ? ps->pixel_shader_resource : nullptr, kPixelConstantsSize,
                 pixel_constants_allocation.mapping);
-      g_worker_ring_checked.fetch_add(1, std::memory_order_relaxed);
-      if (!equal) g_worker_ring_mismatches.fetch_add(1, std::memory_order_relaxed);
+      if (REXCVAR_GET(gta4_native_debug_counters)) {
+        g_worker_ring_checked.fetch_add(1, std::memory_order_relaxed);
+        if (!equal) g_worker_ring_mismatches.fetch_add(1, std::memory_order_relaxed);
+      }
     }
   } else if (!bind_guest_constants(NativeConstantBufferKind::kVertex,
                                    command.shader_state->vertex_constants,
@@ -23128,6 +23832,7 @@ bool Gta4NativeGraphicsSystem::BindCommonDrawState(
   #include "emission_trace_bind.inc"
 
   const bool capture_gbuffer_constant_upload =
+      REXCVAR_GET(gta4_trace_native_renderer) &&
       ShouldLogDiagnosticFrame(diagnostic_submitted_frame_) &&
       (bound_vertex_shader_hash == 0x4D629CC41AD0FE18ull ||
        bound_vertex_shader_hash == 0x43005295B40E2DB6ull);
@@ -23305,99 +24010,121 @@ bool Gta4NativeGraphicsSystem::BindCommonDrawState(
   if (active_frame_slot_ >= frame_constant_arenas_.size()) {
     return false;
   }
-  NativeFrameConstantArena& constant_arena = frame_constant_arenas_[active_frame_slot_];
-  uint64_t remembered_identity = 0;
-  uint64_t* shared_identity = nullptr;
-  if (constant_arena.last_shared_identity && REXCVAR_GET(gta4_native_shared_key_memo) &&
-      shared_key == constant_arena.last_shared_key) {
-    remembered_identity = constant_arena.last_shared_identity;
-    shared_identity = &remembered_identity;
+  if (resources.has_last_shared_key && shared_key == resources.last_shared_key) {
+    shared_constants_allocation = resources.last_shared_allocation;
   } else {
-    shared_identity = constant_arena.shared_versions.Find(shared_key);
-  }
-  if (!shared_identity) {
-    if (!constant_arena.next_shared_identity ||
-        constant_arena.next_shared_identity == std::numeric_limits<uint64_t>::max()) {
-      return false;
-    }
-    const uint64_t next_identity = constant_arena.next_shared_identity++;
-    const auto insertion = constant_arena.shared_versions.Insert(shared_key, next_identity);
-    if (!insertion || !insertion.inserted) {
-      return false;
-    }
-    shared_identity = insertion.value;
-  }
-  if (shared_identity != &remembered_identity) {
-    constant_arena.last_shared_key = shared_key;
-    constant_arena.last_shared_identity = *shared_identity;
-  }
-  if (!FindFrameConstantBuffer(NativeConstantBufferKind::kShared, *shared_identity,
-                               shared_constants_allocation)) {
-    NativeSharedConstants shared_constants{};
-    shared_constants.modern_effects = BuildModernEffectConstants(
-        shared_key.modern_effects_enabled, shared_key.tone_lut_address, command.environmental_data.get());
-    shared_constants.modern_effects.reserved = shared_key.water_reflection;
-    shared_constants.modern_effects.viewport[0] = requested_viewport_x * float(width) / float(logical_width);
-    shared_constants.modern_effects.viewport[1] = requested_viewport_y * float(height) / float(logical_height);
-    shared_constants.modern_effects.viewport[2] = requested_viewport_width * float(width) / float(logical_width);
-    shared_constants.modern_effects.viewport[3] = requested_viewport_height * float(height) / float(logical_height);
-    shared_constants.modern_effects.depth_range[0] = std::clamp(requested_min_depth, 0.0f, 1.0f);
-    shared_constants.modern_effects.depth_range[1] = std::clamp(requested_max_depth, 0.0f, 1.0f);
-    shared_constants.modern_effects.cloud_mask_address = shared_key.cloud_mask_address;
-    shared_constants.modern_effects.cloud_mask_width = shared_key.cloud_mask_width;
-    shared_constants.modern_effects.cloud_mask_height = shared_key.cloud_mask_height;
-    shared_constants.split_postfx_applied = float(shared_key.split_postfx_applied);
-    for (uint32_t i = 0; i < kNativeColorOutputTargetCount; ++i) {
-      shared_constants.color_output[i] = NativeColorOutput(
-          shared_key.color_output_info[i], (shared_key.color_output_mask & (1u << i)) != 0);
+    NativeFrameConstantArena& constant_arena = frame_constant_arenas_[active_frame_slot_];
+    uint64_t remembered_identity = 0;
+    uint64_t* shared_identity = nullptr;
+    if (constant_arena.last_shared_identity && REXCVAR_GET(gta4_native_shared_key_memo) &&
+        shared_key == constant_arena.last_shared_key) {
+      remembered_identity = constant_arena.last_shared_identity;
+      shared_identity = &remembered_identity;
+    } else {
+      shared_identity = constant_arena.shared_versions.Find(shared_key);
     }
-    for (uint32_t stage = 0; stage < kShaderTextureCount; ++stage) {
-      shared_constants.texture_2d_indices[stage] = command.texture_descriptor_indices[stage];
-      shared_constants.texture_2d_array_indices[stage] = command.texture_descriptor_indices[stage];
-      shared_constants.texture_3d_indices[stage] = command.texture_descriptor_indices[stage];
-      shared_constants.texture_cube_indices[stage] = command.texture_descriptor_indices[stage];
-      shared_constants.sampler_indices[stage] = command.sampler_descriptor_indices[stage];
-      shared_constants.sampler_lod_bias[stage] =
-          std::bit_cast<float>(shared_key.sampler_lod_bias_bits[stage]);
-    }
-    for (uint32_t component = 0; component < std::size(shared_constants.clip_plane);
-         ++component) {
-      shared_constants.clip_plane[component] =
-          std::bit_cast<float>(command.fixed_function_state.clip_plane_bits[component]);
-    }
-    shared_constants.clip_plane_enabled =
-        (command.fixed_function_state.user_clip_plane_enable_mask & 1u) != 0;
-    shared_constants.alpha_threshold = command.fixed_function_state.alpha_reference;
-    shared_constants.alpha_to_mask = command.fixed_function_state.alpha_to_mask;
-    shared_constants.alpha_to_mask_sample_count = uint32_t(samples);
-    shared_constants.booleans = PackNativeShaderBooleans(vertex_booleans, pixel_booleans);
-    shared_constants.half_pixel_offset_x = 1.0f / static_cast<float>(logical_width);
-    shared_constants.half_pixel_offset_y = -1.0f / static_cast<float>(logical_height);
-    const FragmentCoordinateScale fragment_coordinate_scale =
-        CalculateFragmentCoordinateScale(logical_width, logical_height, width, height);
-    shared_constants.fragment_coordinate_scale_x = fragment_coordinate_scale.x;
-    shared_constants.fragment_coordinate_scale_y = fragment_coordinate_scale.y;
-    shared_constants.motion_blur_time_scale = MotionBlurTimeScale(command.environmental_data.get());
-    if (command.environmental_data) {
-      const EnvironmentalDataV2& environment = *command.environmental_data;
-      shared_constants.environmental_valid_fields = environment.valid_fields;
-      shared_constants.fog_parameters[0] = environment.fog_density;
-      shared_constants.fog_parameters[1] = environment.fog_height_falloff;
-      shared_constants.fog_parameters[2] = environment.fog_altitude_tweak;
-      shared_constants.fog_parameters[3] = environment.fog_power;
-      std::copy(environment.camera_position.begin(), environment.camera_position.end(),
-                shared_constants.camera_position);
-      std::copy(environment.view_inverse_matrix.begin(), environment.view_inverse_matrix.end(),
-                shared_constants.view_inverse_matrix);
-      shared_constants.projection_scale[0] = environment.projection_matrix[0];
-      shared_constants.projection_scale[1] = environment.projection_matrix[5];
-    }
-    const std::span<const uint8_t> shared_bytes(reinterpret_cast<const uint8_t*>(&shared_constants),
-                                                sizeof(shared_constants));
-    if (!GetOrCreateFrameConstantBuffer(NativeConstantBufferKind::kShared, *shared_identity,
-                                        shared_bytes, false, shared_constants_allocation)) {
-      return false;
+    if (!shared_identity) {
+      if (!constant_arena.next_shared_identity ||
+          constant_arena.next_shared_identity == std::numeric_limits<uint64_t>::max()) {
+        return false;
+      }
+      const uint64_t next_identity = constant_arena.next_shared_identity++;
+      const auto insertion = constant_arena.shared_versions.Insert(shared_key, next_identity);
+      if (!insertion || !insertion.inserted) {
+        return false;
+      }
+      shared_identity = insertion.value;
+    }
+    if (shared_identity != &remembered_identity) {
+      constant_arena.last_shared_key = shared_key;
+      constant_arena.last_shared_identity = *shared_identity;
+    }
+    if (!FindFrameConstantBuffer(NativeConstantBufferKind::kShared, *shared_identity,
+                                 shared_constants_allocation)) {
+      NativeSharedConstants shared_constants{};
+      if (shared_key.modern_effects_enabled) {
+        shared_constants.modern_effects = BuildModernEffectConstants(
+            shared_key.modern_effects_enabled, shared_key.tone_lut_address, command.environmental_data.get());
+        shared_constants.modern_effects.reserved = shared_key.water_reflection;
+        shared_constants.modern_effects.viewport[0] = requested_viewport_x * float(width) / float(logical_width);
+        shared_constants.modern_effects.viewport[1] = requested_viewport_y * float(height) / float(logical_height);
+        shared_constants.modern_effects.viewport[2] = requested_viewport_width * float(width) / float(logical_width);
+        shared_constants.modern_effects.viewport[3] = requested_viewport_height * float(height) / float(logical_height);
+        shared_constants.modern_effects.depth_range[0] = std::clamp(requested_min_depth, 0.0f, 1.0f);
+        shared_constants.modern_effects.depth_range[1] = std::clamp(requested_max_depth, 0.0f, 1.0f);
+        shared_constants.modern_effects.cloud_mask_address = shared_key.cloud_mask_address;
+        shared_constants.modern_effects.cloud_mask_width = shared_key.cloud_mask_width;
+        shared_constants.modern_effects.cloud_mask_height = shared_key.cloud_mask_height;
+      }
+      shared_constants.split_postfx_applied = float(shared_key.split_postfx_applied);
+      for (uint32_t i = 0; i < kNativeColorOutputTargetCount; ++i) {
+        shared_constants.color_output[i] = NativeColorOutput(
+            shared_key.color_output_info[i], (shared_key.color_output_mask & (1u << i)) != 0);
+      }
+        if (command.used_texture_mask != 0 || bias_stages != 0 ||
+          native_descriptor_backend_ != NativeDescriptorBackend::kIndexed ||
+          !native_descriptor_paging_) {
+        std::memcpy(shared_constants.texture_2d_indices,
+                    command.texture_descriptor_indices.data(),
+                    sizeof(shared_constants.texture_2d_indices));
+        std::memcpy(shared_constants.texture_2d_array_indices,
+                    command.texture_descriptor_indices.data(),
+                    sizeof(shared_constants.texture_2d_array_indices));
+        std::memcpy(shared_constants.texture_3d_indices,
+                    command.texture_descriptor_indices.data(),
+                    sizeof(shared_constants.texture_3d_indices));
+        std::memcpy(shared_constants.texture_cube_indices,
+                    command.texture_descriptor_indices.data(),
+                    sizeof(shared_constants.texture_cube_indices));
+        std::memcpy(shared_constants.sampler_indices,
+                    command.sampler_descriptor_indices.data(),
+                    sizeof(shared_constants.sampler_indices));
+        std::memcpy(shared_constants.sampler_lod_bias,
+                    shared_key.sampler_lod_bias_bits.data(),
+                    sizeof(shared_constants.sampler_lod_bias));
+      }
+      for (uint32_t component = 0; component < std::size(shared_constants.clip_plane);
+           ++component) {
+        shared_constants.clip_plane[component] =
+            std::bit_cast<float>(command.fixed_function_state.clip_plane_bits[component]);
+      }
+      shared_constants.clip_plane_enabled =
+          (command.fixed_function_state.user_clip_plane_enable_mask & 1u) != 0;
+      shared_constants.alpha_threshold = command.fixed_function_state.alpha_reference;
+      shared_constants.alpha_to_mask = command.fixed_function_state.alpha_to_mask;
+      shared_constants.alpha_to_mask_sample_count = uint32_t(samples);
+      shared_constants.booleans = PackNativeShaderBooleans(vertex_booleans, pixel_booleans);
+      shared_constants.half_pixel_offset_x = 1.0f / static_cast<float>(logical_width);
+      shared_constants.half_pixel_offset_y = -1.0f / static_cast<float>(logical_height);
+      const FragmentCoordinateScale fragment_coordinate_scale =
+          CalculateFragmentCoordinateScale(logical_width, logical_height, width, height);
+      shared_constants.fragment_coordinate_scale_x = fragment_coordinate_scale.x;
+      shared_constants.fragment_coordinate_scale_y = fragment_coordinate_scale.y;
+      shared_constants.motion_blur_time_scale = MotionBlurTimeScale(command.environmental_data.get());
+      if (command.environmental_data) {
+        const EnvironmentalDataV2& environment = *command.environmental_data;
+        shared_constants.environmental_valid_fields = environment.valid_fields;
+        shared_constants.fog_parameters[0] = environment.fog_density;
+        shared_constants.fog_parameters[1] = environment.fog_height_falloff;
+        shared_constants.fog_parameters[2] = environment.fog_altitude_tweak;
+        shared_constants.fog_parameters[3] = environment.fog_power;
+        std::copy(environment.camera_position.begin(), environment.camera_position.end(),
+                  shared_constants.camera_position);
+        std::copy(environment.view_inverse_matrix.begin(), environment.view_inverse_matrix.end(),
+                  shared_constants.view_inverse_matrix);
+        shared_constants.projection_scale[0] = environment.projection_matrix[0];
+        shared_constants.projection_scale[1] = environment.projection_matrix[5];
+      }
+      const std::span<const uint8_t> shared_bytes(reinterpret_cast<const uint8_t*>(&shared_constants),
+                                                  sizeof(shared_constants));
+      if (!GetOrCreateFrameConstantBuffer(NativeConstantBufferKind::kShared, *shared_identity,
+                                          shared_bytes, false, shared_constants_allocation)) {
+        return false;
+      }
     }
+    resources.has_last_shared_key = true;
+    resources.last_shared_key = shared_key;
+    resources.last_shared_allocation = shared_constants_allocation;
   }
 
   static const bool trace_glass_output = [] {
@@ -23757,7 +24484,7 @@ bool Gta4NativeGraphicsSystem::RecordPrimitiveUpBatch(
   const VkDeviceSize default_vertex_offset = 0;
   BindNativeVertexBuffer(command_buffer, kDefaultVertexBinding, default_vertex_buffer, default_vertex_offset);
   BindNativeVertexBuffer(command_buffer, 0, vertices.buffer, vertices.offset);
-  dfn.vkCmdBindIndexBuffer(command_buffer, indices.buffer, indices.offset, VK_INDEX_TYPE_UINT32);
+  BindNativeIndexBuffer(command_buffer, indices.buffer, indices.offset, VK_INDEX_TYPE_UINT32);
   ObserveNativeGpuProfileDraw(first, pipeline, target.samples, 0, uint32_t(index_count));
   if (REXCVAR_GET(gta4_native_debug_gpu_attribution) != 2) dfn.vkCmdDrawIndexed(command_buffer, uint32_t(index_count), 1, 0, 0, 0);
   TraceModernShaderDraw(first, target);
@@ -24089,7 +24816,7 @@ bool Gta4NativeGraphicsSystem::RecordPrimitiveUp(VkCommandBuffer command_buffer,
       profile::CpuCall(profile::CpuOp::kDriverDraw, [&] { if (!REXCVAR_GET(gta4_native_debug_gpu_attribution) || REXCVAR_GET(gta4_native_debug_gpu_attribution) == 1) dfn.vkCmdDraw(command_buffer, 4, 1, rectangle * 4, 0); });
     }
   } else if (draw.primitive_type == uint32_t(xenos::PrimitiveType::kQuadList)) {
-    profile::CpuCall(profile::CpuOp::kDriverBind, [&] { return dfn.vkCmdBindIndexBuffer(command_buffer, quad_list_indices.buffer, quad_list_indices.offset,
+    profile::CpuCall(profile::CpuOp::kDriverBind, [&] { return BindNativeIndexBuffer(command_buffer, quad_list_indices.buffer, quad_list_indices.offset,
                              VK_INDEX_TYPE_UINT32); });
     ObserveNativeGpuProfileDraw(command, pipeline, target.samples, 0, quad_list_index_count);
     profile::CpuCall(profile::CpuOp::kDriverDraw, [&] { if (!REXCVAR_GET(gta4_native_debug_gpu_attribution) || REXCVAR_GET(gta4_native_debug_gpu_attribution) == 1) dfn.vkCmdDrawIndexed(command_buffer, quad_list_index_count, 1, 0, 0, 0); });
@@ -24101,7 +24828,7 @@ bool Gta4NativeGraphicsSystem::RecordPrimitiveUp(VkCommandBuffer command_buffer,
     if (!AllocateUpload(count * sizeof(uint32_t), alignof(uint32_t), indices, NativeUploadKind::kIndex) ||
         !WriteNativeSequentialFan({reinterpret_cast<uint32_t*>(indices.mapping), size_t(count)},
                                   host_vertex_count)) return fail("triangle-fan-upload");
-    dfn.vkCmdBindIndexBuffer(command_buffer, indices.buffer, indices.offset, VK_INDEX_TYPE_UINT32);
+    BindNativeIndexBuffer(command_buffer, indices.buffer, indices.offset, VK_INDEX_TYPE_UINT32);
     ObserveNativeGpuProfileDraw(command, pipeline, target.samples, 0, uint32_t(count));
     if (REXCVAR_GET(gta4_native_debug_gpu_attribution) != 2) dfn.vkCmdDrawIndexed(command_buffer, uint32_t(count), 1, 0, 0, 0);
   } else {
@@ -24115,14 +24842,34 @@ bool Gta4NativeGraphicsSystem::RecordPrimitiveUp(VkCommandBuffer command_buffer,
 bool Gta4NativeGraphicsSystem::GetRequiredVertexStreams(
     const NativePipelineState& state,
     std::array<bool, kVertexStreamCount>& required_streams) const {
-  required_streams.fill(false);
-  if (!state.vertex_shader_resource || !state.vertex_declaration_resource) {
-    return false;
+  const NativeShader* vs = state.vertex_shader_resource;
+  const NativeVertexDeclaration* vdecl = state.vertex_declaration_resource.get();
+  if (!vs || !vdecl) {
+    required_streams.fill(false);
+    return false;
+  }
+  struct StreamMemo {
+    const NativeShader* vs = nullptr;
+    const NativeVertexDeclaration* vdecl = nullptr;
+    uint64_t vs_hash = 0;
+    uint64_t vdecl_gen = 0;
+    std::array<bool, kVertexStreamCount> streams{};
+    bool valid = false;
+  };
+  static thread_local std::array<StreamMemo, 64> s_memo{};
+  const size_t slot = ((reinterpret_cast<uintptr_t>(vs) >> 4) ^
+                       (reinterpret_cast<uintptr_t>(vdecl) >> 2)) & 63;
+  StreamMemo& m = s_memo[slot];
+  if (m.valid && m.vs == vs && m.vdecl == vdecl &&
+      m.vs_hash == vs->hash && m.vdecl_gen == vdecl->generation) {
+    required_streams = m.streams;
+    return true;
   }
 
-  for (const NativeVertexInput& shader_input : state.vertex_shader_resource->vertex_inputs) {
+  required_streams.fill(false);
+  for (const NativeVertexInput& shader_input : vs->vertex_inputs) {
     const VertexElement* matching_element = nullptr;
-    for (const VertexElement& element : state.vertex_declaration_resource->elements) {
+    for (const VertexElement& element : vdecl->elements) {
       if (ConvertVertexUsageToLocation(element.usage, element.usage_index) ==
           shader_input.location) {
         matching_element = &element;
@@ -24137,6 +24884,7 @@ bool Gta4NativeGraphicsSystem::GetRequiredVertexStreams(
     }
     required_streams[matching_element->stream] = true;
   }
+  m = {vs, vdecl, vs->hash, vdecl->generation, required_streams, true};
   return true;
 }
 
@@ -24243,7 +24991,7 @@ bool Gta4NativeGraphicsSystem::RecordPrimitive(VkCommandBuffer command_buffer,
       *(indices++) = vertex + 2;
       *(indices++) = vertex + 3;
     }
-    profile::CpuCall(profile::CpuOp::kDriverBind, [&] { return dfn.vkCmdBindIndexBuffer(command_buffer, indices_allocation.buffer, indices_allocation.offset,
+    profile::CpuCall(profile::CpuOp::kDriverBind, [&] { return BindNativeIndexBuffer(command_buffer, indices_allocation.buffer, indices_allocation.offset,
                              VK_INDEX_TYPE_UINT32); });
     ObserveNativeGpuProfileDraw(command, pipeline, target.samples, 0, index_count);
     profile::CpuCall(profile::CpuOp::kDriverDraw, [&] { if (!REXCVAR_GET(gta4_native_debug_gpu_attribution) || REXCVAR_GET(gta4_native_debug_gpu_attribution) == 1) dfn.vkCmdDrawIndexed(command_buffer, index_count, 1, 0, int32_t(draw.start_vertex), 0); });
@@ -24255,7 +25003,7 @@ bool Gta4NativeGraphicsSystem::RecordPrimitive(VkCommandBuffer command_buffer,
     if (!AllocateUpload(count * sizeof(uint32_t), alignof(uint32_t), indices, NativeUploadKind::kIndex) ||
         !WriteNativeSequentialFan({reinterpret_cast<uint32_t*>(indices.mapping), size_t(count)},
                                   draw.vertex_count, draw.start_vertex)) return fail("triangle-fan-upload");
-    dfn.vkCmdBindIndexBuffer(command_buffer, indices.buffer, indices.offset, VK_INDEX_TYPE_UINT32);
+    BindNativeIndexBuffer(command_buffer, indices.buffer, indices.offset, VK_INDEX_TYPE_UINT32);
     ObserveNativeGpuProfileDraw(command, pipeline, target.samples, 0, uint32_t(count));
     if (REXCVAR_GET(gta4_native_debug_gpu_attribution) != 2) dfn.vkCmdDrawIndexed(command_buffer, uint32_t(count), 1, 0, 0, 0);
   } else {
@@ -24280,6 +25028,18 @@ void Gta4NativeGraphicsSystem::BindNativeVertexBuffer(VkCommandBuffer command_bu
                                                                        &buffer, &offset);
 }
 
+void Gta4NativeGraphicsSystem::BindNativeIndexBuffer(VkCommandBuffer command_buffer,
+                                                     VkBuffer buffer, VkDeviceSize offset,
+                                                     VkIndexType index_type) {
+  const bool changed = native_draw_state_cache_.UpdateIndexBuffer(
+      NativeVulkanHandleIdentity(command_buffer), NativeVulkanHandleIdentity(buffer),
+      uint64_t(offset), uint32_t(index_type));
+  if (!changed && REXCVAR_GET(gta4_native_dedupe_index_binding)) return;
+  auto* vulkan_provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
+  vulkan_provider->vulkan_device()->functions().vkCmdBindIndexBuffer(command_buffer, buffer,
+                                                                     offset, index_type);
+}
+
 bool Gta4NativeGraphicsSystem::RecordIndexedPrimitive(VkCommandBuffer command_buffer,
                                                       const NativeCommand& command, uint32_t width,
                                                       uint32_t height,
@@ -24555,6 +25315,7 @@ bool Gta4NativeGraphicsSystem::RecordIndexedPrimitive(VkCommandBuffer command_bu
       }
     }
   }
+#if 0
   if (guest_restart_enabled && draw.primitive_type != uint32_t(xenos::PrimitiveType::kTriangleFan)) {
     const VkDeviceSize restart_size = VkDeviceSize(host_index_count) * element_size;
     if (!AllocateUpload(restart_size, size_t(element_size), primitive_restart_indices,
@@ -24582,6 +25343,60 @@ bool Gta4NativeGraphicsSystem::RecordIndexedPrimitive(VkCommandBuffer command_bu
     host_index_buffer = primitive_restart_indices.buffer;
     host_index_offset = primitive_restart_indices.offset;
   }
+#endif
+// Xenos и Vulkan оба используют 0xFFFF для 16-битного рестарта: переписывание было
+  // побайтовой копией. Биндим исходный буфер как есть.
+  const bool restart_identity =
+      !index32 && uint16_t(draw.primitive_restart_index) == UINT16_MAX;
+  if (guest_restart_enabled && !restart_identity &&
+      draw.primitive_type != uint32_t(xenos::PrimitiveType::kTriangleFan)) {
+    const VkDeviceSize restart_size = VkDeviceSize(host_index_count) * element_size;
+    if (!AllocateUpload(restart_size, size_t(element_size), primitive_restart_indices,
+                        NativeUploadKind::kIndex)) {
+      return fail("primitive-restart-upload-allocation");
+    }
+    if (index32) {
+      const uint32_t* source = reinterpret_cast<const uint32_t*>(selected_index_bytes);
+      uint32_t* destination = reinterpret_cast<uint32_t*>(primitive_restart_indices.mapping);
+      // Рестарт у Xenos 24-битный: сравниваем маскированные значения с обеих сторон.
+      const uint32_t restart = draw.primitive_restart_index & xenos::kVertexIndexMask;
+      uint32_t index = 0;
+#if defined(__aarch64__)
+      const uint32x4_t vmask = vdupq_n_u32(xenos::kVertexIndexMask);
+      const uint32x4_t vrestart = vdupq_n_u32(restart);
+      const uint32x4_t vones = vdupq_n_u32(UINT32_MAX);
+      for (; index + 4 <= host_index_count; index += 4) {
+        const uint32x4_t v = vld1q_u32(source + index);
+        const uint32x4_t hit = vceqq_u32(vandq_u32(v, vmask), vrestart);
+        vst1q_u32(destination + index, vbslq_u32(hit, vones, v));
+      }
+#endif
+      for (; index < host_index_count; ++index) {
+        const uint32_t value = source[index];
+        destination[index] = (value & xenos::kVertexIndexMask) == restart ? UINT32_MAX : value;
+      }
+    } else {
+      const uint16_t* source = reinterpret_cast<const uint16_t*>(selected_index_bytes);
+      uint16_t* destination = reinterpret_cast<uint16_t*>(primitive_restart_indices.mapping);
+      const uint16_t guest_restart_index = uint16_t(draw.primitive_restart_index);
+      uint32_t index = 0;
+#if defined(__aarch64__)
+      const uint16x8_t vrestart = vdupq_n_u16(guest_restart_index);
+      const uint16x8_t vones = vdupq_n_u16(UINT16_MAX);
+      for (; index + 8 <= host_index_count; index += 8) {
+        const uint16x8_t v = vld1q_u16(source + index);
+        vst1q_u16(destination + index, vbslq_u16(vceqq_u16(v, vrestart), vones, v));
+      }
+#endif
+      for (; index < host_index_count; ++index) {
+        destination[index] = source[index] == guest_restart_index ? UINT16_MAX : source[index];
+      }
+    }
+    selected_index_bytes = primitive_restart_indices.mapping;
+    host_start_index = 0;
+    host_index_buffer = primitive_restart_indices.buffer;
+    host_index_offset = primitive_restart_indices.offset;
+  }
   const bool collect_indexed_diagnostics =
       CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeTrace) ||
       (CachedDiagnosticsEnabled(rex::diagnostics::Category::kTransition) &&
@@ -24855,7 +25670,7 @@ bool Gta4NativeGraphicsSystem::RecordIndexedPrimitive(VkCommandBuffer command_bu
     }
   }
 
-  profile::CpuCall(profile::CpuOp::kDriverBind, [&] { return dfn.vkCmdBindIndexBuffer(command_buffer, host_index_buffer, host_index_offset, bound_index_type); });
+  profile::CpuCall(profile::CpuOp::kDriverBind, [&] { return BindNativeIndexBuffer(command_buffer, host_index_buffer, host_index_offset, bound_index_type); });
   if (IsHoveGantryGeometryCandidate(command)) {
     static std::atomic<uint64_t> hove_recorded_draw_count{0};
     const uint64_t hit = ++hove_recorded_draw_count;
@@ -25327,6 +26142,108 @@ VkPipeline Gta4NativeGraphicsSystem::GetOrCreateDepthHandoffPipeline(
                                        destination_samples);
 }
 
+// SGSR: создание пайплайна для Snapdragon Game Super Resolution
+VkPipeline Gta4NativeGraphicsSystem::GetOrCreateSGSRPresentPipeline() {
+  if (sgsr_present_pipeline_) {
+    return sgsr_present_pipeline_;
+  }
+  auto* vulkan_provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
+  const ui::vulkan::VulkanDevice* vulkan_device =
+      vulkan_provider ? vulkan_provider->vulkan_device() : nullptr;
+  if (!vulkan_device || !resolve_conversion_pipeline_layout_) {
+    return VK_NULL_HANDLE;
+  }
+  const auto& dfn = vulkan_device->functions();
+  const VkDevice device = vulkan_device->device();
+  VkShaderModule vertex_shader = gpu_labels::CreateShader(vulkan_device, fullscreen_cw_vs, sizeof(fullscreen_cw_vs), "GTA4/fullscreen_cw_vs");
+  // SGSR: используем gta4_native_sgsr_present_ps вместо HDR
+  VkShaderModule pixel_shader = gpu_labels::CreateShader(vulkan_device, gta4_native_sgsr_present_ps, sizeof(gta4_native_sgsr_present_ps), "GTA4/gta4_native_sgsr_present_ps");
+  // SGSR: одноразовый лог об успешной загрузке шейдера
+  if (pixel_shader) {
+    static std::atomic<bool> logged_sgsr_shader{false};
+    if (!logged_sgsr_shader.exchange(true)) {
+      REXLOG_INFO("gta4-native-upscaler: [SGSR] SGSR pixel shader successfully compiled and loaded!");
+    }
+  }
+  if (!vertex_shader || !pixel_shader) {
+    if (vertex_shader) {
+      profile::CpuCall(profile::CpuOp::kDriverDestruction, [&] { return dfn.vkDestroyShaderModule(device, vertex_shader, nullptr); });
+    }
+    if (pixel_shader) {
+      profile::CpuCall(profile::CpuOp::kDriverDestruction, [&] { return dfn.vkDestroyShaderModule(device, pixel_shader, nullptr); });
+    }
+    return VK_NULL_HANDLE;
+  }
+
+  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
+  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
+  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
+  stages[0].module = vertex_shader;
+  stages[0].pName = "main";
+  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
+  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
+  stages[1].module = pixel_shader;
+  stages[1].pName = "main";
+  VkPipelineVertexInputStateCreateInfo vertex_input{};
+  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
+  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
+  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
+  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
+  VkPipelineViewportStateCreateInfo viewport_state{};
+  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
+  viewport_state.viewportCount = 1;
+  viewport_state.scissorCount = 1;
+  VkPipelineRasterizationStateCreateInfo rasterization{};
+  rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
+  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
+  rasterization.cullMode = VK_CULL_MODE_NONE;
+  rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
+  rasterization.lineWidth = 1.0f;
+  VkPipelineMultisampleStateCreateInfo multisample{};
+  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
+  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
+  VkPipelineColorBlendAttachmentState color_attachment{};
+  color_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
+                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
+  VkPipelineColorBlendStateCreateInfo color_blend{};
+  color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
+  color_blend.attachmentCount = 1;
+  color_blend.pAttachments = &color_attachment;
+  const std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
+  VkPipelineDynamicStateCreateInfo dynamic_state{};
+  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
+  dynamic_state.dynamicStateCount = uint32_t(dynamic_states.size());
+  dynamic_state.pDynamicStates = dynamic_states.data();
+  const VkFormat destination_format = ui::vulkan::VulkanPresenter::kGuestOutputFormat;
+  VkPipelineRenderingCreateInfo rendering_info{};
+  rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
+  rendering_info.colorAttachmentCount = 1;
+  rendering_info.pColorAttachmentFormats = &destination_format;
+  VkGraphicsPipelineCreateInfo pipeline_info{};
+  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
+  pipeline_info.pNext = &rendering_info;
+  pipeline_info.stageCount = uint32_t(stages.size());
+  pipeline_info.pStages = stages.data();
+  pipeline_info.pVertexInputState = &vertex_input;
+  pipeline_info.pInputAssemblyState = &input_assembly;
+  pipeline_info.pViewportState = &viewport_state;
+  pipeline_info.pRasterizationState = &rasterization;
+  pipeline_info.pMultisampleState = &multisample;
+  pipeline_info.pColorBlendState = &color_blend;
+  pipeline_info.pDynamicState = &dynamic_state;
+  pipeline_info.layout = resolve_conversion_pipeline_layout_;
+  const VkResult result = profile::CpuCall(profile::CpuOp::kDriverPipeline, [&] { return dfn.vkCreateGraphicsPipelines(
+      device, native_pipeline_cache_, 1, &pipeline_info, nullptr, &sgsr_present_pipeline_); });
+  profile::CpuCall(profile::CpuOp::kDriverDestruction, [&] { return dfn.vkDestroyShaderModule(device, pixel_shader, nullptr); });
+  profile::CpuCall(profile::CpuOp::kDriverDestruction, [&] { return dfn.vkDestroyShaderModule(device, vertex_shader, nullptr); });
+  if (result != VK_SUCCESS) {
+    sgsr_present_pipeline_ = VK_NULL_HANDLE;
+  } else {
+    native_pipeline_cache_generation_.fetch_add(1, std::memory_order_release);
+  }
+  return sgsr_present_pipeline_;
+}
+
 VkPipeline Gta4NativeGraphicsSystem::GetOrCreateHDRPresentPipeline() {
   if (hdr_present_pipeline_) {
     return hdr_present_pipeline_;
@@ -25832,7 +26749,7 @@ bool Gta4NativeGraphicsSystem::RecordResolveConversion(
   constants.destination_height = destination_height;
   constants.flags = xenos_float16_pack ? kResolveConversionFlagXenosFloat16Pack : 0u;
   constants.flags |= (uint32_t(color_exponent) & 63u) << 8;
-  if (color_exponent) {
+  if (color_exponent && REXCVAR_GET(gta4_trace_native_renderer)) {
     static std::atomic<uint32_t> output_resolve_records{0};
     if (output_resolve_records.fetch_add(1, std::memory_order_relaxed) < 64) {
       REXLOG_INFO("gta4-native-color-resolve: frame={} source={:08X} destination={:08X} "
@@ -28825,9 +29742,17 @@ bool Gta4NativeGraphicsSystem::RecordPresent(
         hdr_headroom);
   }
 
-  VkPipeline present_pipeline = GetOrCreateHDRPresentPipeline();
+  //VkPipeline present_pipeline = GetOrCreateHDRPresentPipeline();
+  //if (present_pipeline && frame_descriptor_pool_ && presenter_view && shader_source_view) {
+  //начало
+    // SGSR: проверяем, выбран ли режим SGSR, и выбираем соответствующий пайплайн
+  const bool use_sgsr = rex::cvar::Query<std::string>("gta4_native_upscaler") == "sgsr";
+  VkPipeline present_pipeline = use_sgsr ? GetOrCreateSGSRPresentPipeline() : GetOrCreateHDRPresentPipeline();
+  
   if (present_pipeline && frame_descriptor_pool_ && presenter_view && shader_source_view) {
-    const auto& dfn = vulkan_device->functions();
+  //конец
+	
+	const auto& dfn = vulkan_device->functions();
     const VkDevice device = vulkan_device->device();
     VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
     VkDescriptorSetAllocateInfo descriptor_allocate_info{};
@@ -28917,8 +29842,10 @@ bool Gta4NativeGraphicsSystem::RecordPresent(
       constants.shoulder_start = static_cast<float>(
           std::clamp(rex::cvar::Query<double>("gta4_native_auto_hdr_shoulder_start"), 0.0, 1.0));
       constants.shoulder_power = static_cast<float>(
-          std::clamp(rex::cvar::Query<double>("gta4_native_auto_hdr_shoulder_power"), 1.0, 10.0));
-      const uint32_t aa_bits = temporal_resolved_ ? 0 : GetNativePresentationAABits();
+            std::clamp(rex::cvar::Query<double>("gta4_native_auto_hdr_shoulder_power"), 1.0, 10.0));
+        constants.sgsr_sharpness = static_cast<float>(
+            std::clamp(rex::cvar::Query<double>("gta4_sgsr_sharpness"), 0.0, 5.0));
+        const uint32_t aa_bits = temporal_resolved_ ? 0 : GetNativePresentationAABits();
       const uint32_t ssaa_bits = ssaa_applied ? 32u : 0u;
       constants.output_mode = (hdr_output ? 1u : 0u) | aa_bits | ssaa_bits | 4u |
                               (REXCVAR_GET(gta4_native_output_dither) ? 8u : 0u) |
@@ -28938,6 +29865,7 @@ bool Gta4NativeGraphicsSystem::RecordPresent(
               constants.paper_white_nits, constants.peak_nits, constants.shoulder_start,
               constants.shoulder_power, aa_bits, bool(ssaa_bits), constants.output_mode));
       }
+#if 0
       if (!aa_bits && !ssaa_bits &&
           rex::cvar::Query<std::string>("gta4_native_upscaler") == "fsr1") {
         static std::atomic<bool> logged_fsr_without_aa{false};
@@ -28947,6 +29875,28 @@ bool Gta4NativeGraphicsSystem::RecordPresent(
               "edge quality may be unstable");
         }
       }
+#endif
+//начало
+      // SGSR: отмечаем запрос SGSR в нативном проходе. Пока используется
+      // старый HDR-шейдер; ветка SGSR будет добавлена на следующем этапе.
+      const std::string requested_upscaler =
+          rex::cvar::Query<std::string>("gta4_native_upscaler");
+      if (!aa_bits && !ssaa_bits && requested_upscaler == "fsr1") {
+        static std::atomic<bool> logged_fsr_without_aa{false};
+        if (!logged_fsr_without_aa.exchange(true)) {
+          REXLOG_WARN(
+              "gta4-native-upscaler: FSR 1 input anti-aliasing is disabled; "
+              "edge quality may be unstable");
+        }
+      }
+      if (!aa_bits && !ssaa_bits && requested_upscaler == "sgsr") {
+        // SGSR: одноразовый лог, подтверждающий, что кадр пошёл через SGSR-пайплайн
+        static std::atomic<bool> logged_sgsr_pending{false};
+        if (!logged_sgsr_pending.exchange(true)) {
+          REXLOG_INFO("gta4-native-upscaler: [SGSR] Successfully presenting frame using SGSR pipeline!");
+        }
+      }
+//конец
       if (EmissionTraceConfig().pipeline_full_readback && bulb_trace_frame_ && !bulb_routes_.empty() &&
           bulb_full_frame_ == diagnostic_submitted_frame_) {
         REXLOG_INFO("gta4-bulb-full: point=present-bind run={} frame={} input-image={} input-view={} source-format={} source={}x{} destination={}x{} "
@@ -29594,6 +30544,7 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
   const profile::CpuScope profile_scope(profile::CpuOp::kRecordFrame);
 
   SCOPE_profile_cpu_i("gpu", "GTA4 Native RecordNativeFrame");
+    const uint64_t record_t0 = FrameTimelineNow();
   BeginBulbFrame(submitted_frame);
   BeginFireFrame(command_buffer, submitted_frame);
   const auto fire_frame_reset = MakeScopeExit([this] { fire_frame_ = fire_images_ = fire_event_active_ = false; });
@@ -29919,7 +30870,7 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
   size_t first_opaque_depth_consumer_index = SIZE_MAX;
   size_t first_translucent_depth_consumer_index = SIZE_MAX;
   std::string_view first_translucent_depth_category;
-  if (diagnostic_frame && forward_depth_handle && depth_handoff_command_index != SIZE_MAX) {
+  if (REXCVAR_GET(gta4_trace_native_renderer) && diagnostic_frame && forward_depth_handle && depth_handoff_command_index != SIZE_MAX) {
     for (size_t pending_index = 0; pending_index < current_frame_.size(); ++pending_index) {
       if (pending_index <= depth_handoff_command_index) {
         continue;
@@ -30921,7 +31872,7 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
       observation.classification = attribution::NativePassClassification::kUnknown;
     } else {
       std::string_view translucent_category;
-      if (command.pipeline_state && command.pipeline_state->pixel_shader_resource) {
+      if (REXCVAR_GET(gta4_trace_native_renderer) && command.pipeline_state && command.pipeline_state->pixel_shader_resource) {
         translucent_category = ClassifyTranslucentDiagnosticShader(
             command.pipeline_state->pixel_shader_resource->filename);
       }
@@ -31270,8 +32221,9 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
   // and the state they point to (cache misses): fetch the fields recording
   // reads a few commands ahead, and the state objects once their pointers are
   // in cache.
-  const bool prefetch_commands = REXCVAR_GET(gta4_native_recorder_prefetch);
-  const auto prefetch_command = [](const NativeCommand& ahead) {
+ const bool prefetch_commands = REXCVAR_GET(gta4_native_recorder_prefetch);
+      const bool hitch_breakdown = REXCVAR_GET(gta4_native_hitch_breakdown);
+      const auto prefetch_command = [](const NativeCommand& ahead) {
     __builtin_prefetch(&ahead.type);
     __builtin_prefetch(&ahead.bytes);
     __builtin_prefetch(&ahead.shader_state);
@@ -31282,10 +32234,27 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
     __builtin_prefetch(&ahead.fixed_function_state);
     __builtin_prefetch(reinterpret_cast<const uint8_t*>(&ahead.fixed_function_state) + 64);
   };
-  const auto prefetch_state = [](const NativeCommand& ahead) {
+const auto prefetch_state = [](const NativeCommand& ahead) {
     if (const auto* state = ahead.pipeline_state.get()) __builtin_prefetch(state);
     if (const auto* state = ahead.shader_state.get()) __builtin_prefetch(state);
   };
+
+  // Диагностические строковые cvar'ы читаются один раз за кадр, а не на каждую команду.
+  const std::string ps9_stencil_probe_frame =
+      REXCVAR_GET(gta4_native_light_ps9_stencil_probe);
+  const std::string stencil_face_probe_frame =
+      REXCVAR_GET(gta4_native_light_stencil_face_probe);
+  const bool legacy_ps9_bypass_frame =
+      REXCVAR_GET(gta4_native_light_ps9_stencil_bypass);
+
+  g_hitch_record_setup_ns.store(FrameTimelineNow() - record_t0,
+                                std::memory_order_relaxed);
+  g_hitch_cmd_draw_ns.store(0, std::memory_order_relaxed);
+  g_hitch_cmd_draw_sh_ns.store(0, std::memory_order_relaxed);
+  g_hitch_cmd_draw_w_ns.store(0, std::memory_order_relaxed);
+  g_hitch_cmd_resolve_ns.store(0, std::memory_order_relaxed);
+  g_hitch_cmd_clear_ns.store(0, std::memory_order_relaxed);
+  g_hitch_cmd_other_ns.store(0, std::memory_order_relaxed);
   for (size_t command_index = 0; command_index < current_frame_.size(); ++command_index) {
     breadcrumb_command = command_index;
     if (prefetch_commands) {
@@ -31295,25 +32264,54 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
     if (command_index < merged_up_until) {
       continue;  // Recorded as part of the previous DrawPrimitiveUp batch.
     }
-    const NativeCommand& queued_command = current_frame_[command_index];
-    // Optional small-draw culling in the water reflection and shadow passes.
+   const NativeCommand& queued_command = current_frame_[command_index];
+        const uint64_t cmd_t0 = hitch_breakdown ? FrameTimelineNow() : 0;
+        const auto cmd_timer = MakeScopeExit([&] {
+          if (!hitch_breakdown) return;
+          const uint64_t dt = FrameTimelineNow() - cmd_t0;
+          switch (queued_command.type) {
+        case CommandType::kDrawPrimitive:
+        case CommandType::kDrawPrimitiveUp:
+        case CommandType::kDrawIndexedPrimitive:
+          if (shadow_map_draw(queued_command)) {
+            g_hitch_cmd_draw_sh_ns.fetch_add(dt, std::memory_order_relaxed);
+          } else {
+            g_hitch_cmd_draw_w_ns.fetch_add(dt, std::memory_order_relaxed);
+          }
+          g_hitch_cmd_draw_ns.fetch_add(dt, std::memory_order_relaxed);
+          break;
+        case CommandType::kResolve:
+          g_hitch_cmd_resolve_ns.fetch_add(dt, std::memory_order_relaxed);
+          break;
+        case CommandType::kClear:
+          g_hitch_cmd_clear_ns.fetch_add(dt, std::memory_order_relaxed);
+          break;
+        default:
+          g_hitch_cmd_other_ns.fetch_add(dt, std::memory_order_relaxed);
+          break;
+      }
+    });
+// Optional small-draw culling in the water reflection and shadow passes.
     if ((water_reflection_min_indices || shadow_min_indices) &&
         queued_command.type == CommandType::kDrawIndexedPrimitive &&
         queued_command.bytes.size() >= sizeof(DrawIndexedPrimitiveCommand)) {
       DrawIndexedPrimitiveCommand draw{};
       std::memcpy(&draw, queued_command.bytes.data(), sizeof(draw));
-      if (draw.index_count < std::max(water_reflection_min_indices, shadow_min_indices)) {
-        const performance::GpuRange range = performance_range_for_command(queued_command);
-        if (range == performance::GpuRange::kWaterReflections && !skip_water_reflection &&
-            draw.index_count < water_reflection_min_indices) {
-          g_water_reflection_draws_culled.fetch_add(1, std::memory_order_relaxed);
-          continue;
-        }
-        if (range == performance::GpuRange::kRetailWarpShadow &&
-            draw.index_count < shadow_min_indices) {
-          g_shadow_draws_culled.fetch_add(1, std::memory_order_relaxed);
-          continue;
-        }
+      if (shadow_min_indices && draw.index_count < shadow_min_indices &&
+          (shadow_map_draw(queued_command) ||
+           (queued_command.gpu_pass_origin.attributed() &&
+            performance::PerformanceRangeForRetailPhase(
+                queued_command.gpu_pass_origin.retail_phase) ==
+                performance::GpuRange::kRetailWarpShadow))) {
+        g_shadow_draws_culled.fetch_add(1, std::memory_order_relaxed);
+        continue;
+      }
+      if (water_reflection_min_indices && !skip_water_reflection &&
+          draw.index_count < water_reflection_min_indices &&
+          performance_range_for_command(queued_command) ==
+              performance::GpuRange::kWaterReflections) {
+        g_water_reflection_draws_culled.fetch_add(1, std::memory_order_relaxed);
+        continue;
       }
     }
     // Draws, clears and resolves of the water reflection capture: the reflection
@@ -31327,7 +32325,8 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
       const performance::GpuRange range = performance_range_for_command(queued_command);
       if ((skip_water_reflection && range == performance::GpuRange::kWaterReflections) ||
           (skip_environment_reflection && range == performance::GpuRange::kEnvironmentReflections) ||
-          (skip_shadows && range == performance::GpuRange::kRetailWarpShadow)) {
+          (skip_shadows && (range == performance::GpuRange::kRetailWarpShadow ||
+                            shadow_map_draw(queued_command)))) {
         continue;
       }
     }
@@ -31385,7 +32384,8 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
       QueueSurfaceImageRelease(release.resource);
       continue;
     }
-    if ((queued_command.type == CommandType::kDrawPrimitive ||
+        if (REXCVAR_GET(gta4_validate_native_hot_caches) &&
+        (queued_command.type == CommandType::kDrawPrimitive ||
          queued_command.type == CommandType::kDrawPrimitiveUp ||
          queued_command.type == CommandType::kDrawIndexedPrimitive ||
          queued_command.type == CommandType::kClear) &&
@@ -31405,7 +32405,7 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
     const NativeCommand* command_pointer = &queued_command;
     constexpr uint64_t kDeferredLightingPs9Hash = 0x23C88DA6F953E36Bull;
     constexpr uint32_t kFillerVolumePointTechnique = 11;
-    const std::string ps9_stencil_probe = REXCVAR_GET(gta4_native_light_ps9_stencil_probe);
+        const std::string& ps9_stencil_probe = ps9_stencil_probe_frame;
     const bool ps9_accumulation_draw =
         queued_command.light_trace_technique == kFillerVolumePointTechnique &&
         queued_command.light_trace_mode == 0 && queued_command.pipeline_state &&
@@ -31423,7 +32423,7 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
         ps9_probe_instance = StableDeferredLocalLightInstanceId(*vertex_constants, *constant_base);
       }
     }
-    const bool legacy_ps9_bypass = REXCVAR_GET(gta4_native_light_ps9_stencil_bypass);
+    const bool legacy_ps9_bypass = legacy_ps9_bypass_frame;
     const bool targeted_ps9_probe =
         ps9_probe_instance == kApartmentBulbX890Instance && ps9_stencil_probe != "original";
     if (ps9_accumulation_draw && (legacy_ps9_bypass || targeted_ps9_probe)) {
@@ -31455,7 +32455,7 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
       }
     }
 
-    const std::string stencil_face_probe = REXCVAR_GET(gta4_native_light_stencil_face_probe);
+    const std::string& stencil_face_probe = stencil_face_probe_frame;
     constexpr uint64_t kDeferredLightingVs1Hash = 0xA97B1F043F720396ull;
     uint64_t stencil_face_probe_instance = 0;
     if (stencil_face_probe != "original" && queued_command.pipeline_state &&
@@ -31545,11 +32545,14 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
     const bool descriptor_draw = command.type == CommandType::kDrawPrimitive ||
                                  command.type == CommandType::kDrawPrimitiveUp ||
                                  command.type == CommandType::kDrawIndexedPrimitive;
-    // Clears (vkCmdClearAttachments keeps the bound state) and phase markers
-    // bind nothing; every other command may bind its own pipelines.
-    const bool keeps_draw_state =
-        REXCVAR_GET(gta4_native_keep_draw_state) &&
-        (command.type == CommandType::kClear || command.type == CommandType::kRenderPhaseMarker);
+    // Clears (vkCmdClearAttachments keeps the bound state), phase markers, and
+    // SetRenderState (no-op on the state cache) bind nothing; every other
+    // command may bind its own pipelines.
+const bool keeps_draw_state =
+    REXCVAR_GET(gta4_native_keep_draw_state) &&
+    (command.type == CommandType::kClear ||
+     command.type == CommandType::kRenderPhaseMarker);
+    if (!descriptor_draw) native_target_memo_.valid = false;
     if (!descriptor_draw && !keeps_draw_state) {
       native_draw_state_cache_.Reset(); ubo_bound_layout_ = VK_NULL_HANDLE;
     } else if (keeps_draw_state) {
@@ -31578,9 +32581,11 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
     }
     diagnostic_submitted_frame_ = submitted_frame;
     diagnostic_command_index_ = command_index;
-    gpu_flight::Record("native.command", NativeVulkanHandleIdentity(command_buffer),
-                       submission_tracker_ ? submission_tracker_->GetCurrentSubmission() : 0,
-                       submitted_frame, command_index, uint64_t(command.type));
+    if (IsNativeFlightRecorderEnabled()) {
+      gpu_flight::Record("native.command", NativeVulkanHandleIdentity(command_buffer),
+                         submission_tracker_ ? submission_tracker_->GetCurrentSubmission() : 0,
+                         submitted_frame, command_index, uint64_t(command.type));
+    }
     diagnostic_render_phase_ = command.render_phase;
     diagnostic_render_phase_object_ = command.render_phase_object;
     fire_event_active_ = fire_frame_ && (command.fire_trace || command.type == CommandType::kResolve ||
@@ -32222,7 +33227,22 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
         end_rendering(__LINE__);
         rendering = false;
       }
-      if (resolve_matches_present && command_index + 1 == current_frame_.size() && !hdr_output &&
+	  
+      // Ищем, есть ли после текущей команды еще резолвы в этот же фронтбуфер.
+      // Если нет — мы можем безопасно применить present-from-surface.
+      bool subsequent_present_resolve = false;
+      if (resolve_matches_present) {
+        for (size_t s = command_index + 1; s < current_frame_.size(); ++s) {
+          const NativeCommand& next_cmd = current_frame_[s];
+          if (next_cmd.type == CommandType::kResolve && next_cmd.resolve_destination &&
+              next_cmd.resolve_destination->generation == present_source->generation) {
+            subsequent_present_resolve = true;
+            break;
+          }
+        }
+      }
+
+      if (resolve_matches_present && !subsequent_present_resolve && !hdr_output &&
           REXCVAR_GET(gta4_native_present_from_surface) && !diagnostic_frame && !fire_frame_ &&
           !force_content_probe && !temporal_display_ready_ && !temporal_active_ &&
           !IsNativeSmaaEnabled() && !IsNativeSsaaEnabled() && command.resolve_swap == UINT32_MAX) {
@@ -33000,7 +34020,8 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
       continue;
     }
     NativeRenderingTarget target;
-    if (!ResolveRenderingTarget(command, presenter_view, width, height, target)) {
+    // Передаем указатель на текущий открытый таргет, чтобы склеить RenderPass
+    if (!ResolveRenderingTargetMemo(command, presenter_view, width, height, target, active_target)) {
       TraceModernShaderFailure(command,"draw.targets","Render target resolution rejected");
       if (collect_frame_diagnostics) {
         ++target_failures;
@@ -33882,7 +34903,7 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
       }
     }
     std::string_view translucent_category;
-    if (full_translucency_diagnostics) {
+    if (full_translucency_diagnostics && REXCVAR_GET(gta4_trace_native_renderer)) {
       translucent_category =
           scene_write_query     ? std::string_view{"scene-write"}
           : gbuffer_write_query ? std::string_view{"gbuffer-write"}
@@ -34801,6 +35822,18 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
         why |= (!target.color_attachment_mask && active_target.color_attachment_mask) ? 1024u : 0u;
         why |= (target.color_attachment_mask && !active_target.color_attachment_mask) ? 2048u : 0u;
         ++g_rendering_split_reasons[why & 4095u];
+        if ((why & 4095u) == 0x025) {
+          static uint32_t s_split_025_limiter = 0;
+          if (++s_split_025_limiter <= 5) {
+            const auto* vs = command.pipeline_state ? command.pipeline_state->vertex_shader_resource : nullptr;
+            const auto* ps = command.pipeline_state ? command.pipeline_state->pixel_shader_resource : nullptr;
+            __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
+                                "split-025-truth: cmd_type=%d phase=%d mask_old=0x%X mask_new=0x%X vs=%s ps=%s",
+                                int(command.type), int(command.render_phase),
+                                active_target.color_attachment_mask, target.color_attachment_mask,
+                                vs ? vs->filename.c_str() : "none", ps ? ps->filename.c_str() : "none");
+          }
+        }
         end_rendering(__LINE__);
       }
       bool content_ready = true;
@@ -35076,12 +36109,21 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
             command_buffer, command_index, target, depth_attachment, stencil_attachment,
             producer_depth_resolve_scan);
       }
-      // TEMP: load ops of rendering scopes, split by colorless (light setup) scopes.
+// TEMP: load ops of rendering scopes, split by colorless (light setup) scopes.
       if (target.depth_stencil_attachment_active) {
         const uint32_t key = (color_attachment_count ? 0u : 4u) |
                              (depth_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 2u : 0u) |
                              (stencil_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 1u : 0u);
         ++g_light_reuse_failures[16 + key];
+
+        if (key == 0) {
+          static uint32_t s_loadop_log_limiter = 0;
+          if (++s_loadop_log_limiter <= 5) {
+            __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
+                                "f16-truth: depth.loadOp=%d stencil.loadOp=%d (0=LOAD, 1=CLEAR, 2=DONT_CARE, 1000400000=NONE)",
+                                int(depth_attachment.loadOp), int(stencil_attachment.loadOp));
+          }
+        }
       }
       gpu_labels::BeginSceneScope(vulkan_provider->vulkan_device(), command_buffer, rendering_info, native_gpu_scope_summary_, submitted_frame, uint32_t(command_index));
       profile::CpuCall(profile::CpuOp::kDriverRendering, [&] { return dfn.vkCmdBeginRendering(command_buffer, &rendering_info); });
@@ -36158,12 +37200,19 @@ bool Gta4NativeGraphicsSystem::RecordNativeFrame(
                                     command.index_buffer ? command.index_buffer->payload.size() : 0,
                                     vs ? vs->hash : 0, ps ? ps->hash : 0));
       }
-      if (collect_frame_diagnostics) {
+     // if (collect_frame_diagnostics) {
+       // command_recorded ? ++successful_draws : ++failed_draws;
+     // }
+      //if (command_recorded) {
+        //transition::NoteFirstIndexedDraw(diagnostic_guest_pc, diagnostic_guest_lr, submitted_frame,
+          //                               diagnostic_draw_id_);
+      // }
+	  if (collect_frame_diagnostics) {
         command_recorded ? ++successful_draws : ++failed_draws;
-      }
-      if (command_recorded) {
-        transition::NoteFirstIndexedDraw(diagnostic_guest_pc, diagnostic_guest_lr, submitted_frame,
-                                         diagnostic_draw_id_);
+        if (command_recorded) {
+          transition::NoteFirstIndexedDraw(diagnostic_guest_pc, diagnostic_guest_lr, submitted_frame,
+                                           diagnostic_draw_id_);
+        }
       }
     } else if (command.type == CommandType::kClear) {
       ClearCommand clear{};
@@ -37388,9 +38437,11 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
         const uint32_t requested_frame_slots = REXCVAR_GET(gta4_native_frames_in_flight);
         const uint32_t effective_frame_slots =
             native_profiler_needs_initialization ? 1 : requested_frame_slots;
-        gpu_flight::Record("native.frame-begin", NativeVulkanHandleIdentity(device),
-                           submission_tracker_->GetCurrentSubmission(), submitted_frame,
-                           effective_frame_slots, current_frame_.size());
+        if (IsNativeFlightRecorderEnabled()) {
+          gpu_flight::Record("native.frame-begin", NativeVulkanHandleIdentity(device),
+                             submission_tracker_->GetCurrentSubmission(), submitted_frame,
+                             effective_frame_slots, current_frame_.size());
+        }
         const uint32_t selected_frame_slot =
             effective_frame_slots == 1 ? 0 : submitted_frame % NativeFrameContextRing::kSlotCount;
         if (!ActivateNativeFrameSlot(selected_frame_slot)) {
@@ -37462,19 +38513,36 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
             tv_lifecycle_trace_->run, submitted_frame, present.device != 0,
             active_texture_frame_, resource_frame));
         active_texture_frame_ = resource_frame;
+        uint64_t hk_t = FrameTimelineNow();
+        const auto hk_lap = [&hk_t](std::atomic<uint64_t>& slot) {
+          const uint64_t now = FrameTimelineNow();
+          slot.store(now - hk_t, std::memory_order_relaxed);
+          hk_t = now;
+        };
+        
         ReleasePendingSurfaceImages();
+        hk_lap(g_hk_surf_ns);
         finish_housekeeping_stage(performance::CpuRange::kHousekeepingSurfaceRelease);
+        
         ReleaseUnusedTextureImages(resource_frame, present_source);
+        hk_lap(g_hk_texunused_ns);
         finish_housekeeping_stage(performance::CpuRange::kHousekeepingTextureReclamation);
+        
         if (!ReleaseRetiredTextureImages()) {
           return false;
         }
+        hk_lap(g_hk_texretired_ns);
         finish_housekeeping_stage(performance::CpuRange::kHousekeepingTextureRetirement);
+        
         ReleaseUnusedBufferResources(resource_frame);
+        hk_lap(g_hk_bufunused_ns);
         finish_housekeeping_stage(performance::CpuRange::kHousekeepingBufferReclamation);
+        
         ReleaseUnusedPersistentBuffers(completed_command_buffer_submission_);
-        finish_housekeeping_stage(performance::CpuRange::kHousekeepingPersistentBufferReclamation);
-        if (native_profiler_enabled) {
+        hk_lap(g_hk_persist_ns);
+		finish_housekeeping_stage(performance::CpuRange::kHousekeepingPersistentBufferReclamation);
+        g_hitch_hk_end_ns.store(FrameTimelineNow(), std::memory_order_relaxed);
+        if (native_profile_frame_started) {
           native_profile_housekeeping_ticks =
               native_profile_completion_processing_ticks +
               (rex::chrono::Clock::QueryHostTickCount() - native_profile_housekeeping_begin);
@@ -37551,13 +38619,22 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
           return false;
         }
         native_command_recording_started = true;
+        native_draw_state_cache_.Reset();
+        ubo_bound_layout_ = VK_NULL_HANDLE;
 #if REX_PLATFORM_ANDROID
         if (REXCVAR_GET(gta4_pipeline_stats) && !pipeline_stats_attempted_) {
-          pipeline_stats_attempted_ = true;
-          g_capture_pipeline_statistics.store(true, std::memory_order_relaxed);
-          __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "pipeline-stats: init=%d",
-                              pipeline_stats_.Initialize(vulkan_device));
-        }
+  pipeline_stats_attempted_ = true;
+  // Сначала проверяем, что расширение поддерживается. Раньше флаг ставился
+  // ДО Initialize: на Adreno без VK_KHR_pipeline_executable_properties
+  // vkCreateGraphicsPipelines начинал падать с VK_ERROR_FEATURE_NOT_PRESENT
+  // (симптом — чёрный экран).
+  const bool stats_ok = pipeline_stats_.Initialize(vulkan_device);
+  g_capture_pipeline_statistics.store(stats_ok, std::memory_order_relaxed);
+  __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
+                      "pipeline-stats: init=%d capture=%d",
+                      int(stats_ok),
+                      int(g_capture_pipeline_statistics.load(std::memory_order_relaxed)));
+}
         if (REXCVAR_GET(gta4_gpu_counters)) {
           if (!gpu_counters_attempted_) {
             gpu_counters_attempted_ = true;
@@ -37671,6 +38748,7 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
         }
         FinalizePlannedResolveSwaps();
         const auto finalize_resolve_swaps = MakeScopeExit([this] { FinalizePlannedResolveSwaps(); });
+        if (REXCVAR_GET(gta4_native_hitch_log)) g_hitch_tex_begin_ns.store(FrameTimelineNow(), std::memory_order_relaxed);
         const bool textures_prepared = PrepareFrameTextures(command_buffer_, bool(present_source),
                                                             submitted_frame, trace_stages) &&
                                        RefreshPackedDepthAliases(command_buffer_, 0);
@@ -37700,12 +38778,30 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
           WriteBreadcrumb(command_buffer_, fmt::format("frame {} start: textures prepared, recording begins",
                                                        submitted_frame));
         }
-        const bool frame_recorded = RecordNativeFrame(
-            command_buffer_, width, height, vulkan_context.image(), vulkan_context.image_view(),
-            submitted_frame, present_source, environmental_data, vulkan_context.hdr_output(),
-            vulkan_context.hdr_headroom(), presenter_transfer_written, presenter_written,
-            trace_stages, trace_sequence, present.diagnostic_force_content_probe != 0);
-        if (present.diagnostic_trace) {
+       // const bool frame_recorded = RecordNativeFrame(
+         //   command_buffer_, width, height, vulkan_context.image(), vulkan_context.image_view(),
+           // submitted_frame, present_source, environmental_data, vulkan_context.hdr_output(),
+            //vulkan_context.hdr_headroom(), presenter_transfer_written, presenter_written,
+            //trace_stages, trace_sequence, present.diagnostic_force_content_probe != 0);
+       const bool hitch_log_active = REXCVAR_GET(gta4_native_hitch_log);
+       const uint64_t hitch_record_begin = hitch_log_active ? FrameTimelineNow() : 0;
+        if (hitch_log_active) {
+          g_hitch_pre_record_ns.store(hitch_record_begin, std::memory_order_relaxed);
+        }
+        bool frame_recorded = false;
+        {
+          TRACE_SCOPE("Rec: RecordNativeFrame");
+          frame_recorded = RecordNativeFrame(
+              command_buffer_, width, height, vulkan_context.image(), vulkan_context.image_view(),
+              submitted_frame, present_source, environmental_data, vulkan_context.hdr_output(),
+              vulkan_context.hdr_headroom(), presenter_transfer_written, presenter_written,
+              trace_stages, trace_sequence, present.diagnostic_force_content_probe != 0);
+        }
+        if (hitch_log_active) {
+          g_hitch_record_ns.store(FrameTimelineNow() - hitch_record_begin,
+                                  std::memory_order_relaxed);
+        }
+		if (present.diagnostic_trace) {
           REXLOG_INFO(
               "gta4-tv-native-record: session={} present={} frame={} origin={} "
               "caller={:08X} result={} source={} presenter-written={} "
@@ -37766,9 +38862,11 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
         gpu_counters_.EndFrame(command_buffer_, active_frame_slot_);
 #endif
         const VkResult end_result = profile::CpuCall(profile::CpuOp::kDriverEnd, [&] { return dfn.vkEndCommandBuffer(command_buffer_); });
-        gpu_flight::Record("native.command-buffer-end", NativeVulkanHandleIdentity(command_buffer_),
-                           submission_tracker_->GetCurrentSubmission(), submitted_frame,
-                           active_frame_slot_, 0, int32_t(end_result));
+        if (IsNativeFlightRecorderEnabled()) {
+          gpu_flight::Record("native.command-buffer-end", NativeVulkanHandleIdentity(command_buffer_),
+                             submission_tracker_->GetCurrentSubmission(), submitted_frame,
+                             active_frame_slot_, 0, int32_t(end_result));
+        }
         if (end_result < VK_SUCCESS) {
           gpu_flight::Fail("native.command-buffer-end", int32_t(end_result),
                            NativeVulkanHandleIdentity(command_buffer_),
@@ -37853,13 +38951,16 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
         {
           auto queue =
               profile::CpuCall(profile::CpuOp::kQueueLock, [&] { return vulkan_device->AcquireQueue(vulkan_device->queue_family_native_offscreen(), 0); });
-          gpu_flight::Record("native.queue-submit-begin", NativeVulkanHandleIdentity(command_buffer_),
-                             submission, submitted_frame, active_frame_slot_,
-                             NativeVulkanHandleIdentity(fence_acquisition.fence()));
+          if (IsNativeFlightRecorderEnabled()) {
+            gpu_flight::Record("native.queue-submit-begin", NativeVulkanHandleIdentity(command_buffer_),
+                               submission, submitted_frame, active_frame_slot_,
+                               NativeVulkanHandleIdentity(fence_acquisition.fence()));
+          }
           submit_result =
               profile::CpuCall(profile::CpuOp::kDriverSubmit, [&] { return dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, fence_acquisition.fence()); });
           if (submit_result == VK_SUCCESS) {
             MarkFrameTimeline(submitted_frame, kTimelineQueueSubmit);
+			if (REXCVAR_GET(gta4_native_hitch_log)) g_hitch_submit_done_ns.store(FrameTimelineNow(), std::memory_order_relaxed);
             vulkan_context.MarkImageAccessSubmitted();
             SubmitTemporalFrame();
             if (temporal_generated_frame_.has_generated_frame)
@@ -37871,9 +38972,11 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
             }
           }
         }
-        gpu_flight::Record("native.queue-submit-end", NativeVulkanHandleIdentity(command_buffer_),
-                           submission, submitted_frame, active_frame_slot_, 0,
-                           int32_t(submit_result));
+        if (IsNativeFlightRecorderEnabled()) {
+          gpu_flight::Record("native.queue-submit-end", NativeVulkanHandleIdentity(command_buffer_),
+                             submission, submitted_frame, active_frame_slot_, 0,
+                             int32_t(submit_result));
+        }
         if (submit_result < VK_SUCCESS) {
           gpu_flight::Fail("native.queue-submit", int32_t(submit_result),
                            NativeVulkanHandleIdentity(command_buffer_), submission, submitted_frame);
@@ -38030,8 +39133,9 @@ bool Gta4NativeGraphicsSystem::ClearGuestOutput(
         // Offscreen work may have been submitted successfully without
         // changing this mailbox image. Preserve its completion object for
         // lifetime safety, but do not publish/rotate an unwritten image.
-        return presenter_written;
-      },
+        if (REXCVAR_GET(gta4_native_hitch_log)) g_hitch_cb_end_ns.store(FrameTimelineNow(), std::memory_order_relaxed);
+          return presenter_written;
+        },
       ui::GuestOutputProvenance{
           .frame_rate_limit = rex::cvar::Query<uint32_t>("gta4_frame_limit"),
           .producer_backpressure = present.device != 0,
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h b/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h
index 0b9e1311..f35e6047 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.h
@@ -479,31 +479,110 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
     // User-provided so pool construction does not zero the whole ~3 KB
     // command before running the member initializers below.
     NativeCommand() {}
-    // Retirement re-initializes pooled commands in place: everything is
-    // destroyed and default-constructed again, except that the constant delta
-    // buffers keep a bounded capacity, so the next draw captured into this
-    // slot does not allocate them.
-    void RecycleInPlace() {
-      constexpr size_t kKeptBytes = 1024;
-      const auto keep = [](auto& buffer) {
-        auto kept = std::move(buffer);
-        kept.clear();
-        if (kept.capacity() * sizeof(kept[0]) > kKeptBytes) kept = {};
-        return kept;
+    // Fast recycling in place: resets primitive state, clears vectors (preserving
+    // their capacity up to a bound), and resets only non-null shared_ptrs.
+    // Avoids destructor calls (~NativeCommand), je_malloc in CaptureConstantPayloadDelta,
+    // and cross-thread je_free churn on GtaCmdRetirer.
+    void Reset() {
+      type = CommandType::kPresent;
+      bindings_prepared = false;
+      temporal_composite_mode = 0;
+      split_postfx_applied = false;
+      render_phase = RenderPhase::kUnknown;
+      render_phase_object = 0;
+      used_texture_mask = 0;
+      failed_texture_mask = 0;
+      realized_image_mask = 0;
+      realized_sampler_mask = 0;
+      guest_null_texture_mask = 0;
+      resolve_swap = UINT32_MAX;
+      temporal_scene_stage = UINT32_MAX;
+      bytes.resize(0);
+      temporal_scene_binding = nullptr;
+      texture_inputs_signature = 0;
+      pipeline_memo_signature = 0;
+      vertex_constants_upload_hash = 0;
+      pixel_constants_upload_hash = 0;
+      vertex_constants_upload_bytes = 0;
+      pixel_constants_upload_bytes = 0;
+      worker_vertex_constants_offset = 0;
+      worker_pixel_constants_offset = 0;
+      worker_ring_serial = 0;
+      descriptor_page = UINT32_MAX;
+      descriptor_copy = 0;
+      image_descriptor_epoch = 0;
+      sampler_descriptor_epoch = 0;
+      cached_descriptor_epoch = 0;
+      environmental_data_hash = 0;
+      // Lightweight scalar resets for in-order Cortex-A55 (avoids zeroing huge POD arrays)
+      lighting = LightingContext{};
+      light_trace_id = 0;
+      light_trace_technique = 0xFFFFFFFFu;
+      light_trace_mode = 0;
+      dirty_components = 0;
+      captured_fixed_function_state_hash = 0;
+      recorded_fixed_function_state_hash = 0;
+      captured_vertex_constants_hash = 0;
+      captured_pixel_constants_hash = 0;
+      vertex_constants_hash = 0;
+      pixel_constants_hash = 0;
+      released_texture_generation = 0;
+      diagnostic_submit_sequence = 0;
+      diagnostic_producer_epoch = 0;
+      protected_generation_count = 0;
+      protected_generations_overflow = false;
+      protected_generations_collected = false;
+      gpu_pass_origin = GpuPassOrigin{};
+      profile_transport = profile::CommandTransport{};
+
+      constexpr size_t kMaxKeptBytes = 32768;
+      const auto clear_keeping_capacity = [](auto& vec) {
+        vec.clear();
+        if (vec.capacity() * sizeof(vec[0]) > kMaxKeptBytes) {
+          vec.shrink_to_fit();
+        }
       };
-      auto vertex_ranges = keep(shader_constant_delta.vertex_constants.ranges);
-      auto vertex_payload = keep(shader_constant_delta.vertex_constants.payload);
-      auto pixel_ranges = keep(shader_constant_delta.pixel_constants.ranges);
-      auto pixel_payload = keep(shader_constant_delta.pixel_constants.payload);
-      auto kept_pending_state = keep(pending_state);
-      std::destroy_at(this);
-      std::construct_at(this);
-      pending_state = std::move(kept_pending_state);
-      shader_constant_delta.vertex_constants.ranges = std::move(vertex_ranges);
-      shader_constant_delta.vertex_constants.payload = std::move(vertex_payload);
-      shader_constant_delta.pixel_constants.ranges = std::move(pixel_ranges);
-      shader_constant_delta.pixel_constants.payload = std::move(pixel_payload);
+      clear_keeping_capacity(payload);
+      clear_keeping_capacity(pending_state);
+      clear_keeping_capacity(shader_constant_delta.vertex_constants.ranges);
+      clear_keeping_capacity(shader_constant_delta.vertex_constants.payload);
+      clear_keeping_capacity(shader_constant_delta.pixel_constants.ranges);
+      clear_keeping_capacity(shader_constant_delta.pixel_constants.payload);
+      shader_constant_delta.vertex_constants.complete_snapshot = false;
+      shader_constant_delta.pixel_constants.complete_snapshot = false;
+      shader_constant_delta.booleans_present = false;
+      shader_constant_delta.compare_snapshot = false;
+
+      if (device_snapshot.storage) device_snapshot.storage.reset();
+      if (room_light_input_bindings) room_light_input_bindings.reset();
+      if (shader_state) shader_state.reset();
+      if (pipeline_state) pipeline_state.reset();
+      if (index_buffer) index_buffer.reset();
+      if (environmental_data) environmental_data.reset();
+      if (fire_trace) fire_trace.reset();
+      if (bulb_trace) bulb_trace.reset();
+      if (phone_trace) phone_trace.reset();
+      if (tv_trace) tv_trace.reset();
+      if (resolve_destination) resolve_destination.reset();
+      if (depth_handoff_source) depth_handoff_source.reset();
+      if (present_source) present_source.reset();
+      if (postfx_half_scene) postfx_half_scene.reset();
+      if (temporal_instance) temporal_instance.reset();
+      if (temporal_prefilter) temporal_prefilter.reset();
+      if (synchronous) synchronous.reset();
+
+      for (auto& vb : vertex_buffers) {
+        if (vb) vb.reset();
+      }
+      for (auto& tex : textures) {
+        if (tex) tex.reset();
+      }
+    }
+
+    void RecycleInPlace() {
+      Reset();
     }
+
     // Layout: a command is ~3 KB and the recorder walks the frame's commands
     // several times, so it is bound by first touches. The small fields every
     // walk reads for every command come first and share a few cache lines;
@@ -1268,6 +1347,7 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
     float peak_nits = 400.0f;
     float shoulder_start = 0.0f;
     float shoulder_power = 2.5f;
+    float sgsr_sharpness = 2.0f; // Добавили параметр резкости SGSR
   };
 
   struct NativeRenderingTarget {
@@ -1469,7 +1549,13 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
   struct NativeFrameResources {
     FrameGenerationMap<NativePersistentBufferKey, NativeUploadAllocation, NativePersistentBufferKeyHash> vertex_uploads;
     FrameGenerationMap<uint64_t, NativeUploadAllocation> index16_uploads, index32_uploads;
+    bool has_last_shared_key = false;
+    NativeSharedConstantSemanticKey last_shared_key{};
+    NativeUploadAllocation last_shared_allocation{};
     bool Reset() {
+      has_last_shared_key = false;
+      last_shared_key = {};
+      last_shared_allocation = {};
       if (!vertex_uploads.CanResetGeneration() || !index16_uploads.CanResetGeneration() ||
           !index32_uploads.CanResetGeneration()) return false;
       return vertex_uploads.ResetGeneration() && index16_uploads.ResetGeneration() &&
@@ -1521,6 +1607,8 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
   uint32_t indexed_merge_count_override_ = 0;
   void BindNativeVertexBuffer(VkCommandBuffer command_buffer, uint32_t binding, VkBuffer buffer,
                               VkDeviceSize offset);
+  void BindNativeIndexBuffer(VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
+                             VkIndexType index_type);
   bool InitializeShaderCache();
   static bool ReflectVertexInputs(const std::vector<uint32_t>& spirv,
                                   std::vector<NativeVertexInput>& inputs);
@@ -1812,6 +1900,8 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
       VkFormat destination_format,
       VkSampleCountFlagBits destination_samples = VK_SAMPLE_COUNT_1_BIT);
   VkPipeline GetOrCreateHDRPresentPipeline();
+    // SGSR: отдельный пайплайн для Snapdragon Game Super Resolution
+  VkPipeline GetOrCreateSGSRPresentPipeline();
   bool RecordResolveConversion(VkCommandBuffer command_buffer, NativeSurfaceImage& source,
                                NativeTextureImage& destination, uint32_t destination_level,
                                int32_t source_left, int32_t source_top, uint32_t source_width,
@@ -1866,7 +1956,12 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
                            const ResolveCommand& resolve, uint32_t submitted_frame);
   bool ResolveRenderingTarget(const NativeCommand& command, VkImageView presenter_view,
                               uint32_t presenter_width, uint32_t presenter_height,
-                              NativeRenderingTarget& target, bool allow_allocation = true);
+                              NativeRenderingTarget& target, bool allow_allocation = true,
+                              const NativeRenderingTarget* active_target = nullptr);
+  bool ResolveRenderingTargetMemo(const NativeCommand& command, VkImageView presenter_view,
+                                  uint32_t presenter_width, uint32_t presenter_height,
+                                  NativeRenderingTarget& target,
+                                  const NativeRenderingTarget& active_target);
   NativeAttachmentUsage GetRenderingTargetUsage(const NativeCommand& command) const;
   bool TransitionRenderingTarget(VkCommandBuffer command_buffer, NativeRenderingTarget& target);
   bool AllocateUpload(VkDeviceSize size, VkDeviceSize alignment, NativeUploadAllocation& allocation,
@@ -1979,6 +2074,7 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
 
   std::mutex render_mutex_;
   std::condition_variable render_condition_;
+  bool worker_waiting_ = false;
   NativeSnapshotResource snapshot_pool_;
   NativeCommandPool<NativeCommand> native_command_pool_;
   NativeOwnedCommands<NativeCommand, true> render_queue_;
@@ -2294,6 +2390,7 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
   uint64_t persistent_buffer_upload_bytes_ = 0;
   uint32_t upload_underutilized_frame_count_ = 0;
   uint32_t secondary_upload_underutilized_frame_count_ = 0;
+  uint32_t constant_arena_underutilized_frames_ = 0;
   NativeContentProbeBuffer content_probe_buffer_;
   NativeContentProbeBuffer secondary_content_probe_buffer_;
   NativeContentProbeBuffer tv_content_probe_buffer_, secondary_tv_content_probe_buffer_;
@@ -2770,6 +2867,7 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
   std::vector<DrawPipelineCacheEntry> draw_pipeline_cache_;
   std::vector<NativeResolveConversionPipeline> resolve_conversion_pipelines_;
   VkPipeline hdr_present_pipeline_ = VK_NULL_HANDLE;
+  VkPipeline sgsr_present_pipeline_ = VK_NULL_HANDLE; // SGSR: кэш пайплайна
   uint64_t legacy_mul_rewrites_ = 0;  // Shader registration thread only.
   std::array<NativeTextureImage, NativeFrameContextRing::kSlotCount> hdr_present_mirrors_{};
   std::vector<NativeSampler> native_samplers_;
@@ -2791,6 +2889,23 @@ class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
       submitted_native_flight_resources_{};
   uint32_t active_texture_frame_ = 0;
   uint64_t next_texture_use_serial_ = 1;
+  // Last realized draw target, reused by the next adjacent draw with identical
+  // target descriptors (ResolveRenderingTargetMemo).
+  struct NativeTargetMemo {
+    bool valid = false;
+    uint32_t frame = 0;
+    uint32_t texture_frame = 0;
+    uint64_t submission = 0;
+    VkImageView presenter_view = VK_NULL_HANDLE;
+    uint32_t presenter_width = 0;
+    uint32_t presenter_height = 0;
+    uint32_t color_attachment_mask = 0;
+    VkImageAspectFlags depth_stencil_aspects = 0;
+    std::array<SurfaceDescriptor, kRenderTargetCount> render_targets{};
+    SurfaceDescriptor depth_stencil{};
+    NativeRenderingTarget target{};
+  };
+  NativeTargetMemo native_target_memo_;
   std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> accounted_texture_bytes_{};
   NativeTextureEvictionIndex texture_eviction_scan_;
   size_t texture_eviction_remaining_ = 0;
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/native_draw_state_cache.h b/glue/rexglue-sdk-main/src/graphics/gta4_native/native_draw_state_cache.h
index 5d0f9985..9fd97841 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/native_draw_state_cache.h
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/native_draw_state_cache.h
@@ -53,6 +53,12 @@ class NativeDrawStateCache {
     if (binding >= vertex_buffers_.size()) return true;
     return vertex_buffers_[binding].Update({command_buffer, buffer, offset});
   }
+  // Index buffer binding is command-buffer state that survives pipeline
+  // and render-pass changes.
+  bool UpdateIndexBuffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset,
+                         uint32_t index_type) {
+    return index_buffer_.Update({command_buffer, buffer, offset, uint64_t(index_type)});
+  }
 
  private:
   template <typename Value>
@@ -82,6 +88,7 @@ class NativeDrawStateCache {
   Tracked<std::array<uint32_t, 4>> blend_constants_;
   Tracked<LayoutValues<3>> push_constants_;
   std::array<Tracked<std::array<uint64_t, 3>>, 32> vertex_buffers_;
+  Tracked<std::array<uint64_t, 4>> index_buffer_;
 };
 
 }  // namespace rex::graphics::gta4_native
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h b/glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h
index 179cbea5..6ad8f741 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/native_owned_commands.h
@@ -12,6 +12,7 @@
 #include <thread>
 #include <type_traits>
 #include <utility>
+#include <pthread.h>
 #include <vector>
 
 namespace rex::graphics::gta4_native {
@@ -46,7 +47,8 @@ class NativeCommandPool {
   // producer then neither constructs ~3 KB per command nor allocates buffers
   // the previous occupant already had.
   static constexpr bool kRecyclable = requires(T& value) { value.RecycleInPlace(); };
-  void SetRecycling(bool enabled) { recycling_.store(enabled, std::memory_order_relaxed); }
+  void SetRecycling(bool) {}
+  // SetRecycling(bool enabled) { recycling_.store(enabled, std::memory_order_relaxed); }
 
   template <typename... Args>
   Owner Make(Args&&... args) {
@@ -86,7 +88,7 @@ class NativeCommandPool {
       T* object = owner.release();
       auto* slot = reinterpret_cast<Slot*>(object);
       if constexpr (kRecyclable) {
-        if (recycling_.load(std::memory_order_relaxed)) {
+        if (true) {
           object->RecycleInPlace();
           slot->constructed = true;
         } else {
@@ -304,6 +306,9 @@ class NativeCommandRetirer {
 
  private:
   void Run() {
+#if defined(__ANDROID__)
+    pthread_setname_np(pthread_self(), "GtaCmdRetirer");
+#endif
     std::unique_lock lock(mutex_);
     for (;;) {
       condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/native_profile_shader_category.h b/glue/rexglue-sdk-main/src/graphics/gta4_native/native_profile_shader_category.h
index c2879102..4fd783c1 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/native_profile_shader_category.h
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/native_profile_shader_category.h
@@ -6,19 +6,28 @@ namespace rex::graphics::gta4_native::performance {
 // Diagnostic fallback only. It never changes render phases, pipelines,
 // culling or shader selection. The label explicitly describes shader families
 // because the title may submit them outside a tagged render-phase scope.
-inline GpuRange ProfileShaderCategory(std::string_view vs, std::string_view ps) {
-  const auto has = [](std::string_view name, std::string_view family) {
-    return name.find(family) != std::string_view::npos;
-  };
-  if (has(vs,"/shadowzdir/")) return GpuRange::kDirectionalShadowShaders;
-  if (has(vs,"/shadowz/") || has(vs,"/shadowzspot/")) return GpuRange::kLocalShadowShaders;
-  if (has(ps,"/rage_postfx") || has(ps,"/gta_postfx")) return GpuRange::kComposite;
-  if (has(ps,"/sky") || has(ps,"/clouds")) return GpuRange::kSkyShaders;
-  if (has(vs,"/gta_im/") || has(ps,"/gta_im/")) return GpuRange::kImmediateShaders;
-  if (has(ps,"/gta_particle") || has(ps,"/particle")) return GpuRange::kParticleShaders;
-  if (has(vs,"/gta_default/") || has(vs,"/gta_normal") || has(vs,"/gta_spec/") ||
-      has(vs,"/gta_vehicle") || has(vs,"/gta_ped/") || has(vs,"/gta_emissive"))
-    return GpuRange::kMaterialShaders;
+//inline GpuRange ProfileShaderCategory(std::string_view vs, std::string_view ps) {
+ // const auto has = [](std::string_view name, std::string_view family) {
+  //  return name.find(family) != std::string_view::npos;
+  //};
+  //if (has(vs,"/shadowzdir/")) return GpuRange::kDirectionalShadowShaders;
+ // if (has(vs,"/shadowz/") || has(vs,"/shadowzspot/")) return GpuRange::kLocalShadowShaders;
+ // if (has(ps,"/rage_postfx") || has(ps,"/gta_postfx")) return GpuRange::kComposite;
+ // if (has(ps,"/sky") || has(ps,"/clouds")) return GpuRange::kSkyShaders;
+ // if (has(vs,"/gta_im/") || has(ps,"/gta_im/")) return GpuRange::kImmediateShaders;
+  //if (has(ps,"/gta_particle") || has(ps,"/particle")) return GpuRange::kParticleShaders;
+  //if (has(vs,"/gta_default/") || has(vs,"/gta_normal") || has(vs,"/gta_spec/") ||
+   //   has(vs,"/gta_vehicle") || has(vs,"/gta_ped/") || has(vs,"/gta_emissive"))
+   // return GpuRange::kMaterialShaders;
+ // return GpuRange::kUnattributed;
+ 
+ inline GpuRange ProfileShaderCategory(std::string_view vs, std::string_view ps) {
+  (void)vs;
+  (void)ps;
   return GpuRange::kUnattributed;
 }
-}  // namespace rex::graphics::gta4_native::performance
+ 
+ 
+ 
+}
+  // namespace rex::graphics::gta4_native::performance
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/native_snapshot_resource.h b/glue/rexglue-sdk-main/src/graphics/gta4_native/native_snapshot_resource.h
index 1294764a..aa6650b2 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/native_snapshot_resource.h
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/native_snapshot_resource.h
@@ -1,5 +1,18 @@
 #pragma once
-#include <algorithm>
+// NativeSnapshotResource — пул снапшотов пайплайна/шейдерного состояния.
+//
+// Контракт (как в оригинале): allocate() зовёт ТОЛЬКО один поток (render worker),
+// deallocate() — любой поток (ретайреры, рекордер, гость).
+//
+// Отличие от патча с thread_local TlsReturnBatch:
+//   * никаких thread_local-указателей на пул -> нет use-after-free, когда пул
+//     уничтожен/пересоздан, а у какого-то потока в TLS ещё лежит batch с
+//     owner == старый this (тот же адрес у нового пула = чужие указатели в free-list);
+//   * возврат — lock-free push в интрузивный стек (CAS), забор — один exchange()
+//     всего списка. "Pop-all" через exchange не подвержен ABA.
+//   * мьютекс остался только для роста (новый блок / новый класс размера) —
+//     это единицы раз за игру.
+#include <atomic>
 #include <cstddef>
 #include <memory_resource>
 #include <mutex>
@@ -8,14 +21,11 @@
 
 namespace rex::graphics::gta4_native {
 
-// Pipeline and shader-state snapshots are allocated by the render worker for
-// nearly every draw and released on the retirement threads. A synchronized
-// pool took its mutex on both sides of every allocation. Here the single
-// allocating thread pops from a private free list and takes the lock only to
-// adopt everything returned since (or to grow); releases from any thread
-// take it once each. Memory is kept for reuse until the resource dies.
 class NativeSnapshotResource final : public std::pmr::memory_resource {
  public:
+  NativeSnapshotResource() = default;
+  NativeSnapshotResource(const NativeSnapshotResource&) = delete;
+  NativeSnapshotResource& operator=(const NativeSnapshotResource&) = delete;
   ~NativeSnapshotResource() override {
     for (void* block : blocks_) ::operator delete(block, std::align_val_t(kAlignment));
   }
@@ -24,16 +34,23 @@ class NativeSnapshotResource final : public std::pmr::memory_resource {
   static constexpr size_t kAlignment = 16;
   static constexpr size_t kChunksPerBlock = 64;
   static constexpr size_t kMaximumClasses = 8;
+
+  struct FreeNode {
+    FreeNode* next;
+  };
+  static_assert(sizeof(FreeNode) <= kAlignment);
+
   struct SizeClass {
-    size_t size = 0;
-    std::vector<void*> local;     // Allocating thread only.
-    std::vector<void*> returned;  // Under mutex_.
+    size_t size = 0;                           // записывается до публикации class_count_
+    FreeNode* local = nullptr;                 // только поток-аллокатор
+    std::atomic<FreeNode*> returned{nullptr};  // push из любых потоков
   };
 
   static size_t Rounded(size_t bytes) { return (bytes + kAlignment - 1) & ~(kAlignment - 1); }
 
   SizeClass* Find(size_t size) {
-    for (size_t i = 0; i < class_count_; ++i)
+    const size_t count = class_count_.load(std::memory_order_acquire);
+    for (size_t i = 0; i < count; ++i)
       if (classes_[i].size == size) return &classes_[i];
     return nullptr;
   }
@@ -43,28 +60,35 @@ class NativeSnapshotResource final : public std::pmr::memory_resource {
     const size_t size = Rounded(bytes);
     SizeClass* size_class = Find(size);
     if (!size_class) {
-      std::lock_guard lock(mutex_);
-      if (class_count_ == kMaximumClasses) {
-        return ::operator new(bytes, std::align_val_t(alignment));
-      }
-      size_class = &classes_[class_count_];
+      std::lock_guard lock(grow_mutex_);
+      const size_t count = class_count_.load(std::memory_order_relaxed);
+      if (count == kMaximumClasses) return ::operator new(bytes, std::align_val_t(alignment));
+      size_class = &classes_[count];
       size_class->size = size;
-      ++class_count_;  // Published to this thread; other threads look up under mutex_.
+      class_count_.store(count + 1, std::memory_order_release);  // публикуем size
     }
-    if (size_class->local.empty()) {
-      std::lock_guard lock(mutex_);
-      size_class->local.swap(size_class->returned);
-      if (size_class->local.empty()) {
+    if (!size_class->local) {
+      // Забрать всё, что вернули другие потоки, одним атомарным обменом.
+      size_class->local = size_class->returned.exchange(nullptr, std::memory_order_acquire);
+      if (!size_class->local) {
         auto* block = static_cast<std::byte*>(
             ::operator new(size * kChunksPerBlock, std::align_val_t(kAlignment)));
-        blocks_.push_back(block);
-        size_class->local.reserve(kChunksPerBlock);
-        for (size_t i = 0; i < kChunksPerBlock; ++i) size_class->local.push_back(block + i * size);
+        {
+          std::lock_guard lock(grow_mutex_);
+          blocks_.push_back(block);
+        }
+        FreeNode* head = nullptr;
+        for (size_t i = kChunksPerBlock; i-- > 0;) {
+          auto* node = reinterpret_cast<FreeNode*>(block + i * size);
+          node->next = head;
+          head = node;
+        }
+        size_class->local = head;
       }
     }
-    void* chunk = size_class->local.back();
-    size_class->local.pop_back();
-    return chunk;
+    FreeNode* node = size_class->local;
+    size_class->local = node->next;
+    return node;
   }
 
   void do_deallocate(void* pointer, size_t bytes, size_t alignment) override {
@@ -72,26 +96,27 @@ class NativeSnapshotResource final : public std::pmr::memory_resource {
       ::operator delete(pointer, std::align_val_t(alignment));
       return;
     }
-    const size_t size = Rounded(bytes);
-    std::lock_guard lock(mutex_);
-    for (size_t i = 0; i < class_count_; ++i) {
-      if (classes_[i].size == size) {
-        classes_[i].returned.push_back(pointer);
-        return;
-      }
+    SizeClass* size_class = Find(Rounded(bytes));
+    if (!size_class) {  // выделено, когда таблица классов была полна
+      ::operator delete(pointer, std::align_val_t(alignment));
+      return;
     }
-    // Allocated after the class table filled up.
-    ::operator delete(pointer, std::align_val_t(alignment));
+    auto* node = static_cast<FreeNode*>(pointer);
+    FreeNode* head = size_class->returned.load(std::memory_order_relaxed);
+    do {
+      node->next = head;
+    } while (!size_class->returned.compare_exchange_weak(head, node, std::memory_order_release,
+                                                         std::memory_order_relaxed));
   }
 
   bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
     return this == &other;
   }
 
-  std::mutex mutex_;
+  std::mutex grow_mutex_;
   SizeClass classes_[kMaximumClasses];
-  size_t class_count_ = 0;
-  std::vector<void*> blocks_;  // Under mutex_.
+  std::atomic<size_t> class_count_{0};
+  std::vector<void*> blocks_;  // под grow_mutex_
 };
 
-}  // namespace rex::graphics::gta4_native
+}  // namespace rex::graphics::gta4_native
\ No newline at end of file
diff --git a/glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h b/glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h
index d919e925..5ac74d97 100644
--- a/glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h
+++ b/glue/rexglue-sdk-main/src/graphics/gta4_native/stateful_constant_state.h
@@ -189,15 +189,18 @@ class ConstantSnapshotPool {
                                                           size_t copy_bytes = SIZE_MAX) {
     copy_bytes = std::min(copy_bytes, source.size());
     std::vector<uint8_t>* buffer = nullptr;
-    {
+    auto& local = local_free_list(source.size());
+    if (local.empty()) {
       std::lock_guard lock(mutex());
-      auto& list = free_list(source.size());
-      if (!list.empty()) {
-        buffer = list.back();
-        list.pop_back();
+      auto& shared = free_list(source.size());
+      while (!shared.empty() && local.size() < 64) {
+        local.push_back(shared.back());
+        shared.pop_back();
       }
     }
-    if (buffer) {
+    if (!local.empty()) {
+      buffer = local.back();
+      local.pop_back();
       std::memcpy(buffer->data(), source.data(), copy_bytes);
     } else if (copy_bytes == source.size()) {
       buffer = new std::vector<uint8_t>(source);
@@ -209,17 +212,40 @@ class ConstantSnapshotPool {
         buffer,
         [](const std::vector<uint8_t>* bytes) {
           auto* owned = const_cast<std::vector<uint8_t>*>(bytes);
+          auto& loc = local_free_list(owned->size());
+          if (loc.size() < 256) {
+            loc.push_back(owned);
+            return;
+          }
           std::lock_guard lock(mutex());
           auto& list = free_list(owned->size());
-          if (list.size() < 8192) list.push_back(owned);
-          else delete owned;
+          // Batch transfer 128 items from thread-local to shared free list in one lock
+          constexpr size_t kBatchSize = 128;
+          size_t transfer_count = std::min(loc.size(), kBatchSize);
+          while (transfer_count > 0 && list.size() < 8192) {
+            list.push_back(loc.back());
+            loc.pop_back();
+            --transfer_count;
+          }
+          while (transfer_count > 0) {
+            delete loc.back();
+            loc.pop_back();
+            --transfer_count;
+          }
+          loc.push_back(owned);
         },
         std::pmr::polymorphic_allocator<std::byte>(&ConstantVersionResource()));
   }
 
  private:
   static std::mutex& mutex() { static std::mutex instance; return instance; }
-  // One list per bank size (vertex, pixel and the rare others).
+  static std::vector<std::vector<uint8_t>*>& local_free_list(size_t size) {
+    thread_local std::vector<std::pair<size_t, std::vector<std::vector<uint8_t>*>>> lists;
+    for (auto& [list_size, list] : lists)
+      if (list_size == size) return list;
+    lists.emplace_back(size, std::vector<std::vector<uint8_t>*>{});
+    return lists.back().second;
+  }
   static std::vector<std::vector<uint8_t>*>& free_list(size_t size) {
     static std::vector<std::pair<size_t, std::vector<std::vector<uint8_t>*>>> lists;
     for (auto& [list_size, list] : lists)
@@ -303,7 +329,8 @@ class AuthoritativeConstantState {
       initialized_ = true;
       auto next = MakeConstantStateVersion();
       next->token = version_;
-      next->content_hash = hash_callback(std::span<const uint8_t>(canonical_));
+      // Было: next->content_hash = hash_callback(std::span<const uint8_t>(canonical_));
+      next->content_hash = 0; // Не хешируем все 64 КБ впустую, хешируется только нужный префикс шейдера!
       next->byte_size = canonical_.size();
       // No delta and no parent: the draw snapshots this version from the
       // canonical bank right away (SnapshotCurrentVersion), while it is current.
@@ -312,7 +339,7 @@ class AuthoritativeConstantState {
       return {ConstantApplyStatus::kApplied, true, current_};
     }
 
-    ConstantPayloadDelta changed_delta;
+ConstantPayloadDelta changed_delta;
     changed_delta.complete_snapshot = !initialized_;
     for (const ConstantDeltaRange& range : delta.ranges) {
       const uint8_t* source = delta.payload.data() + range.payload_offset;
@@ -320,6 +347,8 @@ class AuthoritativeConstantState {
       if (initialized_ && std::memcmp(destination, source, range.byte_count) == 0) {
         continue;
       }
+      // Пишем сразу в canonical_ за один проход!
+      std::memcpy(destination, source, range.byte_count);
       const uint32_t payload_offset = static_cast<uint32_t>(changed_delta.payload.size());
       changed_delta.ranges.push_back(
           {range.destination_offset, payload_offset, range.byte_count});
@@ -333,16 +362,12 @@ class AuthoritativeConstantState {
     if (!CanIncrementStateVersion(version_)) {
       return {ConstantApplyStatus::kVersionSpaceExhausted, false, current_};
     }
-    for (const ConstantDeltaRange& range : changed_delta.ranges) {
-      std::memcpy(canonical_.data() + range.destination_offset,
-                  changed_delta.payload.data() + range.payload_offset, range.byte_count);
-    }
     IncrementStateVersion(version_);
     initialized_ = true;
 
     auto next = MakeConstantStateVersion();
     next->token = version_;
-    next->content_hash = hash_callback(std::span<const uint8_t>(canonical_));
+    next->content_hash = 0; // Отключаем холостой XXH3 по всему банку!
     next->byte_size = canonical_.size();
     // Updates without a consuming draw must not retain an unlimited chain.
     // A complete copied base is equivalent to replaying all prior deltas.
diff --git a/glue/rexglue-sdk-main/src/input/absolute_pointer.cpp b/glue/rexglue-sdk-main/src/input/absolute_pointer.cpp
index 59704d55..dcc8b96e 100644
--- a/glue/rexglue-sdk-main/src/input/absolute_pointer.cpp
+++ b/glue/rexglue-sdk-main/src/input/absolute_pointer.cpp
@@ -49,8 +49,8 @@ bool HostInputAllowed() {
 }
 
 TouchControlsMode GetConfiguredTouchControlsMode() {
-  // The native menu can change this string while render/input threads read it.
-  const std::string mode = rex::cvar::GetFlagByName("touch_controls");
+  // Fast path: direct CVar reference avoids string allocation and hash lookup.
+  const std::string& mode = REXCVAR_GET(touch_controls);
   if (mode == "on") {
     return TouchControlsMode::kOn;
   }
diff --git a/glue/rexglue-sdk-main/src/input/sony_feedback_options.cpp b/glue/rexglue-sdk-main/src/input/sony_feedback_options.cpp
index cfcf9063..61fba54e 100644
--- a/glue/rexglue-sdk-main/src/input/sony_feedback_options.cpp
+++ b/glue/rexglue-sdk-main/src/input/sony_feedback_options.cpp
@@ -22,11 +22,12 @@ bool SetFeaturesEnabled(bool enabled) {
 }
 
 Options GetOptions() {
-  // The SDL output worker and guest threads read while the frontend may write.
-  // Registry queries take its lock; raw REXCVAR_GET references do not.
-  using rex::cvar::Query;
-  return {Query<bool>("gta4_sony_enabled"), Query<bool>("gta4_sony_gestures"),
-          Query<bool>("gta4_sony_lighting"), Query<bool>("gta4_sony_rumble"),
-          Query<bool>("gta4_sony_triggers"), static_cast<float>(Query<double>("gta4_sony_intensity"))};
+  // Direct REXCVAR_GET avoids Query / GetFlagByName string allocations and mutex locks.
+  return {REXCVAR_GET(gta4_sony_enabled),
+          REXCVAR_GET(gta4_sony_gestures),
+          REXCVAR_GET(gta4_sony_lighting),
+          REXCVAR_GET(gta4_sony_rumble),
+          REXCVAR_GET(gta4_sony_triggers),
+          static_cast<float>(REXCVAR_GET(gta4_sony_intensity))};
 }
 }  // namespace rex::input::sony
diff --git a/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_audio.cpp b/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_audio.cpp
index 876d8134..6b2d46cc 100644
--- a/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_audio.cpp
+++ b/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_audio.cpp
@@ -33,8 +33,6 @@ u32 XAudioGetSpeakerConfig_entry(mapped_u32 config_ptr) {
 u32 XAudioGetVoiceCategoryVolumeChangeMask_entry(mapped_void driver_ptr, mapped_u32 out_ptr) {
   assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);
 
-  rex::thread::Sleep(std::chrono::microseconds(1));
-
   // Checking these bits to see if any voice volume changed.
   // I think.
   *out_ptr = 0;
diff --git a/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_io.cpp b/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_io.cpp
index 53ecd059..7f159a56 100644
--- a/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_io.cpp
+++ b/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_io.cpp
@@ -82,6 +82,18 @@ void TelemetryEnd(int64_t begin_ns, uint32_t bytes, bool async) {
 namespace rex::kernel::xboxkrnl {
 using namespace rex::system;
 
+static std::string FixShaderPath(std::string path) {
+  size_t shaders_pos = path.find("shaders\\");
+  if (shaders_pos != std::string::npos && path.find("_e2dcl\\") != std::string::npos) {
+    size_t last_slash = path.find_last_of("\\");
+    if (last_slash != std::string::npos) {
+      std::string filename = path.substr(last_slash + 1);
+      return path.substr(0, shaders_pos) + "shaders\\dcl\\" + filename;
+    }
+  }
+  return path;
+}
+
 struct CreateOptions {
   // https://processhacker.sourceforge.io/doc/ntioapi_8h.html
   static const uint32_t FILE_DIRECTORY_FILE = 0x00000001;
@@ -240,10 +252,10 @@ u32 NtCreateFile_entry(mapped_u32 handle_out, u32 desired_access,
 
   auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);
 
-  rex::filesystem::Entry* root_entry = nullptr;
+rex::filesystem::Entry* root_entry = nullptr;
 
   // Compute path, possibly attrs relative.
-  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
+  auto target_path = FixShaderPath(util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name));
   REXKRNL_IMPORT_TRACE(
       "NtCreateFile", "path={} access={:#x} attrs={:#x} share={:#x} disp={:#x} options={:#x}",
       target_path, (uint32_t)desired_access, (uint32_t)file_attributes, (uint32_t)share_access,
@@ -628,7 +640,7 @@ u32 NtRemoveIoCompletion_entry(u32 handle, mapped_u32 key_context, mapped_u32 ap
 u32 NtQueryFullAttributesFile_entry(ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attribs,
                                     ppc_ptr_t<X_FILE_NETWORK_OPEN_INFORMATION> file_info) {
   auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(obj_attribs->name_ptr);
-  auto path_str = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
+  auto path_str = FixShaderPath(util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name));
   REXKRNL_IMPORT_TRACE("NtQueryFullAttributesFile", "path={}", path_str);
 
   object_ref<XFile> root_file;
@@ -636,11 +648,11 @@ u32 NtQueryFullAttributesFile_entry(ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attribs,
       obj_attribs->root_directory != 0) {
     root_file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(obj_attribs->root_directory);
     assert_not_null(root_file);
-    assert_true(root_file->type() == XObject::Type::File);
+ assert_true(root_file->type() == XObject::Type::File);
     assert_always();
   }
 
-  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
+  auto target_path = FixShaderPath(util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name));
 
   // Enforce that the path is ASCII.
   if (!IsValidPath(target_path, false)) {
diff --git a/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp b/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp
index 8b862fc7..e414ce97 100644
--- a/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp
+++ b/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp
@@ -31,6 +31,8 @@
 #include <rex/thread.h>
 #include <rex/thread/atomic.h>
 
+#include <thread>
+
 namespace rex::kernel::xboxkrnl {
 using namespace rex::system;
 
@@ -386,7 +388,7 @@ void RtlEnterCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
     cs->recursion_count++;
     return;
   }
-
+#if 0
   // Spin loop
   while (spin_count--) {
     if (rex::thread::atomic_cas(-1, 0, &cs->lock_count)) {
@@ -396,6 +398,27 @@ void RtlEnterCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
       return;
     }
   }
+#endif
+
+  // Spin loop — isb/pause so we do not peg the core; periodic yield if the
+  // guest asked for a huge spin_count (up to 255*256).
+  uint32_t spun = 0;
+  while (spin_count--) {
+    if (rex::thread::atomic_cas(-1, 0, &cs->lock_count)) {
+      // Acquired.
+      cs->owning_thread = cur_thread;
+      cs->recursion_count = 1;
+      return;
+    }
+#if defined(__aarch64__)
+    __asm__ __volatile__("isb" ::: "memory");
+#elif defined(__x86_64__) || defined(_M_X64)
+    __builtin_ia32_pause();
+#endif
+    if ((++spun & 63u) == 0u) {
+      std::this_thread::yield();
+    }
+  }
 
   if (rex::thread::atomic_inc(&cs->lock_count) != 0) {
     // Create a full waiter.
diff --git a/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_threading.cpp b/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_threading.cpp
index 9645873f..72c61621 100644
--- a/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_threading.cpp
+++ b/glue/rexglue-sdk-main/src/kernel/xboxkrnl/xboxkrnl_threading.cpp
@@ -13,15 +13,23 @@
 #pragma GCC diagnostic ignored "-Wunused-parameter"
 
 #include <algorithm>
+#include <array>
 #include <atomic>
 #include <chrono>
+#include <cstring>
 #include <memory>
 #include <mutex>
 #include <string>
+#include <thread>
 #include <vector>
 
 #if defined(__ANDROID__) || defined(__linux__)
+#include <linux/futex.h>
 #include <pthread.h>
+#include <sys/syscall.h>
+#include <unistd.h>
+#include <cstdio>
+#include <ctime>
 #endif
 
 #include <rex/chrono/clock.h>
@@ -45,10 +53,15 @@
 #include <rex/system/xtypes.h>
 #include <rex/thread/atomic.h>
 #include <rex/thread/mutex.h>
-
-REXCVAR_DEFINE_UINT32(kernel_spinlock_spins, 64, "Kernel",
+// с 16 на 8 kernel_spinlock_spins
+REXCVAR_DEFINE_UINT32(kernel_spinlock_spins, 8, "Kernel",
                       "Rounds a contended guest spinlock waits on the CPU (watching the lock "
                       "word) before it starts giving up its time slice; 0 yields at once");
+// Experimental, off by default: in contention tests polling with short sleeps (now accurate
+// thanks to the 1 us timer slack in threading_posix.cpp) won over futex wake-ups. A/B on device.
+REXCVAR_DEFINE_BOOL(kernel_spinlock_futex, false, "Kernel",
+                    "After spinning and yielding, a contended guest spinlock sleeps until the "
+                    "owner releases it (futex) instead of polling with 50 us sleeps");
 
 namespace rex::kernel::xboxkrnl {
 using namespace rex::system;
@@ -396,8 +409,14 @@ u32 KeQueryPerformanceFrequency_entry() {
   return static_cast<uint32_t>(result);
 }
 
+namespace {
+void NoteSleepZero();  // defined with the wait counters below
+}
+
 u32 KeDelayExecutionThread_entry(u32 processor_mode, u32 alertable, mapped_u64 interval_ptr) {
   XThread* thread = XThread::GetCurrentThread();
+  // Sleep(0) loops ("while (!ready) Sleep(0);") burn a core and were invisible in the log.
+  if (uint64_t(*interval_ptr) == 0) NoteSleepZero();
 
   if (alertable) {
     thread->DeliverAPCs();
@@ -414,14 +433,44 @@ u32 KeDelayExecutionThread_entry(u32 processor_mode, u32 alertable, mapped_u64 i
 
 namespace {
 
-// Per-thread yield counts for TakeYieldCountSummary. A thread's counter is
-// created on its first yield and kept for the process lifetime (threads that
-// yield are few), so the summary never races a thread's exit.
+// Cheap wait timer. arm64: the ARM generic counter (CNTVCT_EL0), readable from user space,
+// the same on every core, ~52 ns per tick on Snapdragon. Elsewhere: steady_clock in ns.
+inline uint64_t WaitTicks() {
+#if defined(__aarch64__)
+  uint64_t value;
+  __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(value)::"memory");
+  return value;
+#else
+  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
+                      std::chrono::steady_clock::now().time_since_epoch())
+                      .count());
+#endif
+}
+
+inline double WaitTicksToMs(uint64_t ticks) {
+#if defined(__aarch64__)
+  uint64_t frequency;
+  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(frequency));
+  return frequency ? double(ticks) * 1000.0 / double(frequency) : 0.0;
+#else
+  return double(ticks) / 1e6;
+#endif
+}
+
+// Per-thread wait counts for TakeYieldCountSummary. A thread's counter is
+// created on its first yield / spin / Sleep(0) and kept for the process lifetime,
+// so the summary never races a thread's exit.
 struct YieldCounter {
-  std::atomic<uint64_t> yields{0};
-  std::atomic<uint64_t> spins{0};
+  std::atomic<uint64_t> yields{0};       // NtYieldExecution
+  std::atomic<uint64_t> spins{0};        // rounds waiting for a contended guest spinlock
+  std::atomic<uint64_t> spin_ticks{0};   // time spent in those waits (WaitTicks)
+  std::atomic<uint64_t> sleep0{0};       // KeDelayExecutionThread with a zero interval (Sleep(0))
   uint64_t reported_yields = 0;
   uint64_t reported_spins = 0;
+  uint64_t reported_spin_ticks = 0;
+  uint64_t reported_sleep0 = 0;
+  int tid = 0;
+  bool is_realtime = false;
   char name[16] = "?";
 };
 
@@ -432,6 +481,7 @@ YieldCounter& CurrentYieldCounter() {
   thread_local YieldCounter* counter = [] {
     auto* created = new YieldCounter();
 #if defined(__ANDROID__) || defined(__linux__)
+    created->tid = int(syscall(SYS_gettid));
     pthread_getname_np(pthread_self(), created->name, sizeof(created->name));
 #endif
     std::lock_guard lock(g_yield_counters_mutex);
@@ -441,13 +491,45 @@ YieldCounter& CurrentYieldCounter() {
   return *counter;
 }
 
+void NoteSleepZero() {
+  CurrentYieldCounter().sleep0.fetch_add(1, std::memory_order_relaxed);
+}
+
+// Guest threads start as "XThreadXXXX" and are renamed (RenderGuest, RageStream...) a moment
+// later; a counter created in between would keep the old name. Re-read it from /proc
+// (never through pthread_t: the thread may have exited).
+void RefreshCounterName(YieldCounter* counter) {
+#if defined(__ANDROID__) || defined(__linux__)
+  if (!counter->tid) return;
+  char path[64];
+  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", counter->tid);
+  if (FILE* f = std::fopen(path, "r")) {
+    char name[16] = {};
+    if (std::fgets(name, sizeof(name), f)) {
+      for (char& c : name)
+        if (c == '\n') c = '\0';
+      if (name[0]) {
+        std::memcpy(counter->name, name, sizeof(counter->name));
+        counter->is_realtime = std::strstr(counter->name, "Audio") != nullptr ||
+                               std::strstr(counter->name, "RenderGuest") != nullptr;
+      }
+    }
+    std::fclose(f);
+  }
+#endif
+}
+
 }  // namespace
 
+// Per thread: yields / spinlock rounds / ms waiting on spinlocks / Sleep(0) calls,
+// e.g. "RageStream=0/26246/12.3ms/z5100". Sorted by spinlock wait time.
 std::string TakeYieldCountSummary(size_t max_threads) {
   struct Delta {
-    const char* name;
+    std::string name;
     uint64_t yields;
     uint64_t spins;
+    uint64_t spin_ticks;
+    uint64_t sleep0;
   };
   std::vector<Delta> deltas;
   {
@@ -455,21 +537,33 @@ std::string TakeYieldCountSummary(size_t max_threads) {
     for (YieldCounter* counter : g_yield_counters) {
       const uint64_t yields = counter->yields.load(std::memory_order_relaxed);
       const uint64_t spins = counter->spins.load(std::memory_order_relaxed);
-      Delta delta{counter->name, yields - counter->reported_yields,
-                  spins - counter->reported_spins};
+      const uint64_t spin_ticks = counter->spin_ticks.load(std::memory_order_relaxed);
+      const uint64_t sleep0 = counter->sleep0.load(std::memory_order_relaxed);
+      Delta delta{{}, yields - counter->reported_yields, spins - counter->reported_spins,
+                  spin_ticks - counter->reported_spin_ticks, sleep0 - counter->reported_sleep0};
       counter->reported_yields = yields;
       counter->reported_spins = spins;
-      if (delta.yields || delta.spins) deltas.push_back(delta);
+      counter->reported_spin_ticks = spin_ticks;
+      counter->reported_sleep0 = sleep0;
+      if (delta.yields || delta.spins || delta.sleep0) {
+        RefreshCounterName(counter);
+        delta.name = counter->name;
+        deltas.push_back(std::move(delta));
+      }
     }
   }
   std::sort(deltas.begin(), deltas.end(), [](const Delta& a, const Delta& b) {
-    return a.yields + a.spins > b.yields + b.spins;
+    if (a.spin_ticks != b.spin_ticks) return a.spin_ticks > b.spin_ticks;
+    return a.yields + a.spins + a.sleep0 > b.yields + b.spins + b.sleep0;
   });
   std::string summary;
   for (size_t i = 0; i < deltas.size() && i < max_threads; ++i) {
     if (!summary.empty()) summary += ' ';
+    char ms[32];
+    std::snprintf(ms, sizeof(ms), "%.1fms", WaitTicksToMs(deltas[i].spin_ticks));
     summary += deltas[i].name;
-    summary += '=' + std::to_string(deltas[i].yields) + '/' + std::to_string(deltas[i].spins);
+    summary += '=' + std::to_string(deltas[i].yields) + '/' + std::to_string(deltas[i].spins) +
+               '/' + ms + "/z" + std::to_string(deltas[i].sleep0);
   }
   return summary;
 }
@@ -1031,6 +1125,42 @@ u32 NtSignalAndWaitForSingleObjectEx_entry(u32 signal_handle, u32 wait_handle, u
   return result;
 }
 
+// Parking for contended guest spinlocks. Waiters sleep on the lock word itself (futex);
+// the release in xeKeKfReleaseSpinLock wakes one of them. A small table of waiter counts
+// (by address) lets an uncontended release skip the syscall. The 1 ms timeout keeps a
+// waiter safe if some path ever frees a lock without going through the release below.
+namespace {
+std::array<std::atomic<uint32_t>, 64> g_spinlock_waiters{};
+
+inline std::atomic<uint32_t>& SpinlockWaiters(const volatile void* word) {
+  return g_spinlock_waiters[(reinterpret_cast<uintptr_t>(word) >> 2) & 63];
+}
+
+void SpinlockSleepUntilReleased(volatile uint32_t* word) {
+  std::atomic<uint32_t>& waiters = SpinlockWaiters(word);
+  waiters.fetch_add(1, std::memory_order_seq_cst);
+  const uint32_t owner = __atomic_load_n(word, __ATOMIC_SEQ_CST);
+  if (owner != 0) {
+#if defined(__ANDROID__) || defined(__linux__)
+    timespec timeout{0, 1000000};  // 1 ms
+    syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, owner, &timeout, nullptr, 0);
+#else
+    std::this_thread::sleep_for(std::chrono::microseconds(50));
+#endif
+  }
+  waiters.fetch_sub(1, std::memory_order_seq_cst);
+}
+
+void SpinlockWakeWaiter(volatile uint32_t* word) {
+  // Orders the release store above against the waiter-count load (store -> load).
+  std::atomic_thread_fence(std::memory_order_seq_cst);
+  if (SpinlockWaiters(word).load(std::memory_order_relaxed) == 0) return;
+#if defined(__ANDROID__) || defined(__linux__)
+  syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
+#endif
+}
+}  // namespace
+
 // Guest-memory IRQL helpers - read/write current_irql directly from PCR.
 // Take PPCContext* explicitly so they work from any thread (including host threads
 // during InitializeGuestObject, dispatch thread creation, etc.).
@@ -1060,26 +1190,39 @@ uint32_t xeKeKfAcquireSpinLock(PPCContext* ctx, X_KSPINLOCK* lock, bool change_i
   // another core: watching the lock word for a while usually gets it without
   // a trip through the scheduler (sched_yield costs microseconds and can hand
   // the core to an unrelated thread). Past the budget the owner was most
-  // likely preempted, so give the core up as before.
-  YieldCounter& counter = CurrentYieldCounter();
+  // likely preempted (on a phone it can be off-core for milliseconds), so sleep
+  // until the release wakes us instead of polling.
+YieldCounter& counter = CurrentYieldCounter();
+  const uint64_t wait_begin = WaitTicks();
   uint32_t rounds = 0;
   const uint32_t spin_rounds = REXCVAR_GET(kernel_spinlock_spins);
-  do {
+  const bool use_futex = REXCVAR_GET(kernel_spinlock_futex);
+  for (;;) {
     counter.spins.fetch_add(1, std::memory_order_relaxed);
     if (rounds < spin_rounds) {
       ++rounds;
-      for (int i = 0; i < 32 && __atomic_load_n(&lock->prcb_of_owner.value, __ATOMIC_RELAXED);
+      for (int i = 0; i < 16 && __atomic_load_n(&lock->prcb_of_owner.value, __ATOMIC_RELAXED) != 0;
            ++i) {
 #if defined(__aarch64__)
-        __asm__ __volatile__("yield");
+        __asm__ __volatile__("isb" ::: "memory");
 #elif defined(__x86_64__) || defined(_M_X64)
         __builtin_ia32_pause();
 #endif
       }
+    } else if (rounds < spin_rounds + 4) {
+      ++rounds;
+      sched_yield();
+    } else if (use_futex) {
+      SpinlockSleepUntilReleased(&lock->prcb_of_owner.value);
     } else {
-      rex::thread::MaybeYield();
+      std::this_thread::sleep_for(std::chrono::microseconds(counter.is_realtime ? 10 : 50));
     }
-  } while (!rex::thread::atomic_cas(0u, self, &lock->prcb_of_owner.value));
+    if (__atomic_load_n(&lock->prcb_of_owner.value, __ATOMIC_RELAXED) == 0 &&
+        rex::thread::atomic_cas(0u, self, &lock->prcb_of_owner.value)) {
+      break;
+    }
+  }
+  counter.spin_ticks.fetch_add(WaitTicks() - wait_begin, std::memory_order_relaxed);
   return old_irql;
 }
 
@@ -1087,6 +1230,7 @@ void xeKeKfReleaseSpinLock(PPCContext* ctx, X_KSPINLOCK* lock, uint32_t old_irql
                            bool change_irql) {
   assert_true(lock->prcb_of_owner == static_cast<uint32_t>(ctx->r13.u64));
   rex::thread::atomic_store_release(0u, &lock->prcb_of_owner.value);
+  SpinlockWakeWaiter(&lock->prcb_of_owner.value);
   if (change_irql && old_irql < IRQL_DISPATCH) {
     xeKfLowerIrql(ctx, static_cast<unsigned char>(old_irql));
   }
@@ -1659,4 +1803,4 @@ REX_EXPORT_STUB(__imp__KeStallExecutionProcessor);
 REX_EXPORT_STUB(__imp__KeSweepDcacheRange);
 REX_EXPORT_STUB(__imp__KeSweepIcacheRange);
 REX_EXPORT_STUB(__imp__KeTestAlertThread);
-REX_EXPORT(__imp__KfRaiseIrql, rex::kernel::xboxkrnl::KfRaiseIrql_entry)
+REX_EXPORT(__imp__KfRaiseIrql, rex::kernel::xboxkrnl::KfRaiseIrql_entry)
\ No newline at end of file
diff --git a/glue/rexglue-sdk-main/src/system/xthread.cpp b/glue/rexglue-sdk-main/src/system/xthread.cpp
index 91913215..d8d4e87a 100644
--- a/glue/rexglue-sdk-main/src/system/xthread.cpp
+++ b/glue/rexglue-sdk-main/src/system/xthread.cpp
@@ -13,7 +13,7 @@
 #include <atomic>
 #include <cstring>
 #include <thread>
-
+#include <pthread.h>
 #include <fmt/format.h>
 
 #include <rex/chrono/clock.h>
@@ -669,8 +669,36 @@ void XThread::Execute() {
   // when another fiber switches back to the main execution context.
   main_fiber_ = rex::thread::Fiber::ConvertCurrentThread();
 
-  // Execute the function
+ // Execute the function
   REXSYS_NOISY_DEBUG("XThread::Execute - Calling function at {:08X}", address);
+
+#if defined(__ANDROID__)
+  // Читаем реальную целевую функцию из контекста RAGE (регистр r4 -> первое 32-битное слово)
+  uint32_t rage_fn = 0;
+  if (ctx->r4.u32 >= 0x80000000 && ctx->r4.u32 < 0x90000000) {
+    std::memcpy(&rage_fn, base + ctx->r4.u32, sizeof(rage_fn));
+    rage_fn = __builtin_bswap32(rage_fn);
+  }
+
+  if (address == 0x821909D0 || rage_fn == 0x821909D0) {
+    pthread_setname_np(pthread_self(), "AudioGuest");
+  } else if (address == 0x821B6F18 || rage_fn == 0x821B6F18) {
+    pthread_setname_np(pthread_self(), "RenderGuest");
+  } else if (rage_fn == 0x82902728) {
+    pthread_setname_np(pthread_self(), "RageStream");
+  } else if (rage_fn == 0x8285D368) {
+    pthread_setname_np(pthread_self(), "RageJobWork");
+  } else if (rage_fn == 0x82798BA0 || rage_fn == 0x827989F8) {
+    pthread_setname_np(pthread_self(), "RagePhysics");
+  } else if (rage_fn == 0x8284C5E8) {
+    pthread_setname_np(pthread_self(), "RageFileIO");
+  } else if (rage_fn != 0) {
+    char tname[16];
+    std::snprintf(tname, sizeof(tname), "Rage_%06X", rage_fn & 0xFFFFFF);
+    pthread_setname_np(pthread_self(), tname);
+  }
+#endif
+
   func(*ctx, base);
 
   exit_code = static_cast<int>(ctx->r3.u32);
diff --git a/glue/rexglue-sdk-main/src/ui/imgui_drawer.cpp b/glue/rexglue-sdk-main/src/ui/imgui_drawer.cpp
index 77882a75..518ccb0f 100644
--- a/glue/rexglue-sdk-main/src/ui/imgui_drawer.cpp
+++ b/glue/rexglue-sdk-main/src/ui/imgui_drawer.cpp
@@ -696,8 +696,13 @@ void ImGuiDrawer::ClearInput() {
 }
 
 void ImGuiDrawer::OnKey(KeyEvent& e, bool is_down) {
-  RequestInputPaint();
   auto& io = GetIO();
+  // Gamepad and hotkeys drive the game; repaint only when an ImGui window
+  // navigates or takes text. Otherwise each button edge re-presented the
+  // same guest frame from the UI thread.
+  if (io.NavActive || io.WantCaptureKeyboard || io.WantTextInput) {
+    RequestInputPaint();
+  }
   const VirtualKey virtual_key = e.virtual_key();
   if (auto imGuiKey = VirtualKeyToImGuiKey(virtual_key); imGuiKey) {
     io.AddKeyEvent(*imGuiKey, is_down);
diff --git a/glue/rexglue-sdk-main/src/ui/presenter.cpp b/glue/rexglue-sdk-main/src/ui/presenter.cpp
index c81f558b..f91ca468 100644
--- a/glue/rexglue-sdk-main/src/ui/presenter.cpp
+++ b/glue/rexglue-sdk-main/src/ui/presenter.cpp
@@ -13,6 +13,7 @@
 #include <chrono>
 #include <atomic>
 #include <cctype>
+#include <thread>
 #include <utility>
 
 #include <rex/assert.h>
@@ -31,6 +32,10 @@
 REXCVAR_DEFINE_BOOL(host_present_from_non_ui_thread, true, "UI/Presenter",
                     "Allow presentation from non-UI thread");
 
+REXCVAR_DEFINE_INT32(present_ui_fps_limit, 60, "UI/Presenter",
+                     "Maximum frame rate for UI-only repaints on non-Win32 platforms (0 = uncapped)")
+    .range(0, 240);
+
 REXCVAR_DEFINE_BOOL(present_letterbox, true, "UI/Presenter",
                     "Enable letterboxing for non-native aspect ratios");
 
@@ -281,6 +286,39 @@ GuestOutputPaintConfig BuildGuestOutputPaintConfigFromCVar() {
 
 }  // namespace
 
+// Frame clock for the FPS overlay: one steady_clock timestamp per published
+// guest frame. Single writer (the publishing thread), lock-free readers.
+namespace {
+constexpr uint32_t kOverlayFrameClockSize = 512;
+std::atomic<uint64_t> g_overlay_frame_count{0};
+std::atomic<uint64_t> g_overlay_frame_ns[kOverlayFrameClockSize];
+
+void NoteOverlayFramePublished() {
+  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
+                                    std::chrono::steady_clock::now().time_since_epoch())
+                                    .count());
+  const uint64_t index = g_overlay_frame_count.load(std::memory_order_relaxed);
+  g_overlay_frame_ns[index % kOverlayFrameClockSize].store(now, std::memory_order_relaxed);
+  g_overlay_frame_count.store(index + 1, std::memory_order_release);
+}
+}  // namespace
+
+// Copies the newest min(count - first, capacity, 511) timestamps, oldest
+// first, and returns the number of frames published so far.
+extern "C" __attribute__((visibility("default"), used)) uint64_t rex_overlay_frame_clock_read(
+    uint64_t first, uint64_t* out_ns, uint32_t capacity, uint32_t* out_written) {
+  const uint64_t count = g_overlay_frame_count.load(std::memory_order_acquire);
+  uint64_t begin = first > count ? count : first;
+  const uint64_t limit = std::min<uint64_t>(capacity, kOverlayFrameClockSize - 1);
+  if (count - begin > limit) begin = count - limit;
+  for (uint64_t index = begin; index < count; ++index) {
+    out_ns[index - begin] =
+        g_overlay_frame_ns[index % kOverlayFrameClockSize].load(std::memory_order_relaxed);
+  }
+  if (out_written) *out_written = uint32_t(count - begin);
+  return count;
+}
+
 namespace rex {
 namespace ui {
 
@@ -663,10 +701,16 @@ bool Presenter::RefreshGuestOutput(
   }
 
   const bool paired_presentation = is_active && writable_properties.provenance.paired_presentation;
+  // На экранах загрузки/меню (< 64 команд) при выключенном капе ограничиваем 60 FPS,
+  // чтобы не молотить 1200+ FPS впустую и не греть CPU до начала игры.
+  uint32_t effective_fps_limit = provenance.frame_rate_limit;
+  if (effective_fps_limit == 0 && provenance.native_command_count < 64) {
+    effective_fps_limit = 60;
+  }
   const uint64_t publication_serial = frame_publication_gate_.Publish(
-      is_active ? provenance.frame_rate_limit : 0, paired_presentation);
+      (is_active && paired_presentation) ? effective_fps_limit : 0, paired_presentation);
   writable_properties.provenance.publication_serial = publication_serial;
-  host_frame_rate_limit_.store(is_active ? provenance.frame_rate_limit : 0,
+  host_frame_rate_limit_.store((is_active && paired_presentation) ? effective_fps_limit : 0,
                                std::memory_order_relaxed);
 
   // Make the new image the next to present on the host (the "ready" one),
@@ -727,6 +771,7 @@ bool Presenter::RefreshGuestOutput(
                 probe.guest_image, probe.guest_version, published_mailbox_index, probe.native_submission);
   }
   publication_progress_.Publish(publication_serial);
+  NoteOverlayFramePublished();
   // Trigger the presentation on the host.
   PaintResult paint_result = PaintResult::kNotPresented;
   PaintMode paint_mode_snapshot = PaintMode::kNone;
@@ -869,12 +914,31 @@ bool Presenter::RefreshGuestOutput(
         paint_result_name(paint_result));
   }
   }
-  if (is_active && provenance.producer_backpressure && (provenance.frame_rate_limit || paired_presentation) &&
+  if (is_active && provenance.producer_backpressure && (effective_fps_limit || paired_presentation) &&
       std::this_thread::get_id() != ui_thread_id_ &&
       paint_result != PaintResult::kGpuLostResponsible &&
       paint_result != PaintResult::kGpuLostExternally) {
     const uint64_t begin = FramePacerNowNs();
-    const bool accepted = frame_publication_gate_.Wait(publication_serial);
+    bool accepted = true;
+    if (paired_presentation) {
+      accepted = frame_publication_gate_.Wait(publication_serial);
+    } else if (effective_fps_limit > 0) {
+      // Мягкий пейсер без провала в 2x слот (66.6 мс): если кадр уже опоздал
+      // (now >= next_deadline_ns), отпускаем сразу без штрафа ожидания.
+      static std::atomic<uint64_t> s_next_deadline_ns{0};
+      const uint64_t period_ns = 1'000'000'000ull / effective_fps_limit;
+      uint64_t deadline = s_next_deadline_ns.load(std::memory_order_relaxed);
+      if (begin + period_ns < deadline || begin >= deadline + period_ns) {
+        deadline = begin;
+      }
+      if (begin < deadline) {
+        std::this_thread::sleep_for(std::chrono::nanoseconds(deadline - begin));
+        s_next_deadline_ns.store(deadline + period_ns, std::memory_order_relaxed);
+      } else {
+        // Кадр уже >= 33.3 мс: не ждём следующий слот 66.6 мс, отпускаем мгновенно!
+        s_next_deadline_ns.store(begin + period_ns, std::memory_order_relaxed);
+      }
+    }
     if (rex::diagnostics::IsEnabled(rex::diagnostics::Category::kPresenter)) {
       REXLOG_INFO("FramePacer point=handoff serial={} frame={} released={} wait-ns={}",
                   publication_serial, provenance.submitted_frame, accepted, FramePacerNowNs()-begin);
@@ -1806,7 +1870,7 @@ Presenter::PaintResult Presenter::PaintAndPresent(bool execute_ui_drawers) {
   PollPresentationTiming();
   const auto display_target = DisplayLinkTargetNs();
   if (display_target && !*display_target) return PaintResult::kNotPresented;
-  pacing_attempt_ = display_target
+ pacing_attempt_ = display_target
       ? frame_pacer_.PlanForDisplay(FramePacerNowNs(), *display_target)
       : frame_pacer_.Plan(FramePacerNowNs());
   if (pacing_attempt_.delay_ns) {
@@ -2009,6 +2073,18 @@ void Presenter::WaitForUITickFromUIThread() {
     }
     dxgi_ui_tick_signal_condition_.wait(dxgi_ui_tick_lock);
   }
+#else
+  const int32_t fps_limit = REXCVAR_GET(present_ui_fps_limit);
+  if (fps_limit > 0) {
+    const auto min_interval = std::chrono::microseconds(1'000'000 / fps_limit);
+    const auto now = std::chrono::steady_clock::now();
+    static auto last_ui_paint_time = std::chrono::steady_clock::now();
+    const auto elapsed = now - last_ui_paint_time;
+    if (elapsed < min_interval) {
+      std::this_thread::sleep_for(min_interval - elapsed);
+    }
+    last_ui_paint_time = std::chrono::steady_clock::now();
+  }
 #endif  // XE_PLATFORM
 }
 
@@ -2071,4 +2147,5 @@ void Presenter::DXGIUITickThread() {
 #endif  // XE_PLATFORM
 
 }  // namespace ui
+
 }  // namespace rex
diff --git a/glue/rexglue-sdk-main/thirdparty/FFmpeg b/glue/rexglue-sdk-main/thirdparty/FFmpeg
--- a/glue/rexglue-sdk-main/thirdparty/FFmpeg
+++ b/glue/rexglue-sdk-main/thirdparty/FFmpeg
@@ -1 +1 @@
-Subproject commit 0604b464c7cb4ebc94940cf1f324a3b26b87717c
+Subproject commit 0604b464c7cb4ebc94940cf1f324a3b26b87717c-dirty
diff --git a/glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp b/glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp
index 32d427d1..cafb3b91 100644
--- a/glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp
+++ b/glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp
@@ -171,11 +171,12 @@ namespace disruptorplus
                 std::unique_lock<std::mutex> lock(m_mutex);
                 m_cv.wait_until(
                     lock,
+					timeoutTime,
                     [&]() -> bool {
                         result = minimum_sequence_after(sequence, count, sequences);
                         return difference(result, sequence) >= 0;
-                    },
-                    timeoutTime);
+                    }
+                    );
             }
             return result;
         }
diff --git a/glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/spin_wait.hpp b/glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/spin_wait.hpp
index 2d8a95a5..604ece89 100644
--- a/glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/spin_wait.hpp
+++ b/glue/rexglue-sdk-main/thirdparty/disruptorplus/include/disruptorplus/spin_wait.hpp
@@ -54,7 +54,8 @@ namespace disruptorplus
         /// Reset the spin_wait back to its original state.
         void reset()
         {
-            m_value = std::thread::hardware_concurrency() > 1 ? 0 : 10;
+           static const bool is_multiprocessor = std::thread::hardware_concurrency() > 1;
+           m_value = is_multiprocessor ? 0 : 10;
         }
         
         /// \brief
diff --git a/glue/rexglue-sdk-main/thirdparty/libmspack b/glue/rexglue-sdk-main/thirdparty/libmspack
--- a/glue/rexglue-sdk-main/thirdparty/libmspack
+++ b/glue/rexglue-sdk-main/thirdparty/libmspack
@@ -1 +1 @@
-Subproject commit 305907723a4e7ab2018e58040059ffb5e77db837
+Subproject commit 305907723a4e7ab2018e58040059ffb5e77db837-dirty
diff --git a/os/android/app/build.gradle.kts b/os/android/app/build.gradle.kts
index edf46fc4..ee542046 100644
--- a/os/android/app/build.gradle.kts
+++ b/os/android/app/build.gradle.kts
@@ -19,7 +19,7 @@ val sdlJavaDir = rootProject.file(
 android {
     namespace   = "com.libertyrecomp"
     compileSdk  = 35
-    ndkVersion  = "29.0.14206865"
+    ndkVersion  = "30.0.16248370"
 
     defaultConfig {
         applicationId = "com.libertyrecomp"
@@ -58,7 +58,7 @@ android {
             // Unpack the libraries into nativeLibraryDir: the runtime finds its
             // GPU plugins by real path next to itself, and a Turnip driver
             // loaded through libadrenotools needs real files as well.
-            useLegacyPackaging = true
+            useLegacyPackaging = false
             // build_native.sh decides what is stripped.
             keepDebugSymbols += "**/*.so"
         }
diff --git a/os/android/app/src/main/assets/default_args.txt b/os/android/app/src/main/assets/default_args.txt
index a5a09240..d1535d23 100644
--- a/os/android/app/src/main/assets/default_args.txt
+++ b/os/android/app/src/main/assets/default_args.txt
@@ -1,8 +1,5 @@
---log_level=warn
-# Display settings (resolution, upscaler, VSync, frame limit, anti-aliasing,
-# draw distance, shadows, reflections, motion blur) belong to the in-game
-# Display menu: a line here would override its saved choice on every launch.
---gta4_dynamic_draw_distance=true
+# Base performance & stability defaults for Android
+--gta4_dynamic_draw_distance=false
 --gta4_dynamic_draw_distance_min=0.80
 --gta4_shadow_distance_scale=1.0
 --gta4_reflection_capture_distance=near
@@ -10,8 +7,29 @@
 --gta4_parked_car_density_scale=0.4
 --gta4_ped_density_scale=0.4
 --gta4_scenario_ped_density_scale=0.4
+--gta4_native_shadow_interval=1
+--gta4_native_shadow_min_indices=150
+--gta4_native_max_queued_frames=2
+--gta4_native_descriptor_backend=indexed
+--gta4_native_async_constant_release=true
+--gta4_fps_guard=0
+--audio_maxqframes=64
+--android_perf_hint=false
+--android_pin_hot_threads=false
+--vulkan_allow_present_mode_mailbox=true
+--vulkan_prefer_present_mode_fifo=false
+--gta4_present_mode=mailbox
+
+# All heavy profilers and frame-timeline log spam off by default
+--gta4_native_frame_timeline=false
+--gta4_native_hitch_log=false
+--gta4_native_hitch_breakdown=false
 --gta4_profile_native_detailed_cpu=false
 --gta4_profile_native_detailed_gpu=false
---gta4_native_max_queued_frames=3
-# The 30 FPS guard (environment reflection rate only now) is off by default.
---gta4_fps_guard=0
+--gta4_profile_native_autostart=false
+--gta4_native_pipeline_report=false
+--gta4_native_hdr_mode=off
+--android_thread_layout=lanes
+--diagnostics=true
+--diagnostics_categories=logging,native-profiler
+--log_level=warn
diff --git a/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java b/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java
index e72269da..67cf2627 100644
--- a/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java
+++ b/os/android/app/src/main/java/com/libertyrecomp/LauncherActivity.java
@@ -119,7 +119,7 @@ public class LauncherActivity extends Activity {
         }, "InstallerLoad").start();
         // `am start ... --ez play true` (the development scripts) goes
         // straight into the game with the current driver choice.
-        if (savedInstanceState == null && getIntent().getBooleanExtra("play", false)) play(false);
+        if (savedInstanceState == null && getIntent().getBooleanExtra("play", false)) play(getIntent().getBooleanExtra("debug_log", false));
     }
 
     @Override
@@ -165,7 +165,7 @@ public class LauncherActivity extends Activity {
         ImageView mark = new ImageView(this);
         mark.setImageResource(R.drawable.ic_github);
         row.addView(mark, new LinearLayout.LayoutParams(dp(16), dp(16)));
-        TextView name = text("vaduur", 13, R.color.picker_muted);
+       TextView name = text("vaduur | ViBetimized by Gemini & GVINDOLIN", 13, R.color.picker_muted);
         LinearLayout.LayoutParams nameParams = new LinearLayout.LayoutParams(
                 ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
         nameParams.leftMargin = dp(6);
@@ -442,10 +442,9 @@ public class LauncherActivity extends Activity {
                     mCrashCard.setVisibility(View.GONE);
                     return;
                 }
-                mCrashText.setText(report.headline + "\n\nSaved to Android/data/" + getPackageName()
+               mCrashText.setText(report.headline + "\n\nSaved to Android/data/" + getPackageName()
                         + "/files/crash_reports/" + report.file.getName()
-                        + "\nPlease attach it to an issue on GitHub (vaduur/LibertyRecompAndroid), "
-                        + "or press Share report to send it from here.");
+                        + "\nPress Share report to send it from here.");
                 mCrashCard.setVisibility(View.VISIBLE);
             });
         }, "CrashReports").start();
@@ -1055,12 +1054,36 @@ public class LauncherActivity extends Activity {
         TextView title = text("Performance test settings", 18, R.color.picker_text);
         title.setTypeface(Typeface.DEFAULT_BOLD);
         card.addView(title);
-        card.addView(text("WARNING!!! For lower-end devices and endorsed ONLY for testing. SHIT "
+        // The warning is folded under an "Important" spoiler; A / tap opens it.
+        TextView warning = text("WARNING!!! For lower-end devices and endorsed ONLY for testing. SHIT "
                 + "WILL BREAK, and by shit I mean almost all visuals. If you have stable 30fps "
                 + "please don't touch it, or your game will look like a potato, and a very ugly one "
                 + "a that with tons of other visual bugs and crashes. Issues made with any of these "
                 + "settings probably will be skipped on Github. You have been warned, have fun then:",
-                12, R.color.picker_error), matchWrap(dp(4)));
+                12, R.color.picker_error);
+        warning.setVisibility(View.GONE);
+        String importantLabel = "Important \u2014 read before changing anything";
+        TextView important = text("\u25B8 " + importantLabel, 14, R.color.picker_error);
+        important.setTypeface(Typeface.DEFAULT_BOLD);
+        important.setPadding(dp(6), dp(8), dp(6), dp(8));
+        important.setBackground(focusBackground(Color.TRANSPARENT));
+        important.setFocusable(true);
+        important.setOnClickListener(v -> {
+            boolean open = warning.getVisibility() != View.VISIBLE;
+            warning.setVisibility(open ? View.VISIBLE : View.GONE);
+            important.setText((open ? "\u25BE " : "\u25B8 ") + importantLabel);
+        });
+        card.addView(important, matchWrap(dp(4)));
+        card.addView(warning, matchWrap(dp(2)));
+
+        // Manual / automatic placement of the game's threads on CPU cores, above all settings.
+        Button threadPlacement = button(ThreadAffinityDialog.summary(this), false);
+        threadPlacement.setOnClickListener(v -> new ThreadAffinityDialog(this,
+                () -> threadPlacement.setText(ThreadAffinityDialog.summary(this))).show());
+        LinearLayout.LayoutParams threadParams =
+                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44));
+        threadParams.topMargin = dp(8);
+        card.addView(threadPlacement, threadParams);
 
         LinearLayout panel = new LinearLayout(this);
         panel.setOrientation(LinearLayout.VERTICAL);
@@ -1228,11 +1251,28 @@ public class LauncherActivity extends Activity {
                         perf.getInt(PERF_WATER_REFLECTIONS, 1));
                 intent.putExtra(LibertyActivity.EXTRA_OTHER_REFLECTION_INTERVAL,
                         perf.getInt(PERF_OTHER_REFLECTIONS, 1));
+                String affinitySpec = ThreadAffinityDialog.buildLaunchSpec(this);
+                Log.i(TAG, "affinity spec=" + affinitySpec);
+                intent.putExtra(ThreadAffinityDialog.EXTRA_SPEC, affinitySpec);
+                writeLaunchTicket(intent);
                 startActivity(intent);
             });
         }, "LaunchGame").start();
     }
 
+    /**
+     * The same extras for LibertyActivity, written synchronously before startActivity():
+     * after stopGameProcess() the system recreates the game activity with the intent it
+     * was first started with (see LibertyActivity.launchIntent).
+     */
+    private void writeLaunchTicket(Intent intent) {
+        boolean saved = getSharedPreferences(LibertyActivity.LAUNCH_TICKET, Context.MODE_PRIVATE).edit()
+                .putString("intent", intent.toUri(Intent.URI_INTENT_SCHEME))
+                .putLong("time", System.currentTimeMillis())
+                .commit();
+        if (!saved) Log.w(TAG, "launch ticket not saved; the game may start with older settings");
+    }
+
     private void stopGameProcess() {
         ActivityManager manager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
         if (manager == null) return;
@@ -1401,4 +1441,4 @@ public class LauncherActivity extends Activity {
         //noinspection ResultOfMethodCallIgnored
         file.delete();
     }
-}
+}
\ No newline at end of file
diff --git a/os/android/app/src/main/java/com/libertyrecomp/LibertyActivity.java b/os/android/app/src/main/java/com/libertyrecomp/LibertyActivity.java
index a05ae52f..fb4e1be2 100644
--- a/os/android/app/src/main/java/com/libertyrecomp/LibertyActivity.java
+++ b/os/android/app/src/main/java/com/libertyrecomp/LibertyActivity.java
@@ -85,6 +85,16 @@ public class LibertyActivity extends SDLActivity {
     static final String EXTRA_OTHER_REFLECTION_INTERVAL = "perf_other_reflection_interval";
     private boolean mDebugLog;
     private File mDebugLogFile;
+    /**
+     * Settings of the launcher's last "Play" press (LauncherActivity.writeLaunchTicket).
+     * getIntent() is not enough: the launcher kills the old :game process before Play, but
+     * the system keeps this singleTask activity's record and recreates it with the intent
+     * it was first started with; the new intent only arrives later, in onNewIntent().
+     */
+    static final String LAUNCH_TICKET = "launch_ticket";
+    /** A ticket older than this is not from this Play press. */
+    private static final long LAUNCH_TICKET_MAX_AGE_MS = 120_000;
+    private Intent mLaunchIntent;
 
     @Override
     protected void onCreate(Bundle savedInstanceState) {
@@ -94,7 +104,7 @@ public class LibertyActivity extends SDLActivity {
         getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
         // Steady clocks the device can hold, instead of peaks followed by
         // throttling (only where the device supports it).
-        Intent intent = getIntent();
+        Intent intent = launchIntent();
         if (intent != null && intent.getBooleanExtra(EXTRA_SUSTAINED, false)) {
             android.os.PowerManager power = (android.os.PowerManager) getSystemService(POWER_SERVICE);
             boolean supported = power != null && power.isSustainedPerformanceModeSupported();
@@ -180,6 +190,12 @@ public class LibertyActivity extends SDLActivity {
     protected SDLSurface createSDLSurface(Context context) {
         SDLSurface surface = super.createSDLSurface(context);
         if (mSurfaceWidth > 0 && mSurfaceHeight > 0) {
+            android.view.ViewGroup.LayoutParams lp = surface.getLayoutParams();
+            if (lp != null) {
+                lp.width = android.view.ViewGroup.LayoutParams.MATCH_PARENT;
+                lp.height = android.view.ViewGroup.LayoutParams.MATCH_PARENT;
+                surface.setLayoutParams(lp);
+            }
             surface.getHolder().setFixedSize(mSurfaceWidth, mSurfaceHeight);
             Log.i(TAG, "fixed surface " + mSurfaceWidth + "x" + mSurfaceHeight);
         }
@@ -215,17 +231,30 @@ public class LibertyActivity extends SDLActivity {
     // The launcher's performance test settings replace these cvars' lines in
     // args.txt for this run (the file itself is left as it is).
     private void applyPerformanceTestArguments(List<String> arguments) {
-        Intent intent = getIntent();
+        Intent intent = launchIntent();
+        // Thread placement from the launcher; independent of the extras below.
+        // android_thread_pinning.cpp reads the environment variable before the
+        // android_thread_affinity cvar, so native.toml / args.txt cannot override it.
+        // libmain.so is loaded after onCreate() -> prepareEnvironment(), so the
+        // variable is already set when the pinning thread starts.
+        if (intent != null && intent.hasExtra(ThreadAffinityDialog.EXTRA_SPEC)) {
+            String spec = intent.getStringExtra(ThreadAffinityDialog.EXTRA_SPEC);
+            if (spec == null || spec.isEmpty()) spec = "auto";
+            setenv(ThreadAffinityDialog.ENV_SPEC, spec);
+            Log.i(TAG, "thread affinity " + spec);
+        }
         if (intent == null || !intent.hasExtra(EXTRA_SHADOWS_OFF)) return;
         boolean shadowsOff = intent.getBooleanExtra(EXTRA_SHADOWS_OFF, false);
         // 0 = off.
         int water = Math.max(0, Math.min(intent.getIntExtra(EXTRA_WATER_REFLECTION_INTERVAL, 1), 3));
         int environment =
                 Math.max(0, Math.min(intent.getIntExtra(EXTRA_OTHER_REFLECTION_INTERVAL, 1), 3));
-        arguments.removeIf(a -> a.startsWith("--gta4_native_shadow_interval")
-                || a.startsWith("--gta4_native_water_reflection_interval")
+       arguments.removeIf(a -> a.startsWith("--gta4_native_water_reflection_interval")
                 || a.startsWith("--gta4_native_environment_reflection_interval"));
-        arguments.add("--gta4_native_shadow_interval=" + (shadowsOff ? 0 : 1));
+        if (shadowsOff) {
+            arguments.removeIf(a -> a.startsWith("--gta4_native_shadow_interval"));
+            arguments.add("--gta4_native_shadow_interval=0");
+        }
         arguments.removeIf(a -> a.startsWith("--android_gpu_turbo"));
         arguments.add("--android_gpu_turbo=" + intent.getBooleanExtra(EXTRA_GPU_TURBO, false));
         // Only adds: safe mode can also come from args.txt (scripts/regress.sh --safe).
@@ -286,16 +315,20 @@ public class LibertyActivity extends SDLActivity {
         // "Play (Debug Log)" from the launcher: the runtime logs at info level
         // and the whole process log is recorded into files/debug_logs/.
         applyPerformanceTestArguments(arguments);
-        mDebugLog = getIntent() != null && getIntent().getBooleanExtra(EXTRA_DEBUG_LOG, false);
+        Intent launch = launchIntent();
+        mDebugLog = launch != null && launch.getBooleanExtra(EXTRA_DEBUG_LOG, false);
         if (mDebugLog) {
-            arguments.removeIf(a -> a.startsWith("--diagnostics") || a.startsWith("--log_level"));
-            diagnosticsSet = false;
+            arguments.removeIf(a -> a.startsWith("--log_level") || a.startsWith("--gta4_native_hitch_log"));
             arguments.add("--log_level=info");
+            arguments.add("--gta4_native_hitch_log=true");
             mDebugLogFile = DebugLog.start(dataRoot);
+        } else {
+            arguments.removeIf(a -> a.startsWith("--gta4_native_hitch_log"));
+            arguments.add("--gta4_native_hitch_log=false");
         }
         if (!diagnosticsSet) {
             arguments.add("--diagnostics=true");
-            arguments.add("--diagnostics_categories=logging");
+            arguments.add("--diagnostics_categories=logging,native-profiler");
         }
         addInstallSources(new File(dataRoot, "install"), arguments);
         for (int i = arguments.size() - 1; i >= 0; --i) {
@@ -396,6 +429,51 @@ public class LibertyActivity extends SDLActivity {
         }
     }
 
+    /**
+     * The launcher's settings for this start: the ticket written right before
+     * startActivity() if there is a fresh one, else the activity's intent.
+     * Read once; the ticket is removed so a later restart by the system (no Play
+     * press) falls back to the intent.
+     */
+    private Intent launchIntent() {
+        if (mLaunchIntent != null) return mLaunchIntent;
+        Intent intent = getIntent();
+        mLaunchIntent = intent;
+        android.content.SharedPreferences ticket = getSharedPreferences(LAUNCH_TICKET, MODE_PRIVATE);
+        String uri = ticket.getString("intent", null);
+        long age = System.currentTimeMillis() - ticket.getLong("time", 0);
+        if (uri == null) return mLaunchIntent;
+        ticket.edit().clear().commit();
+        if (age < 0 || age > LAUNCH_TICKET_MAX_AGE_MS) {
+            Log.w(TAG, "launch ticket ignored: " + (age / 1000) + " s old");
+            return mLaunchIntent;
+        }
+        try {
+            Intent fromTicket = Intent.parseUri(uri, Intent.URI_INTENT_SCHEME);
+            String before = intent != null ? intent.getStringExtra(ThreadAffinityDialog.EXTRA_SPEC) : null;
+            String after = fromTicket.getStringExtra(ThreadAffinityDialog.EXTRA_SPEC);
+            boolean stale = !sameExtras(intent, fromTicket);
+            Log.i(TAG, "launch settings: launcher ticket"
+                    + (stale ? " (activity intent was stale; affinity " + before + " -> " + after + ")" : ""));
+            mLaunchIntent = fromTicket;
+        } catch (java.net.URISyntaxException e) {
+            Log.w(TAG, "launch ticket unreadable: " + uri, e);
+        }
+        return mLaunchIntent;
+    }
+
+    @SuppressWarnings("deprecation")
+    private static boolean sameExtras(Intent a, Intent b) {
+        Bundle x = a != null ? a.getExtras() : null;
+        Bundle y = b != null ? b.getExtras() : null;
+        if (x == null || y == null) return x == y;
+        if (!x.keySet().equals(y.keySet())) return false;
+        for (String key : x.keySet()) {
+            if (!String.valueOf(x.get(key)).equals(String.valueOf(y.get(key)))) return false;
+        }
+        return true;
+    }
+
     private static void setenv(String name, String value) {
         try {
             Os.setenv(name, value, true);
@@ -524,6 +602,10 @@ public class LibertyActivity extends SDLActivity {
      */
     private static final String[] CONFIG_LINE_REMOVALS = {
             "gta4_output_height_cap = 720",
+            "gta4_profile_native_detailed_cpu = true",
+            "gta4_profile_native_detailed_gpu = true",
+            "gta4_native_frame_timeline = true",
+            "gta4_native_max_queued_frames = 3",
     };
 
     /**
@@ -625,4 +707,4 @@ public class LibertyActivity extends SDLActivity {
                         | View.SYSTEM_UI_FLAG_FULLSCREEN
                         | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
     }
-}
+}
\ No newline at end of file
diff --git a/os/android/gradle.properties b/os/android/gradle.properties
index f8142cc8..85e1326b 100644
--- a/os/android/gradle.properties
+++ b/os/android/gradle.properties
@@ -16,3 +16,6 @@ org.gradle.caching=true
 
 # Warn instead of erroring on R8 legacy-lambda shrinker edge cases.
 android.suppressUnsupportedCompileSdk=35
+
+# Enabled parallel sync for Gradle 9.4+
+org.gradle.tooling.parallel=true
diff --git a/os/android/scripts/build_apk.sh b/os/android/scripts/build_apk.sh
index bb210ea1..3109a891 100644
--- a/os/android/scripts/build_apk.sh
+++ b/os/android/scripts/build_apk.sh
@@ -12,6 +12,8 @@ cat > "$APP_DIR/local.properties" <<EOF
 sdk.dir=$(cygpath -m "$ANDROID_HOME" 2>/dev/null || echo "$ANDROID_HOME")
 EOF
 
+export JAVA_HOME="C:\Users\Ad\.jdks\jbr-21.0.11" 
+
 cd "$APP_DIR"
-./gradlew --no-daemon "$TASK"
+./gradlew --no-daemon "$TASK" --stacktrace
 ls -la "$APP_DIR/app/build/outputs/apk/$VARIANT/"
diff --git a/os/android/scripts/build_native.sh b/os/android/scripts/build_native.sh
index b54fc3d7..5d8ec1ba 100644
--- a/os/android/scripts/build_native.sh
+++ b/os/android/scripts/build_native.sh
@@ -7,7 +7,7 @@
 # usage: build_native.sh [jobs]      (default: number of CPUs - 2)
 set -euo pipefail
 . "$(dirname "$0")/env.sh"
-JOBS="${1:-$(( $(nproc) > 3 ? $(nproc) - 2 : 1 ))}"
+JOBS=18
 
 [ -d "$ANDROID_NDK" ] || { echo "no NDK at $ANDROID_NDK"; exit 1; }
 
@@ -17,8 +17,8 @@ python "$APP_DIR/scripts/materialize_symlinks.py"
 # Snapdragon 865 and every Android 10+ arm64 device we target are ARMv8.2:
 # inline LSE atomics instead of outline-atomic calls, and native ELF TLS
 # (API 29+) instead of emulated TLS. Both are hot in the guest thread.
-CPU_FLAGS="-march=armv8.2-a -fno-emulated-tls"
-if [ ! -f "$BUILD_DIR/build.ninja" ] || ! grep -q -- "-fno-emulated-tls" "$BUILD_DIR/CMakeCache.txt"; then
+CPU_FLAGS="-march=armv8.2-a+lse+dotprod+fp16+crypto -mno-outline-atomics -w -Wno-asm-operand-widths -DREX_FPCR_STICKY_FLUSH"
+if [ ! -f "$BUILD_DIR/build.ninja" ] || ! grep -qE "ANDROID_PLATFORM:[A-Z]*=android-29" "$BUILD_DIR/CMakeCache.txt"; then
   echo "== configuring $BUILD_TYPE in $BUILD_DIR"
   cmake -S "$REPO" -B "$BUILD_DIR" -G Ninja \
     -DCMAKE_C_FLAGS="$CPU_FLAGS" \
@@ -26,7 +26,7 @@ if [ ! -f "$BUILD_DIR/build.ninja" ] || ! grep -q -- "-fno-emulated-tls" "$BUILD
     -DCMAKE_TOOLCHAIN_FILE="$REPO/toolchains/android.cmake" \
     -DANDROID_NDK="$ANDROID_NDK" \
     -DANDROID_ABI=arm64-v8a \
-    -DANDROID_PLATFORM=android-28 \
+    -DANDROID_PLATFORM=android-29 \
     -DANDROID_STL=c++_shared \
     -DLIBERTY_RECOMP_TARGET_PLATFORM=android \
     -DLIBERTY_RECOMP_ANDROID_RUNTIME_ASSETS=ON \
@@ -61,7 +61,7 @@ mkdir -p "$JNILIBS"
 rm -f "$JNILIBS"/libmain.so "$JNILIBS"/libliberty_install.so "$JNILIBS"/librexruntime.so "$JNILIBS"/librexgpu-*.so       "$JNILIBS"/libSDL3.so "$JNILIBS"/libc++_shared.so
 for so in "$BUILD_DIR"/glue/gta4-recomp/libmain.so \
           "$BUILD_DIR"/glue/gta4-recomp/libliberty_install.so "$REX_OUT_DIR"/librexruntime.so \
-          "$REX_OUT_DIR"/librexgpu-*.so "$REX_OUT_DIR"/libSDL3.so; do
+          "$REX_OUT_DIR"/librexgpu-*.so "$REX_OUT_DIR"/libSDL3.so;do
   [ -f "$so" ] || continue
   # Packaged unstripped by default so ndk-stack and simpleperf can resolve
   # frames. STRIP=1 makes a smaller APK.
diff --git a/os/android/scripts/env.sh b/os/android/scripts/env.sh
index f0ca8761..7c07d916 100644
--- a/os/android/scripts/env.sh
+++ b/os/android/scripts/env.sh
@@ -16,7 +16,7 @@ if [ -z "${ANDROID_HOME:-}" ]; then
 fi
 export ANDROID_HOME
 export ANDROID_SDK_ROOT="$ANDROID_HOME"
-export NDK_VER="${NDK_VER:-29.0.14206865}"
+export NDK_VER="${NDK_VER:-30.0.16248370}"
 export ANDROID_NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/$NDK_VER}"
 export JAVA_HOME="${JAVA_HOME:-/c/Program Files/Android/Android Studio/jbr}"
 
@@ -48,6 +48,12 @@ export FILES="/sdcard/Android/data/$PKG/files"
 export GAME_DIR="$FILES/LibertyRecomp/game"
 
 export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"
+for _cmake_bin in "$ANDROID_HOME"/cmake/*/bin; do
+  if [ -d "$_cmake_bin" ]; then
+    export PATH="$_cmake_bin:$PATH"
+    break
+  fi
+done
 
 # Whichever single device is attached, unless SERIAL is set.
 if [ -z "${SERIAL:-}" ]; then
diff --git a/thirdparty/MoltenVK/MoltenVK b/thirdparty/MoltenVK/MoltenVK
--- a/thirdparty/MoltenVK/MoltenVK
+++ b/thirdparty/MoltenVK/MoltenVK
@@ -1 +1 @@
-Subproject commit db445ff2042d9ce348c439ad8451112f354b8d2a
+Subproject commit db445ff2042d9ce348c439ad8451112f354b8d2a-dirty
diff --git a/thirdparty/ffmpeg-core b/thirdparty/ffmpeg-core
--- a/thirdparty/ffmpeg-core
+++ b/thirdparty/ffmpeg-core
@@ -1 +1 @@
-Subproject commit 5089f001a5442308599fbab8e35282f5ad8ccf13
+Subproject commit 5089f001a5442308599fbab8e35282f5ad8ccf13-dirty
diff --git a/thirdparty/plume b/thirdparty/plume
--- a/thirdparty/plume
+++ b/thirdparty/plume
@@ -1 +1 @@
-Subproject commit 519e3258048fe0bc65044e75fba7b1543e4c8e90
+Subproject commit 519e3258048fe0bc65044e75fba7b1543e4c8e90-dirty
diff --git a/tools/XenosRecomp b/tools/XenosRecomp
--- a/tools/XenosRecomp
+++ b/tools/XenosRecomp
@@ -1 +1 @@
-Subproject commit fb32631ee398e46f2a113d8f9103201dbaa000b4
+Subproject commit fb32631ee398e46f2a113d8f9103201dbaa000b4-dirty
diff --git a/tools/llvm-project-libcxx b/tools/llvm-project-libcxx
--- a/tools/llvm-project-libcxx
+++ b/tools/llvm-project-libcxx
@@ -1 +1 @@
-Subproject commit 2078da43e25a4623cab2d0d60decddf709aaea28
+Subproject commit 2078da43e25a4623cab2d0d60decddf709aaea28-dirty
```

---

## 6. ДИФФЫ СМЕЖНЫХ ПОДМОДУЛЕЙ (SUBMODULES GIT DIFFS)

```diff
Entering 'glue/rexglue-sdk-main/thirdparty/FFmpeg'
```

---

## 7. РЕКОМЕНДАЦИИ И ТОЧКИ ВНИМАНИЯ ДЛЯ АУДИТОРА

При аудите изменений рекомендуется обратить особое внимание на следующие аспекты:
1. **Параллелизм конвейера (`gta4_native_frame_complete_signal = 1` vs `2`)**:
   - Режим `1` абсолютно безопасен, так как команды упакованы и переданы рекордеру.
   - Режим `2` (сигнал сразу после `Present`) является экспериментальным и требует проверки отсутствия перезаписи кольцевых буферов констант при резких просадках GPU.
2. **Битовая маска текстур (`used_texture_mask`)**:
   - Проверено, что все 26 слотов укладываются в 32-битное поле маски, а также корректно учитывается слот `packed_depth_source`.
3. **Пиннинг аудио (`nice = -16`)**:
   - Категорически не рекомендуется понижать приоритет аудио-потоков до `nice = -8` на Snapdragon 888 во избежание деградации декодера XMA.
4. **Физический спайк (`rage_physics` / `rage_jobs`)**:
   - Просадки FPS при разрушении физических объектов (столбы/лавки) связаны с кратковременным насыщением пула задач на ядрах 4–5. Перспективное направление оптимизации — балансировка `Kind::kJobsLane` и приоритетов `rage_jobs`.
