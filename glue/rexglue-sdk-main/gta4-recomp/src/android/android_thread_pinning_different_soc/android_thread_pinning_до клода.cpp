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
                    "Pin audio to Prime core (X1) and engine workloads to Big cluster (A78)(888 snap)");

namespace {
	
	struct CpuTopology {
  uint32_t mask_little = 0;
  uint32_t mask_big = 0;
  uint32_t mask_prime = 0;
};

CpuTopology DetectTopology() {
  CpuTopology topo;
  uint32_t capacities[8] = {};
  uint32_t max_cap = 0;
  uint32_t min_cap = 999999;
  bool read_success = false;

  for (int cpu = 0; cpu < 8; ++cpu) {
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
    FILE* f = std::fopen(path, "r");
    if (!f) {
      // Запасной вариант через максимальную частоту
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
      f = std::fopen(path, "r");
    }
    if (f) {
      if (std::fscanf(f, "%u", &capacities[cpu]) == 1) {
        read_success = true;
      } else {
        capacities[cpu] = 0;
      }
      std::fclose(f);
    }
    
    if (capacities[cpu] > max_cap) max_cap = capacities[cpu];
    if (capacities[cpu] < min_cap && capacities[cpu] > 0) min_cap = capacities[cpu];
  }

  // Если SELinux заблокировал чтение /sys, возвращаем пустую структуру
  if (!read_success || max_cap == 0) {
    return topo; 
  }

  // Распределяем ядра по трем корзинам
  for (int cpu = 0; cpu < 8; ++cpu) {
    if (capacities[cpu] == 0) continue; // Пропускаем нечитаемые/отключенные ядра
    
    if (capacities[cpu] == max_cap) {
      topo.mask_prime |= (1u << cpu);
    } else if (capacities[cpu] <= min_cap + 100) { // +100 для запаса погрешности A55
      topo.mask_little |= (1u << cpu);
    } else {
      topo.mask_big |= (1u << cpu);
    }
  }
  return topo;
}
	

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

// Убираем CPU 7 из kMaskPerformance, чтобы рендер НЕ вытеснял звук на Prime-ядре!
void PinningLoop() {
  // 1. Пытаемся определить топологию динамически
  CpuTopology topo = DetectTopology();

uint32_t kMaskLittle = 0, kMaskA78 = 0, kMaskPrime = 0;
uint32_t kMaskPerformance = 0, kMaskWorkers = 0;
uint32_t kMaskAudio = 0, kMaskRecorder = 0, kMaskEngine = 0;

// 2. Универсальный Fallback (без привязки к SD888)
// Если топология не прочиталась (SELinux), мы гарантируем, что звук уйдет на САМОЕ СТАРШЕЕ ядро.
if (topo.mask_little == 0 || topo.mask_big == 0 || topo.mask_prime == 0) {
  uint32_t num_cores = std::thread::hardware_concurrency();
  if (num_cores == 0 || num_cores > 32) num_cores = 8; // Защита от безумных значений

  __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", 
                      "Affinity: Topology detection failed (SELinux?). Using generic %u-core fallback.", num_cores);

  // Самое старшее ядро (7-е для 8 ядер, 9-е для 10 ядер) — всегда Prime
  kMaskPrime = (1u << (num_cores - 1)); 
  
  // Остальные ядра делим пополам: верхняя половина — Big, нижняя — Little
  uint32_t half = num_cores / 2;
  kMaskA78 = 0;
  for (uint32_t i = half; i < num_cores - 1; ++i) kMaskA78 |= (1u << i);
  
  kMaskLittle = 0;
  for (uint32_t i = 0; i < half; ++i) kMaskLittle |= (1u << i);

  kMaskPerformance = kMaskA78 | kMaskPrime; // Рендеру отдаем все Big + Prime
  kMaskWorkers     = kMaskLittle | kMaskA78; // Воркерам — всё, кроме Prime
  kMaskAudio       = kMaskPrime;             // звук — только Prime
  kMaskRecorder    = kMaskA78 | kMaskPrime;  // запись — Big + Prime
  kMaskEngine = kMaskA78 | kMaskPrime; // 0xF0 (Big + Prime)
} else {
  __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp", 
                      "Affinity: Dynamic topology detected! Little: 0x%X, Big: 0x%X, Prime: 0x%X", 
                      topo.mask_little, topo.mask_big, topo.mask_prime);
  kMaskLittle      = topo.mask_little;
  kMaskA78         = topo.mask_big;
  kMaskPrime       = topo.mask_prime;

