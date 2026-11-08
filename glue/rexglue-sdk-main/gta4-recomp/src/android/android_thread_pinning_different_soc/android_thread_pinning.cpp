// android_thread_pinning.cpp — размещение известных потоков игры по ядрам.
//
// Откуда берётся раскладка (первое найденное):
//   1. переменная окружения LIBERTY_THREAD_AFFINITY — её ставит LibertyActivity
//      до загрузки libmain.so (значение из настроек лаунчера);
//   2. cvar android_thread_affinity (args.txt / native.toml / live_cvars.txt).
// Формат обоих:
//   auto
//   manual;<id>:<hex mask>;<id>:<hex mask>...
// В ручном режиме потоки, которых нет в списке, размещаются автоматически.
// Идентификаторы и автоматические маски совпадают с ThreadAffinityDialog.java (лаунчер).
#include <dirent.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <android/log.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(gta4_cluster_pinning, true, "GTA IV/Performance",
                    "Pin the game's known threads to CPU clusters");
REXCVAR_DEFINE_STRING(android_thread_affinity, "auto", "GTA IV/Performance",
                      "Thread placement: 'auto' or 'manual;<thread id>:<hex cpu mask>;...' "
                      "(unlisted threads stay automatic). The launcher's choice "
                      "(LIBERTY_THREAD_AFFINITY) wins over this value.");

#ifndef LIBERTY_CPU_SYSFS
#define LIBERTY_CPU_SYSFS "/sys/devices/system/cpu"
#endif

namespace {

constexpr const char* kTag = "LibertyRecomp";
constexpr const char* kEnvSpec = "LIBERTY_THREAD_AFFINITY";
constexpr int kMaxCpus = 16;

// ---------------------------------------------------------------- topology

struct CpuTopology {
  int count = 8;
  uint32_t little = 0, big = 0, prime = 0;
  bool fallback = false;
  uint32_t all() const { return count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1u); }
};

int ReadInt(const char* path) {
  FILE* f = std::fopen(path, "r");
  if (!f) return -1;
  int value = -1;
  if (std::fscanf(f, "%d", &value) != 1) value = -1;
  std::fclose(f);
  return value;
}

int PossibleCpuCount() {
  FILE* f = std::fopen(LIBERTY_CPU_SYSFS "/possible", "r");
  int count = 0;
  if (f) {
    char buf[64] = {};
    if (std::fgets(buf, sizeof(buf), f)) {
      const char* dash = std::strrchr(buf, '-');
      count = std::atoi(dash ? dash + 1 : buf) + 1;
    }
    std::fclose(f);
  }
  if (count <= 0) count = int(sysconf(_SC_NPROCESSORS_CONF));
  if (count <= 0) count = 8;
  return count > kMaxCpus ? kMaxCpus : count;
}

