# ИНСТРУКЦИЯ ПО РАБОТЕ С GIT ДЛЯ ЛЮБОГО АССИСТЕНТА И РАЗРАБОТЧИКА (GIT WORKFLOW)

Этот документ содержит **железные правила** работы с данным репозиторием.  
Если ты новый агент / ассистент после сжатия контекста — **прочитай это перед тем, как трогать Git!**

---

## 1. Параметры репозитория
- **Целевой репозиторий:** `https://github.com/An7ik11er/LibertyRecompAndroid_some_vibetization.git`
- **Рабочая ветка:** `main`
- **Оригинальный репо (`vaduur`):** **НЕ ТРОГАТЬ!** Оригинал заморожен. Никаких PR, веток или коммитов в `vaduur/LibertyRecompAndroid`.

---

## 2. Главные правила безопасности (КАК НЕ ЗАРУИНИТЬ)

1. **НИКОГДА не делать слепой `git add .` или `git add -A`!**  
   - В рабочей директории при компиляции и тестах образуются логи, дампы (`.txt`, `.csv`), дампы трейсов (`traces/`), APK-файлы и бэкапы.  
   - Сначала проверяем: `git status --short`.  
   - Добавляем только целевые изменённые файлы: например, `git add glue/ os/`.

2. **Не коммитить «грязные» сабмодули:**  
   - Сабмодули `tools/XenosRecomp`, `glue/rexglue-sdk-main/thirdparty/FFmpeg` и другие могут показывать статус `m` (modified content) из-за локальной компиляции.  
   - **Не делай** `git add tools/XenosRecomp` или `git add glue/.../thirdparty/...`, если ты целенаправленно не менял их коммиты.

3. **Не сохранять токен GitHub (PAT) в открытом виде:**  
   - Лучший и самый безопасный способ пуша с токеном без сохранения его в `.git/config`:
     ```powershell
     git push https://An7ik11er:<ТОКЕН>@github.com/An7ik11er/LibertyRecompAndroid_some_vibetization.git main
     ```
   - Если адрес с токеном был записан в `origin`, после пуша обязательно верни чистый URL:
     ```powershell
     git remote set-url origin https://github.com/An7ik11er/LibertyRecompAndroid_some_vibetization.git
     ```

---

## 3. Стандартный пошаговый цикл внесения правок

### Шаг 1. Проверить статус
```powershell
cd G:\cod\LibertyRecompAndroid
git status --short
```

### Шаг 2. Добавить только нужные файлы
```powershell
git add glue/rexglue-sdk-main/src/graphics/gta4_native/
git add glue/rexglue-sdk-main/gta4-recomp/src/android/
# или конкретные файлы
```

### Шаг 3. Сделать понятный коммит
```powershell
git commit -m "feat/fix: описание изменений"
```

### Шаг 4. Отправить в GitHub
```powershell
# Используем прямой пуш с токеном, чтобы не забивать конфиг
git push https://An7ik11er:<ТОКЕН>@github.com/An7ik11er/LibertyRecompAndroid_some_vibetization.git main
```

---

## 4. Что где лежит в проекте:
- `glue/rexglue-sdk-main/src/graphics/gta4_native/` — Vulkan нативный рендерер, стриминг S1 (`native_stream_record.inc`), дедупликация, шейдеры.
- `glue/rexglue-sdk-main/gta4-recomp/src/android/` — Привязка ядер (CPU Affinity) и потоков под Snapdragon 888.
- `os/android/app/src/main/java/com/libertyrecomp/` — Диалоги лаунчера, выбор пресетов SoC, аргументы.
- `os/android/scripts/build_native.sh` — Сборка C++ библиотек NDK (`JOBS=4`).
- `os/android/scripts/build_apk.sh` — Сборка финального APK через Gradle.