  kMaskPerformance = kMaskA78 | kMaskPrime;  // 0xF0
  kMaskWorkers     = kMaskLittle | kMaskA78; // 0x7F (Little + Big, без Prime)
  kMaskAudio       = kMaskPrime;             // 0x80 (только Prime)
  kMaskRecorder    = kMaskA78 | kMaskPrime;  // 0xF0 (Big + Prime)
  kMaskEngine = kMaskA78 | kMaskPrime; // 0xF0 (Big + Prime)
}

  // ВОТ ЭТИ ПЕРЕМЕННЫЕ ТЫ ПОТЕРЯЛ! Возвращаем их на место.
  std::unordered_set<pid_t> tracked_tids;
  static pid_t main_sdl_tid = 0;
  static pid_t main_recorder_tid = 0;
  static pid_t main_activity_tid = 0;

  while (true) {
    // Чаще, чем раз в секунду — audio-потоки иногда пересоздаются
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

        // 1. AUDIO → только Prime, nice -18 (каждый цикл, не tracked)
// 1a. Хостовый вывод звука (SDLAudio / AudioTrack) → эксклюзивно Prime (0x80), nice -18
      if (name.find("Track") != std::string::npos ||
          name.find("SDLAudio") != std::string::npos) {
        SetAffinityMask(tid, kMaskAudio);
        setpriority(PRIO_PROCESS, tid, -18);
        {
          static std::unordered_set<pid_t> audio_logged;
          if (!audio_logged.contains(tid)) {
            audio_logged.insert(tid);
            __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                                "Affinity: HOST AUDIO '%s' (tid %d) -> Prime 0x%X nice -18",
                                name.c_str(), tid, kMaskAudio);
          }
        }
        continue;
      }

      // 1b. Главный гостевой поток звука AudioGuest → Big+Prime (0xF0), nice -6
      if (name == "AudioGuest") {
        SetAffinityMask(tid, kMaskRecorder);
        setpriority(PRIO_PROCESS, tid, -6);
        {
          static std::unordered_set<pid_t> guest_audio_logged;
          if (!guest_audio_logged.contains(tid)) {
            guest_audio_logged.insert(tid);
            __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                                "Affinity: AUDIO GUEST '%s' (tid %d) -> Big+Prime 0x%X nice -6",
                                name.c_str(), tid, kMaskRecorder);
          }
        }
        continue;
      }

      // 1c. Фоновые декодеры (XMA Decoder, Audio Worker) → Little+Big (0x7F), nice -2
      // Приоритет nice -2 ниже, чем у AudioGuest (-6) и рендера (-4), чтобы они НЕ вытесняли их!
      if (name.find("Audio") != std::string::npos ||
          name.find("XMA") != std::string::npos) {
        SetAffinityMask(tid, kMaskWorkers);
        setpriority(PRIO_PROCESS, tid, -2);
        {
          static std::unordered_set<pid_t> xma_logged;
          if (!xma_logged.contains(tid)) {
            xma_logged.insert(tid);
            __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                                "Affinity: AUDIO HELPER '%s' (tid %d) -> Workers 0x%X nice -2",
                                name.c_str(), tid, kMaskWorkers);
          }
        }
        continue;
      }

      if (tracked_tids.contains(tid)) continue;

      // 2. GtaRecorder: только ПЕРВЫЙ поток — настоящий рекордер (Big+Prime 0xF0, nice -8).
      // Все дочерние потоки с унаследованным именем GtaRecorder (компиляторы пайплайнов) -> Workers (0x7F, nice -2)!
      if (name == "GtaRecorder") {
        if (main_recorder_tid == 0 || main_recorder_tid == tid) {
          main_recorder_tid = tid;
          SetAffinityMask(tid, kMaskRecorder);
          setpriority(PRIO_PROCESS, tid, -8);
          tracked_tids.insert(tid);
          __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                              "Affinity: PRIMARY GtaRecorder (tid %d) -> Big+Prime 0x%X nice -8",
                              tid, kMaskRecorder);
        } else {
          SetAffinityMask(tid, kMaskWorkers);
          setpriority(PRIO_PROCESS, tid, -2);
          tracked_tids.insert(tid);
          __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                              "Affinity: CHILD GtaRecorder helper (tid %d) -> Workers 0x%X nice -2",
                              tid, kMaskWorkers);
        }
      }
      // 3. Главный движок / RenderWork / Guest → Big+Prime (0xF0), nice -4
      else if (name.rfind("Main XThread", 0) == 0 ||
               name == "GtaRenderWork" ||
               name == "RenderGuest") {
        SetAffinityMask(tid, kMaskEngine);
        setpriority(PRIO_PROCESS, tid, -4);
        tracked_tids.insert(tid);
        __android_log_print(ANDROID_LOG_INFO, "LibertyRecomp",
                            "Affinity: Engine '%s' (tid %d) -> Big+Prime 0x%X nice -4",
                            name.c_str(), tid, kMaskEngine);
      }
      // 4. Критичные воркеры кадра (физика, джобы RAGE, чтение файлов ядра) → Workers (0x7F), nice -2
      else if (name == "RagePhysics" ||
               name == "RageJobWork" ||
               name.rfind("Kernel ", 0) == 0) {
        SetAffinityMask(tid, kMaskWorkers);
        setpriority(PRIO_PROCESS, tid, -2);
        tracked_tids.insert(tid);
      }
      // 5. Retirer (очистка памяти команд) → Workers (0x7F: Little + Big), nice 0
      else if (name == "GtaCmdRetirer") {
        SetAffinityMask(tid, kMaskWorkers);
        setpriority(PRIO_PROCESS, tid, 0);
        tracked_tids.insert(tid);
      }
      // 6. Фон → Little (0x0F)
      else if (name.find("TimerQueue") != std::string::npos ||
               name.find("Installer") != std::string::npos ||
               name.find("SDLTimer") != std::string::npos ||
               name.find("binder") != std::string::npos ||
               name.find("UIMonitor") != std::string::npos ||
               name == "RageFileIO") {
        SetAffinityMask(tid, kMaskLittle);
        tracked_tids.insert(tid);
      }
      // 7. SDL main → Little (0x0F) после старта
      else if (name == "SDLThread" || name == "SDLActivity") {
        SetAffinityMask(tid, kMaskLittle);
        tracked_tids.insert(tid);
      }
      // 8. RageStream и остальные воркеры Rage / XThread → Little+Big (0x7F), nice 0
      else if (name == "RageStream" || name.rfind("Rage", 0) == 0 || name.rfind("XThread", 0) == 0) {
        SetAffinityMask(tid, kMaskWorkers);
        setpriority(PRIO_PROCESS, tid, 0);
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