CpuTopology DetectTopology() {
  CpuTopology topo;
  topo.count = PossibleCpuCount();
  int cap[kMaxCpus] = {};
  int max_cap = 0, min_cap = 0x7FFFFFFF;
  bool ok = false;
  for (int cpu = 0; cpu < topo.count; ++cpu) {
    char path[160];
    std::snprintf(path, sizeof(path), LIBERTY_CPU_SYSFS "/cpu%d/cpu_capacity", cpu);
    int c = ReadInt(path);
    if (c <= 0) {
      std::snprintf(path, sizeof(path), LIBERTY_CPU_SYSFS "/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
      c = ReadInt(path);
    }
    cap[cpu] = c > 0 ? c : 0;
    if (c > 0) {
      ok = true;
      if (c > max_cap) max_cap = c;
      if (c < min_cap) min_cap = c;
    }
  }
  if (ok) {
    for (int cpu = 0; cpu < topo.count; ++cpu) {
      if (cap[cpu] <= 0) continue;
      if (cap[cpu] == max_cap) topo.prime |= 1u << cpu;
      else if (cap[cpu] <= min_cap + 100) topo.little |= 1u << cpu;  // +100: допуск для A55
      else topo.big |= 1u << cpu;
    }
    // Для 2-кластерных чипов 6+2 (8 Elite / Gen 5: CPU0-5 Performance, CPU6-7 Prime)
    // делим 6 ядер Performance пополам: 0x07 под воркеры и 0x38 под рендер/аудио,
    // сохраняя оба Prime-ядра (0xC0) в topo.prime (синхронно с ThreadAffinityDialog.java).
    if (topo.little && topo.prime && !topo.big) {
      int move_to_big = __builtin_popcount(topo.little) / 2;
      for (int cpu = topo.count - 1; cpu >= 0 && move_to_big > 0; --cpu) {
        if (topo.little & (1u << cpu)) {
          topo.little &= ~(1u << cpu);
          topo.big |= (1u << cpu);
          --move_to_big;
        }
      }
    }
  }
  if (!topo.little || !topo.big || !topo.prime) {
    topo.fallback = true;
    const int n = topo.count, half = n / 2;
    topo.prime = 1u << (n - 1);
    topo.big = 0;
    for (int i = half; i < n - 1; ++i) topo.big |= 1u << i;
    topo.little = 0;
    for (int i = 0; i < half; ++i) topo.little |= 1u << i;
  }
  return topo;
}

// ---------------------------------------------------------------- known threads

enum class Kind : uint8_t { kLittle, kBig, kPrime, kBigPrime, kWorkers, kLittleBig };

uint32_t AutoMask(Kind kind, const CpuTopology& t) {
  switch (kind) {
    case Kind::kLittle: return t.little;
    case Kind::kBig: return t.big;
    case Kind::kPrime: return t.prime;
    case Kind::kBigPrime: return t.big | t.prime;
    case Kind::kWorkers: return t.little ? t.little : (t.little | t.big);
    case Kind::kLittleBig: return t.little | t.big;
  }
  return t.little;
}

enum ThreadId : uint8_t {
  kRenderGuest, kRecorder, kRenderWork, kMainXThread, kSdlThread, kSdlHelpers,
  kAudioGuest, kHostAudio, kXmaDecoder, kAudioWorker,
  kRageStream, kRageFileIO, kKernelIO,
  kRageJobs, kRagePhysics, kRageOther,
  kCmdRetirer, kRecorderHelpers, kSdlActivity, kBackground,
  kThreadIdCount,
  kUnknown = 0xFF,
};

constexpr int kKeepNice = 1000;  // не трогать приоритет

struct ThreadInfo {
  const char* id;  // = id в ThreadAffinityDialog.java
  Kind auto_kind;  // автоматическое размещение
  int nice;        // kKeepNice = не менять
  bool sticky;     // удерживать маску и в auto
};

constexpr std::array<ThreadInfo, kThreadIdCount> kThreads = {{
    {"render_guest", Kind::kBigPrime, -6, true},
    {"recorder", Kind::kBigPrime, -8, true},
    {"render_work", Kind::kBig, -4, true},
    {"main_xthread", Kind::kBigPrime, -6, true},
    {"sdl_thread", Kind::kBig, -2, false},
    {"sdl_helpers", Kind::kLittle, kKeepNice, false},
    {"audio_guest", Kind::kBig, -8, true},
    {"host_audio", Kind::kPrime, -18, true},
    {"xma_decoder", Kind::kWorkers, -16, true},
    {"audio_worker", Kind::kWorkers, -16, true},
    {"rage_stream", Kind::kWorkers, 0, false},
    {"rage_fileio", Kind::kLittle, kKeepNice, false},
    {"kernel_io", Kind::kWorkers, -2, false},
    {"rage_jobs", Kind::kLittleBig, -2, false},
    {"rage_physics", Kind::kLittleBig, -2, false},
    {"rage_other", Kind::kWorkers, 0, false},
    {"cmd_retirer", Kind::kWorkers, 0, false},
    {"recorder_helpers", Kind::kWorkers, -2, false},
    {"sdl_activity", Kind::kLittle, kKeepNice, false},
    {"background", Kind::kLittle, kKeepNice, false},
}};

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool Starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

ThreadId Classify(const std::string& name, pid_t tid, pid_t primary_recorder, pid_t primary_sdl) {
  if (Has(name, "Track") || Has(name, "SDLAudio")) return kHostAudio;
  if (name == "AudioGuest") return kAudioGuest;
  if (Has(name, "XMA")) return kXmaDecoder;
  if (Has(name, "Audio")) return kAudioWorker;
  if (name == "GtaRecorder") return tid == primary_recorder ? kRecorder : kRecorderHelpers;
  if (name == "RenderGuest") return kRenderGuest;
  if (Starts(name, "Main XThread")) return kMainXThread;
  if (name == "GtaRenderWork") return kRenderWork;
  if (name == "RagePhysics") return kRagePhysics;
  if (name == "RageJobWork") return kRageJobs;
  if (Starts(name, "Kernel ")) return kKernelIO;
  if (name == "GtaCmdRetirer") return kCmdRetirer;
  if (name == "RageFileIO") return kRageFileIO;
  if (Has(name, "TimerQueue") || Has(name, "Installer") || Has(name, "SDLTimer") ||
      Has(name, "binder") || Has(name, "UIMonitor"))
    return kBackground;
  if (name == "SDLThread") return tid == primary_sdl ? kSdlThread : kSdlHelpers;
  if (name == "SDLActivity") return kSdlActivity;
  if (name == "RageStream") return kRageStream;
  if (Starts(name, "Rage") || Starts(name, "XThread")) return kRageOther;
  return kUnknown;
}

// ---------------------------------------------------------------- spec

struct Placement {
  bool manual = false;
  std::array<uint32_t, kThreadIdCount> mask{};  // 0 = автоматически
};

int FindThreadId(const std::string& id) {
  for (int i = 0; i < kThreadIdCount; ++i)
    if (id == kThreads[i].id) return i;
  return -1;
}

std::string Trim(std::string s) {
  const char* ws = " \t\r\n\"'";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  const size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

Placement ParseSpec(const std::string& spec, const CpuTopology& topo) {
  Placement p;
  size_t pos = 0;
  bool first = true;
  while (pos <= spec.size()) {
    size_t end = spec.find(';', pos);
    if (end == std::string::npos) end = spec.size();
    const std::string token = Trim(spec.substr(pos, end - pos));
    pos = end + 1;
    if (token.empty()) continue;
    if (first) {
      first = false;
      if (token == "manual") { p.manual = true; continue; }
      if (token == "auto") return p;
      p.manual = true;
    }
    const size_t colon = token.find(':');
    if (colon == std::string::npos) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: bad entry '%s' (ignored)", token.c_str());
      continue;
    }
    const std::string id = Trim(token.substr(0, colon));
    const int index = FindThreadId(id);
    if (index < 0) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: unknown thread id '%s' (ignored)", id.c_str());
      continue;
    }
    char* tail = nullptr;
    const std::string hex = Trim(token.substr(colon + 1));
    const unsigned long requested = std::strtoul(hex.c_str(), &tail, 16);
    if (hex.empty() || (tail && *tail)) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: %s: bad mask '%s' (ignored)", id.c_str(),
                          hex.c_str());
      continue;
    }
    const uint32_t mask = uint32_t(requested) & topo.all();
    if (!mask) {
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "Affinity: %s mask 0x%lX has no cores on this device, automatic placement used",
                          id.c_str(), requested);
      continue;
    }
    p.mask[index] = mask;
  }
  bool any = false;
  for (uint32_t m : p.mask) any |= m != 0;
  if (!any) p.manual = false;
  return p;
}

