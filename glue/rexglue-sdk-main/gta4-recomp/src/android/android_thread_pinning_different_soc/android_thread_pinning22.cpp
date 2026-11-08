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
                    "Pin audio to Prime core and engine workloads to Big cluster (Snapdragon 855)");

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
  // Snapdragon 855 topology:
  // 0-3 : Kryo 485 Silver (A55) Little   (0x0F)
  // 4-6 : Kryo 485 Gold   (A76)          (0x70)
  // 7   : Kryo 485 Gold+  (A76 Prime)    (0x80)
  const uint32_t kMaskLittle      = 0x0F;  // 0-3
  const uint32_t kMaskA76         = 0x70;  // 4-6
  const uint32_t kMaskPrime       = 0x80;  // 7
  const uint32_t kMaskPerformance = 0xF0;  // 4-7
  const uint32_t kMaskWorkers     = 0x7C;  // 2-6

  std::unordered_set<pid_t> tracked_tids;
  static pid_t main_sdl_tid = 0;
  static pid_t main_recorder_tid = 0;
  static pid_t main_activity_tid = 0;

  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    if (!REXCVAR_GET(gta4_cluster_pinning)) {
      continue;
    }

    DIR* dir = opendir("/proc/self/task");
    if (!dir) continue;

    while (dirent* entry = readdir(dir)) {
      pid_t tid = pid_t(std::atoi(entry->d_name));
      if (tid <= 0) continue;

      std::string name = GetThreadName(tid);
      if (name.empty()) continue;

      // ------------------------------------------------------------------
      // 1. КРИТИЧЕСКИЙ AUDIO → только Prime (Gold+)
      // ------------------------------------------------------------------
      if (name.find("Audio") != std::string::npos ||
          name.find("XMA") != std::string::npos ||
          name.find("Track") != std::string::npos ||
          name.find("SDLAudio") != std::string::npos) {

        SetAffinityMask(tid, kMaskPrime);
        setpriority(PRIO_PROCESS, tid, -16);   // чуть мягче, чем на 888

        __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                            "Affinity: AUDIO '%s' (tid %d) -> CPU 7 (Prime) nice -16",
                            name.c_str(), tid);
        continue;
      }

      if (tracked_tids.contains(tid)) continue;

      // ------------------------------------------------------------------
      // 2. Главный поток движка + RenderWork → Big кластер (4-7)
      // ------------------------------------------------------------------
      if (name.rfind("Main XThread", 0) == 0 ||
          name == "GtaRenderWork" ||
          name == "RenderGuest") {
        SetAffinityMask(tid, kMaskPerformance);
        setpriority(PRIO_PROCESS, tid, -4);
        tracked_tids.insert(tid);
        __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                            "Affinity: Core engine '%s' (tid %d) -> CPU 4-7 nice -4",
                            name.c_str(), tid);
      }
      // ------------------------------------------------------------------
      // 3. GtaRecorder → Big
      // ------------------------------------------------------------------
      else if (name == "GtaRecorder") {
        if (main_recorder_tid == 0) {
          main_recorder_tid = tid;
          SetAffinityMask(tid, kMaskPerformance);
          setpriority(PRIO_PROCESS, tid, -4);
          __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                              "Affinity: Primary GtaRecorder (tid %d) -> CPU 4-7", tid);
        } else {
          SetAffinityMask(tid, kMaskA76);
        }
        tracked_tids.insert(tid);
      }
      // ------------------------------------------------------------------
      // 4. GtaCmdRetirer → A76 (4-6)
      // ------------------------------------------------------------------
      else if (name == "GtaCmdRetirer") {
        SetAffinityMask(tid, kMaskA76);
        setpriority(PRIO_PROCESS, tid, -2);
        tracked_tids.insert(tid);
        __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                            "Affinity: GtaCmdRetirer (tid %d) -> CPU 4-6", tid);
      }
      // ------------------------------------------------------------------
      // 5. Фоновые службы → Little
      // ------------------------------------------------------------------
      else if (name.find("TimerQueue") != std::string::npos ||
               name.find("Installer") != std::string::npos ||
               name.find("SDLTimer") != std::string::npos ||
               name.find("binder") != std::string::npos ||
               name.find("UIMonitor") != std::string::npos) {
        SetAffinityMask(tid, kMaskLittle);
        tracked_tids.insert(tid);
      }
      // ------------------------------------------------------------------
      // 6. SDLThread / SDLActivity
      // ------------------------------------------------------------------
      else if (name == "SDLThread") {
        if (main_sdl_tid == 0) {
          main_sdl_tid = tid;
          SetAffinityMask(tid, kMaskPerformance);
          setpriority(PRIO_PROCESS, tid, -4);
        } else {
          SetAffinityMask(tid, kMaskLittle);
        }
        tracked_tids.insert(tid);
      }
      else if (name == "SDLActivity") {
        if (main_activity_tid == 0) {
          main_activity_tid = tid;
          SetAffinityMask(tid, kMaskPerformance);
          setpriority(PRIO_PROCESS, tid, -4);
        } else {
          SetAffinityMask(tid, kMaskLittle);
        }
        tracked_tids.insert(tid);
      }
      // ------------------------------------------------------------------
      // 7. Обычные XThread → workers (2-6)
      // ------------------------------------------------------------------
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