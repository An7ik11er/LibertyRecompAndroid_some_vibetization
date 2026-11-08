#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>

#include <android/log.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(gta4_cluster_pinning, true, "GTA IV/Performance",
                    "Pin audio to Prime core (X1) and engine workloads to Big cluster (A78)");

namespace {

void SetAffinityMask(pid_t tid, uint32_t mask) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu = 0; cpu < 8; ++cpu) {
    if (mask & (1u << cpu)) {
      CPU_SET(cpu, &set);
    }
  }
  syscall(__NR_sched_setaffinity, tid, sizeof(set), &set);
}

std::string GetThreadName(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  FILE* file = std::fopen(path, "r");
  if (!file) return {};
  char name[64] = {};
  if (std::fgets(name, sizeof(name), file)) {
    size_t len = std::strlen(name);
    if (len > 0 && name[len - 1] == '\n') name[len - 1] = '\0';
  }
  std::fclose(file);
  return std::string(name);
}

void PinningLoop() {
  // Маски ядер Snapdragon 888:
  // 0x0F = CPU 0, 1, 2, 3 (Little / Cortex-A55)
  // 0xF0 = CPU 4, 5, 6, 7 (Big A78 + Prime X1 — 4 быстрых ядра)
  // 0xFC = CPU 2..7 (два быстрых Little + все Big/Prime)
  const uint32_t kMaskLittle = 0x0F;
  const uint32_t kMaskPerformance = 0xF0;
  const uint32_t kMaskWorkers = 0xFC;

  std::unordered_set<pid_t> tracked_tids;
  static pid_t main_sdl_tid = 0;
  static pid_t main_recorder_tid = 0;
  static pid_t main_activity_tid = 0;

  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    if (!REXCVAR_GET(gta4_cluster_pinning)) {
      continue;
    }

    DIR* dir = opendir("/proc/self/task");
    if (!dir) continue;

    while (dirent* entry = readdir(dir)) {
      pid_t tid = pid_t(std::atoi(entry->d_name));
      if (tid <= 0 || tracked_tids.contains(tid)) continue;

      std::string name = GetThreadName(tid);
      if (name.empty()) continue;

      // 1. Звук, XMA-декодеры и аудио треки -> на Little-ядра (CPU 0-3)
      // Им не нужна мощь A78/X1, они отлично работают на слабых ядрах без троттлинга
      if (name.find("Audio") != std::string::npos ||
          name.find("XMA") != std::string::npos ||
          name.find("Track") != std::string::npos ||
          name.find("SDLAudio") != std::string::npos) {
        SetAffinityMask(tid, kMaskLittle);
        tracked_tids.insert(tid);
        __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                            "Affinity: Audio worker '%s' (tid %d) -> CPU 0-3 (Little)",
                            name.c_str(), tid);
      }
      // 2. Главный поток движка и рендера -> Big кластер (CPU 4-7) с умеренным приоритетом
      else if (name.rfind("Main XThread", 0) == 0 ||
               name == "GtaRenderWork") {
        SetAffinityMask(tid, kMaskPerformance);
        // Убираем экстремальный nice -8, чтобы процессор не уходил в перегрев и троттлинг
        setpriority(PRIO_PROCESS, tid, -2);
        tracked_tids.insert(tid);
        __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                            "Affinity: Core engine '%s' (tid %d) -> CPU 4-7 (nice -2)",
                            name.c_str(), tid);
      }
      // 3. GtaRecorder -> первый на CPU 4-7, остальные воркеры на CPU 2-7
      else if (name == "GtaRecorder") {
        if (main_recorder_tid == 0) {
          main_recorder_tid = tid;
          SetAffinityMask(tid, kMaskPerformance);
          setpriority(PRIO_PROCESS, tid, -2);
          __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                              "Affinity: Primary GtaRecorder (tid %d) -> CPU 4-7", tid);
        } else {
          SetAffinityMask(tid, kMaskWorkers);
        }
        tracked_tids.insert(tid);
      }
      // 4. Фоновый поток очистки команд (RetireBatch), таймеры и системные службы -> строго на CPU 0-3
      else if (name == "GtaCmdRetirer" ||
               name.find("TimerQueue") != std::string::npos ||
               name.find("Installer") != std::string::npos ||
               name.find("SDLTimer") != std::string::npos ||
               name.find("binder") != std::string::npos) {
        SetAffinityMask(tid, kMaskLittle);
        tracked_tids.insert(tid);
        __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                            "Affinity: Background worker '%s' (tid %d) -> CPU 0-3",
                            name.c_str(), tid);
      }
      // 5. SDLThread и SDLActivity -> первый на CPU 4-7, остальные на Little
      else if (name == "SDLThread") {
        if (main_sdl_tid == 0) {
          main_sdl_tid = tid;
          SetAffinityMask(tid, kMaskPerformance);
        } else {
          SetAffinityMask(tid, kMaskLittle);
        }
        tracked_tids.insert(tid);
      }
      else if (name == "SDLActivity") {
        if (main_activity_tid == 0) {
          main_activity_tid = tid;
          SetAffinityMask(tid, kMaskPerformance);
        } else {
          SetAffinityMask(tid, kMaskLittle);
        }
        tracked_tids.insert(tid);
      }
      // 6. Гостевые рабочие потоки XThread -> распределяем на CPU 2-7
      else if (name.rfind("XThread", 0) == 0) {
        SetAffinityMask(tid, kMaskWorkers);
        tracked_tids.insert(tid);
      }
    }
    closedir(dir);
  }
}

const bool g_pinning_started = [] {
  std::thread(PinningLoop).detach();
  return true;
}();

}  // namespace