uint32_t EffectiveMask(const Placement& p, ThreadId id, const CpuTopology& topo) {
  return p.manual && p.mask[id] ? p.mask[id] : AutoMask(kThreads[id].auto_kind, topo);
}

void LogPlacement(const Placement& p, const CpuTopology& topo, const char* source) {
  if (!p.manual) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: mode=auto (source: %s)", source);
    return;
  }
  std::string list;
  for (int i = 0; i < kThreadIdCount; ++i) {
    if (!p.mask[i]) continue;
    char item[64];
    std::snprintf(item, sizeof(item), " %s=0x%X", kThreads[i].id, p.mask[i]);
    list += item;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: mode=manual (source: %s, others auto):%s", source,
                      list.c_str());
  const ThreadId heavy[] = {kRenderGuest, kRecorder, kRenderWork, kMainXThread, kSdlThread, kAudioGuest};
  for (ThreadId id : heavy) {
    const uint32_t m = EffectiveMask(p, id, topo);
    if (!(m & ~topo.little))
      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: note: %s only on little cores", kThreads[id].id);
  }
  for (size_t a = 0; a < std::size(heavy); ++a) {
    for (size_t b = a + 1; b < std::size(heavy); ++b) {
      const uint32_t ma = EffectiveMask(p, heavy[a], topo), mb = EffectiveMask(p, heavy[b], topo);
      const uint32_t both = ma | mb;
      if (__builtin_popcount(both) == 1)
        __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: note: %s and %s share one core (0x%X)",
                            kThreads[heavy[a]].id, kThreads[heavy[b]].id, both);
    }
  }
}

