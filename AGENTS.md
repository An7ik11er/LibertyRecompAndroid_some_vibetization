# Instructions for Assistant: Git, Memory, and Quotas

## 1. Repository Configuration
- Target repository: `https://github.com/An7ik11er/LibertyRecompAndroid_some_vibetization.git`
- Branch: `main`
- Upstream (`vaduur/LibertyRecompAndroid`): Frozen. Never commit, branch, or open PRs against upstream.

## 2. Git Safety Rules
1. Never run `git add .` or `git add -A`. Builds generate temporary logs, dumps, traces, and APKs. Always run `git status --short` first and stage only explicit target directories/files.
2. Never commit dirty submodules (`tools/XenosRecomp`, `glue/.../thirdparty/...`) showing modified status `m` caused by local compilation unless submodule commits were intentionally changed.
3. Never store GitHub PAT in plaintext inside `.git/config`. Push using inline credentials:
   `git push https://An7ik11er:<TOKEN>@github.com/An7ik11er/LibertyRecompAndroid_some_vibetization.git main`

## 3. Standard Git Workflow
1. Navigate to project: `cd G:\cod\LibertyRecompAndroid`
2. Verify status: `git status --short`
3. Stage targets: `git add <target_files>`
4. Commit: `git commit -m "feat/fix: summary"`
5. Push: `git push https://An7ik11er:<TOKEN>@github.com/An7ik11er/LibertyRecompAndroid_some_vibetization.git main`

## 4. Key Project Paths
- `G:\cod\cacheSlopodroid`: Android SDK toolchain and cache (NDK `30.0.16248370`, CMake `4.1.2`, ADB `platform-tools/adb.exe`, Gradle cache, build-tools). Pre-configured workspace toolchain; never search host SDKs or alter toolchain roots.
- `glue/rexglue-sdk-main/src/graphics/gta4_native/`: Vulkan native renderer, S1 streaming (`native_stream_record.inc`), deduplication, shaders.
- `glue/rexglue-sdk-main/gta4-recomp/src/android/`: Thread pinning and CPU affinity for Snapdragon 888.
- `os/android/app/src/main/java/com/libertyrecomp/`: Launcher UI, SoC presets, CLI arguments.
- `os/android/scripts/build_native.sh`: C++ NDK library build (`JOBS=4`).
- `os/android/scripts/build_apk.sh`: Final APK packaging via Gradle.

## 5. Mandatory Memory Synchronization
Upon receiving a new author package or concluding a test sprint, update `OUTSIDER_READ_LOG.md` simultaneously in both locations (`Outsider_memory` is gitignored):
1. Agent memory: `<appDataDir>\brain\<active-conversation-id>\OUTSIDER_READ_LOG.md`
2. Project workspace: `G:\cod\LibertyRecompAndroid\Outsider_memory\OUTSIDER_READ_LOG.md`

## 6. Quota and Execution Rules
1. Weekly Limit: Allowed down to 20% remaining for active sprint (default baseline 30%). When reaching 20%: complete immediate subtask (<=1%), persist progress and status to logs, notify user, and stop.
2. 5-Hour Limit: May be utilized freely; pause and wait when approaching the final 5% before reset.
3. Check Hygiene: Do not spam checks. Query `get_current_quota` once every 10–20 actions.
4. Scope Discipline: Avoid endless micro-optimizations. Stop once current improvements are sufficient for testing.

## 7. APK Build Rules (Debug by Default)
1. Always build Debug by default (`build_apk.sh debug` or `build_apk.sh`) with full unstripped symbols. Unstripped symbols are mandatory for `simpleperf` profiling, crash backtraces, and ndk-stack.
2. Stripping libraries (`strip_jnilibs.sh`) and building Release (`build_apk.sh release`) is strictly prohibited unless explicitly commanded by the user.

## 8. Background Task Waiting & Watchdog Rules
1. Never poll background tasks in a loop (`manage_task status` busy-polling is strictly forbidden). Antigravity automatically resumes execution upon task completion.
2. For long-running asynchronous commands (native C++ build, Gradle packaging), schedule a 5-minute watchdog timer via `schedule(DurationSeconds=300, Prompt="Watchdog: 5 min elapsed, check if build/task is still progressing or hung")`.
3. Do not run manual checks while waiting. Only inspect task status if the 5-minute watchdog timer fires, to verify whether the task is still actively outputting or has hung.
