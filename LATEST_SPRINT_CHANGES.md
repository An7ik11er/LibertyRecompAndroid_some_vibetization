# СВОДКА ВСЕХ ИЗМЕНЕНИЙ И ОПТИМИЗАЦИЙ (LATEST CHANGES SUMMARY)
**Проект:** LibertyRecomp (GTA IV Recompiled for Android / ReXGlue)  
**Репозиторий:** `https://github.com/An7ik11er/LibertyRecompAndroid_some_vibetization`  
**Целевой SoC:** Qualcomm Snapdragon 888 (SM8350: 1x Cortex-X1 @ 2.84 GHz, 3x Cortex-A78 @ 2.42 GHz, 4x Cortex-A55 @ 1.80 GHz)  
**GPU / Драйвер:** Adreno 660 / Vulkan Turnip (Mesa 26.3.0-devel / sysmem + autotune)  
**Дата формирования:** 9 ноября 2026 г.  

---

## 1. Решение визуальных багов в режиме S1 (Coroutine Streaming)

### Контекст:
Использовались флаги прямого конвейера:
`--gta4_native_direct_record=true`  
`--gta4_native_stream_record=true`  
`--gta4_native_descriptor_backend=indexed`

### Баг:
Черные силуэты объектов, отсутствие освещения на авто/персонажах, черный экран в меню паузы (карта и вкладки не рендерились), черные стены в квартире Романа.

### Причина:
В методе `StreamOpenFrame()` файла `native_stream_record.inc` отсутствовала синхронизация серийного номера кольцевого буфера:
`recording_ring_serial_` не обновлялся значением `worker_ring_serial_`.
Из-за этого opaque draw-calls отклоняли срезы кольцевого буфера констант как устаревшие и откатывались на неинициализированные снимки кучи с нулевыми регистрами эмбиента и диффузного света.

### Исправление:
- В `StreamOpenFrame()` добавлена явная синхронизация:
  ```cpp
  recording_ring_serial_ = worker_ring_serial_;
  assembly_frame_.reserve(8192);
  ```
- **Результат:** 
  - Квартира Романа: чистая геометрия и полный свет — **64.3 FPS**.
  - Меню паузы и карта: стабильные **65.3 FPS**, корректные шрифты и текстуры.

---

## 2. Разгон производительности на улице (с 10–13 FPS до 20.6 FPS)

### Баг 1: Неправильное распределение ядер (CPU Affinity Bottleneck)
- **Проблема:** Основной поток обработки геометрии `RenderGuest` был привязан к ядру Core 4 (Cortex-A78) и долбился в 98% загрузки с троттлингом. При этом самое мощное Prime-ядро Core 7 (Cortex-X1 @ 2.84 GHz) простаивало на базовой частоте 844 МГц, так как поток `GtaBackend` в режиме прямого рекординга отключен.
- **Исправление в `android_thread_pinning.cpp`:**
  - В раскладке потоков `RenderGuest` переведен из `kGuestLane` в `Kind::kRecorderLane` (маска ядер `0xC0` — Cores 6 и 7, Cortex-X1 + Top Big Cortex-A78).
  - Выставлен агрессивный приоритет `nice = -10`.
- **Результат:** Поток `RenderGuest` задействовал Prime-ядро Cortex-X1 на частотах 2.38–2.84 ГГц.

### Баг 2: Избыточный опрос корутины (StreamPump Overhead)
- **Проблема:** В `graphics_system.cpp:8046` проверка `worker_command_cursor_ == batch_.size()` вызывала переключение контекста корутины на каждую команду. За один кадр происходило до 4 629 переключений контекста (`pumps/frame`).
- **Исправление:** Опрос стриминг-помпы переведен на шаг в 64 команды (`kBatchStride = 64`) и границы открытия кадра.
- **Результат:** Число переключений снизилось с 4 629 до 68 за кадр.

### Итоговый замер на OnePlus 9RT:
- Улица Либерти-Сити: время кадра упало с 75–90 мс (10–13 FPS) до **48.5 мс (20.6 FPS)**.
- Никаких визуальных артефактов, зависаний или рассинхронизации звука.

---

## 3. Стабильность сборки компилятора

- **Проблема:** Clang Thin-LTO падал с ошибкой OOM (`0x5AF`) при сборке в 8 потоков на хосте.
- **Исправление:** В `os/android/scripts/build_native.sh` выставлено ограничение:
  ```bash
  JOBS="${1:-${JOBS:-4}}"
  ```
  Сборка нативного кода теперь проходит стабильно без сбоев памяти.

---

## 4. Очистка репозитория и удаление мертвого стороннего кода

1. **Удалены неиспользуемые сабмодули и папки из `tools/`:**
   - `tools/DLSS` (NVIDIA DLSS не применим на мобильных GPU)
   - `tools/FidelityFX-SDK` и `tools/FidelityFX-SDK-2.1.0` (8 500+ файлов фреймворка Cauldron DX12)
   - `tools/fsr1`, `tools/smaa`
   - `tools/extract-xiso`, `tools/XexToolGUI`, `tools/SparkIV`
   - `tools/OpenOrbis-PS4-Toolchain` (тулчейн PS4)
   - `tools/libnx` (тулчейн Nintendo Switch)
   - `tools/ghidra_11.2.1_PUBLIC`
   - `tools/xenia-master-1`
   - `tools/GTAIV.EFLC.FusionFix-master`
   - `tools/xtd_tools` (включая бинарные архивы на 28 МБ)
   - `tools/xess`
2. **Удалены неиспользуемые сабмодули из `thirdparty/`:**
   - `thirdparty/MoltenVK` (транслятор Metal под macOS/iOS)
   - `thirdparty/plume`, `thirdparty/ffmpeg-core`, `thirdparty/GameNetworkingSockets`
3. **Обновлен `.gitmodules`** — оставлены только реальные зависимости, собираемые внутри `glue/rexglue-sdk-main/thirdparty/` для Android.
4. **Защищен `thirdparty/CMakeLists.txt`** — добавлены проверки `if (EXISTS ...)` во избежание сбоев конфигурации.
5. **Настроен `.gitignore`** — изолированы APK-файлы, временные дампы (`.txt`, `.csv`, `.diff`, `.zip`, `.pbtxt`, `.log`), бэкапы и каталоги `traces/`, `scratch/`, `shit_control/`.

---

## 5. Выгрузка в GitHub

- Репозиторий привязан к личному форку: `https://github.com/An7ik11er/LibertyRecompAndroid_some_vibetization.git`.
- Ветка `main` форка обновлена чистым коммитом `2435707a`.
- Токен доступа (PAT) удален из локального конфига Git в целях безопасности.
- Полная изоляция от апстрим-репозитория (`vaduur`): ни коммитов, ни PR, ни уведомлений разработчику оригинала не отправляется.