// ---------------------------------------------------------------- applying

bool SetAffinityMask(pid_t tid, uint32_t mask) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu = 0; cpu < 32; ++cpu)
    if (mask & (1u << cpu)) CPU_SET(cpu, &set);
  return syscall(__NR_sched_setaffinity, tid, sizeof(set), &set) == 0;
}

uint32_t GetAffinityMask(pid_t tid) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (syscall(__NR_sched_getaffinity, tid, sizeof(set), &set) < 0) return 0;
  uint32_t mask = 0;
  for (int cpu = 0; cpu < 32; ++cpu)
    if (CPU_ISSET(cpu, &set)) mask |= 1u << cpu;
  return mask;
}

std::string GetThreadName(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  FILE* file = std::fopen(path, "r");
  if (!file) return {};
  char name[64] = {};
  if (std::fgets(name, sizeof(name), file)) {
    const size_t len = std::strlen(name);
    if (len && name[len - 1] == '\n') name[len - 1] = '\0';
  }
  std::fclose(file);
  return name;
}

struct Applied {
  std::string name;
  uint32_t mask = 0;
  uint32_t effective = 0;
  uint32_t generation = 0;
  uint32_t restored = 0;
  bool nice_ok = false;
};

struct Seen {
  pid_t tid;
  std::string name;
};

struct SpecSource {
  bool from_env = false;
  std::string env_value;

  void Init() {
    const char* env = std::getenv(kEnvSpec);
    if (env && *env) {
      from_env = true;
      env_value = Trim(env);
    }
  }
  std::string Current(const char*& source) const {
    if (from_env) {
      source = "launcher";
      return env_value;
    }
    source = "cvar";
    return Trim(REXCVAR_GET(android_thread_affinity));
  }
};

bool IsPowerOfTen(uint32_t n) {
  while (n >= 10 && n % 10 == 0) n /= 10;
  return n == 1;
}

void PinningLoop() {
  const CpuTopology topo = DetectTopology();
  if (topo.fallback) {
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "Affinity: topology detection failed (SELinux?). Generic %d-core fallback "
                        "Little: 0x%X, Big: 0x%X, Prime: 0x%X",
                        topo.count, topo.little, topo.big, topo.prime);
  } else {
    __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: topology Little: 0x%X, Big: 0x%X, Prime: 0x%X",
                        topo.little, topo.big, topo.prime);
  }

  SpecSource source;
  source.Init();
  bool cvar_conflict_logged = false;

  std::unordered_map<pid_t, Applied> applied;
  std::unordered_set<std::string> unknown_logged;
  std::vector<Seen> seen;
  std::string spec_text;
  Placement placement;
  bool spec_parsed = false;
  uint32_t generation = 0;
  const auto started = std::chrono::steady_clock::now();
  bool summary_logged = false;

  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!REXCVAR_GET(gta4_cluster_pinning)) continue;

    const char* source_name = "";
    const std::string spec = source.Current(source_name);
    if (!spec_parsed || spec != spec_text) {
      spec_text = spec;
      placement = ParseSpec(spec, topo);
      spec_parsed = true;
      __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: spec [%s] from %s", spec.c_str(), source_name);
      LogPlacement(placement, topo, source_name);
      for (auto& entry : applied) entry.second.mask = 0;
    }
    if (source.from_env && !cvar_conflict_logged) {
      const std::string cvar = Trim(REXCVAR_GET(android_thread_affinity));
      if (cvar != "auto" && cvar != spec) {
        cvar_conflict_logged = true;
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "Affinity: android_thread_affinity='%s' (native.toml/args.txt) ignored: "
                            "the launcher's setting is used",
                            cvar.c_str());
      }
    }

    DIR* dir = opendir("/proc/self/task");
    if (!dir) continue;
    seen.clear();
    pid_t primary_recorder = 0, primary_sdl = 0;
    while (dirent* entry = readdir(dir)) {
      const pid_t tid = pid_t(std::atoi(entry->d_name));
      if (tid <= 0) continue;
      std::string name = GetThreadName(tid);
      if (name.empty()) continue;
      if (name == "GtaRecorder" && (!primary_recorder || tid < primary_recorder)) primary_recorder = tid;
      if (name == "SDLThread" && (!primary_sdl || tid < primary_sdl)) primary_sdl = tid;
      seen.push_back({tid, std::move(name)});
    }
    closedir(dir);

    ++generation;
    for (const Seen& s : seen) {
      const ThreadId id = Classify(s.name, s.tid, primary_recorder, primary_sdl);
      Applied& state = applied[s.tid];
      state.generation = generation;
      if (id == kUnknown) {
        if (state.name != s.name && unknown_logged.size() < 64 && unknown_logged.insert(s.name).second)
          __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: unclassified thread '%s' (tid %d) left as is",
                              s.name.c_str(), s.tid);
        state.name = s.name;
        state.mask = 0;
        continue;
      }
      const ThreadInfo& info = kThreads[id];
      const bool manual = placement.manual && placement.mask[id] != 0;
      const uint32_t mask = manual ? placement.mask[id] : AutoMask(info.auto_kind, topo);
      const bool fresh = state.name != s.name || state.mask != mask;
      const bool hold = manual || info.sticky;
      if (!fresh && !hold) continue;

      bool changed_outside = false;
      if (!fresh && state.effective) {
        const uint32_t current = GetAffinityMask(s.tid);
        changed_outside = current != 0 && current != state.effective;
        if (info.nice != kKeepNice && state.nice_ok) {
          errno = 0;
          const int prio = getpriority(PRIO_PROCESS, id_t(s.tid));
          if (errno == 0 && prio != info.nice) changed_outside = true;
        }
        if (!changed_outside) continue;
      }

      const bool ok = SetAffinityMask(s.tid, mask);
      state.effective = ok ? GetAffinityMask(s.tid) : 0;
      if (info.nice != kKeepNice) state.nice_ok = setpriority(PRIO_PROCESS, id_t(s.tid), info.nice) == 0;
      if (fresh) {
        __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
                            "Affinity: %s '%s' (tid %d) -> 0x%X nice %d%s%s", info.id, s.name.c_str(), s.tid,
                            mask, info.nice == kKeepNice ? 0 : info.nice, manual ? " [manual]" : " [auto]",
                            ok ? "" : " FAILED");
      } else if (changed_outside && IsPowerOfTen(++state.restored)) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "Affinity: %s '%s' (tid %d) mask/nice was changed outside, restored (%u times)",
                            info.id, s.name.c_str(), s.tid, state.restored);
      }
      state.name = s.name;
      state.mask = mask;
    }

    for (auto it = applied.begin(); it != applied.end();) {
      if (it->second.generation != generation) it = applied.erase(it);
      else ++it;
    }

    if (!summary_logged && std::chrono::steady_clock::now() - started > std::chrono::seconds(20)) {
      summary_logged = true;
      std::array<int, kThreadIdCount> count{};
      int unknown = 0;
      for (const Seen& s : seen) {
        const ThreadId id = Classify(s.name, s.tid, primary_recorder, primary_sdl);
        if (id == kUnknown) ++unknown;
        else ++count[id];
      }
      std::string list;
      for (int i = 0; i < kThreadIdCount; ++i) {
        if (!count[i]) continue;
        char item[80];
        std::snprintf(item, sizeof(item), " %s x%d=0x%X", kThreads[i].id, count[i],
                      EffectiveMask(placement, ThreadId(i), topo));
        list += item;
      }
      __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: summary (%zu threads):%s unclassified x%d",
                          seen.size(), list.c_str(), unknown);
    }
  }
}

const bool g_pinning_started = [] {
  std::thread(PinningLoop).detach();
  return true;
}();

}  // namespace