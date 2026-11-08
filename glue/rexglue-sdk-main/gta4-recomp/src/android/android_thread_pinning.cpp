// android_thread_pinning.cpp — размещение известных потоков игры по ядрам и их приоритеты.
//
// Откуда берётся ручная раскладка (первое найденное):
//   1. переменная окружения LIBERTY_THREAD_AFFINITY — её ставит LibertyActivity
//      до загрузки libmain.so (значение из настроек лаунчера);
//   2. cvar android_thread_affinity (args.txt / native.toml / live_cvars.txt).
// Формат обоих:
//   auto
//   manual;<id>:<hex mask>;<id>:<hex mask>...
// В ручном режиме потоки, которых нет в списке, размещаются автоматически.
//
// Автоматика выбирается cvar android_thread_layout:
//   lanes    — «полосы» (по умолчанию): рекордер на Prime + верхнем Big, аудио на нижних
//              Big, где рекордера нет; задачи игры могут уходить на Big; сервисы на Little.
//   clusters — прежняя таблица (по кластерам, как в версии 26.10).
//   priority — только приоритеты (nice из «полос»), маски не ставятся: ядра выбирает
//              планировщик Android. Проверяет, что дают именно маски.
//   off      — ничего не трогать (решает Android; база для сравнения).
// Если раскладку сменить на лету (live_cvars.txt), маски, поставленные раньше, снимаются
// (поток снова может работать на всех ядрах); nice остаётся последним поставленным.
//
// Хостовый вывод звука (AudioTrack / SDLAudio*) не трогается ни в одном режиме: его приоритет
// ставят AAudio/SDL (бывает SCHED_FIFO от системы), а повторная установка nice каждые 400 мс
// только спорит с ними (в прогоне 888 от 26.10 — «restored» десятки раз).
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
REXCVAR_DEFINE_STRING(android_thread_layout, "lanes", "GTA IV/Performance",
                      "Automatic placement: lanes (recorder on prime + top big core, audio on the "
                      "other big cores), clusters (previous per-cluster table), priority (the "
                      "lanes' nice values without CPU masks), or off (Android decides)");

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
  // Старшее Big-ядро (рядом с Prime): его получает рекордер.
  uint32_t top_big() const { return big ? 1u << (31 - __builtin_clz(big)) : 0; }
  // Big без старшего ядра: здесь живёт аудио, чтобы не делить ядро с рекордером.
  uint32_t big_low() const {
    const uint32_t rest = big & ~top_big();
    return rest ? rest : big;
  }
};

int ReadInt(const char* path) {
  FILE* f = std::fopen(path, "r");
  if (!f) return -1;
  int value = -1;
  if (std::fscanf(f, "%d", &value) != 1) value = -1;
  std::fclose(f);
  return value;
}

std::string ReadSmallFile(pid_t tid, const char* leaf) {
  char path[96];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/%s", tid, leaf);
  FILE* f = std::fopen(path, "r");
  if (!f) return "?";
  char buf[64] = {};
  if (std::fgets(buf, sizeof(buf), f)) {
    const size_t len = std::strlen(buf);
    if (len && buf[len - 1] == '\n') buf[len - 1] = '\0';
  }
  std::fclose(f);
  return buf;
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
    // 2-кластерные 6+2 (8 Elite / Gen 5: CPU0-5 Performance, CPU6-7 Prime): нижнюю половину
    // Performance считаем «little» (0x07), верхнюю — «big» (0x38), Prime остаётся 0xC0
    // (как в версии 26.10, синхронно с ThreadAffinityDialog.java).
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

enum class Kind : uint8_t {
  kNone,          // affinity не трогаем
  kLittle,        // сервисы
  kBig,           // Big
  kPrime,
  kBigPrime,
  kWorkers,       // как в 26.10: Little, если есть, иначе Little|Big
  kLittleBig,
  kRecorderLane,  // Prime + старшее Big (минимум два ядра)
  kAudioLane,     // Big без старшего ядра
  kJobsLane,      // Little + Big без старшего ядра
};

uint32_t AutoMask(Kind kind, const CpuTopology& t) {
  switch (kind) {
    case Kind::kNone: return 0;
    case Kind::kLittle: return t.little;
    case Kind::kBig: return t.big;
    case Kind::kPrime: return t.prime;
    case Kind::kBigPrime: return t.big | t.prime;
    case Kind::kWorkers: return t.little ? t.little : (t.little | t.big);
    case Kind::kLittleBig: return t.little | t.big;
    case Kind::kRecorderLane: return t.prime | t.top_big();
    case Kind::kAudioLane: return t.big_low();
    case Kind::kJobsLane: return t.little | t.big_low();
  }
  return t.little;
}

enum ThreadId : uint8_t {
  kRenderGuest, kRecorder, kRenderWork, kMainXThread, kSdlThread, kSdlHelpers,
  kAudioGuest, kHostAudio, kXmaDecoder, kAudioWorker,
  kRageStream, kRageFileIO, kKernelIO,
  kRageJobs, kRagePhysics, kRageOther, kRageSim,
  kCmdRetirer, kRecorderHelpers, kSdlActivity, kBackground,
  kBackend,  // 47: GtaBackend (gta4_native_backend_thread): render worker + frame recording
  kCapture,  // 56: GtaCapture (gta4_native_capture_thread): the capture taken off RenderGuest
  kThreadIdCount,
  kUnknown = 0xFF,
};

constexpr int kKeepNice = 1000;  // не трогать приоритет

struct Rule {
  Kind kind;
  int nice;     // kKeepNice = не менять
  bool sticky;  // удерживать маску и nice (возвращать, если их поменяли)
};

constexpr std::array<const char*, kThreadIdCount> kIds = {{
    "render_guest", "recorder", "render_work", "main_xthread", "sdl_thread", "sdl_helpers",
    "audio_guest", "host_audio", "xma_decoder", "audio_worker",
    "rage_stream", "rage_fileio", "kernel_io",
    "rage_jobs", "rage_physics", "rage_other", "rage_sim",
    "cmd_retirer", "recorder_helpers", "sdl_activity", "background",
    "backend", "capture",
}};

// «Полосы». Нагрузка по оверлею 888 (26.10, % одного ядра): AudioGuest 37, RenderGuest 35,
// GtaRecorder 32 (до ~100 на 855), GtaCmdRetirer 26, GtaRenderWork 23, XMA 18, RageStream 18,
// Main XThread 18, SDLThread 11.
//   855/888: рекордер {6,7}; RenderGuest/Main {4-7}; RenderWork {4-6}; аудио {4,5};
//            задачи игры {0-5}; сервисы {0-3}.
//   Gen 5:   рекордер {5,6,7}; RenderGuest/Main {3-7}; RenderWork {3-5}; аудио {3,4};
//            задачи игры {0-4}; сервисы {0-2}.
// Аудио выше всех по приоритету (-16 = ANDROID_PRIORITY_AUDIO, то же ставит сам рантайм
// в audio_system.cpp / xma_decoder.cpp), но на ядрах, где рекордера нет.
constexpr std::array<Rule, kThreadIdCount> kLanes = {{
    {Kind::kRecorderLane, -10, true},   // render_guest
    {Kind::kRecorderLane, -10, true},   // recorder
    {Kind::kBig, -8, true},             // render_work (A/B test: nice -8)
    {Kind::kBigPrime, -8, true},        // main_xthread
    {Kind::kLittle, -4, false},         // sdl_thread (презентер)
    {Kind::kLittle, kKeepNice, false},  // sdl_helpers
    {Kind::kAudioLane, -16, true},      // audio_guest
    {Kind::kNone, kKeepNice, false},    // host_audio — не трогаем
    {Kind::kAudioLane, -16, true},      // xma_decoder
    {Kind::kAudioLane, -16, true},      // audio_worker
    // 43: +2. World streaming (strStreaming), ~55 % of a Little core while driving, nobody
    // waits for it within a frame; it delayed the physics workers on the same cores.
    {Kind::kLittle, 2, false},          // rage_stream
    {Kind::kLittle, kKeepNice, false},  // rage_fileio
    {Kind::kLittle, -2, false},         // kernel_io
    {Kind::kJobsLane, -2, false},       // rage_jobs (ждёт главный поток игры)
    // 43: -6 as in lanes2. The 42 runs: physics workers were woken in time but got a core
    // 2-3 ms later ("late-run"), 3-7 times per 5 s, mostly at the end-of-step barrier.
    {Kind::kJobsLane, -6, false},       // rage_physics
    {Kind::kLittle, 0, false},          // rage_other
    // 41: Rage_1BA088 (entry 0x821BA088, physics/simulation code 0x827A-0x8281). Main
    // XThread waits for it 3.4 ms a frame (30.10 20:16, 888); it ran 100 % on Little at
    // nice 0 next to RageStream (55 % of a Little core). Same lane as the job pools.
    {Kind::kJobsLane, -6, false},       // rage_sim
    {Kind::kLittle, 0, false},          // cmd_retirer
    {Kind::kLittle, 10, false},         // recorder_helpers
    {Kind::kLittle, kKeepNice, false},  // sdl_activity
    {Kind::kLittle, kKeepNice, false},  // background
    // 47: backend only on Prime. 888, 03.11 (D vs D3): FinishPresent 17.8 ms on Prime only
    // vs 20.0 ms on Prime + top Big; 30.6 vs 29.1 FPS, p99 46.4 vs 48.0 ms, RenderGuest
    // backpressure 17 vs 410 ms per 20 s. RenderGuest/Main keep Big + Prime at -8.
    {Kind::kPrime, -10, true},          // backend
    // 56: Big without Prime (Prime is the backend's), same nice as RenderGuest: it does the
    // capture RenderGuest did (~3 us per command); RenderGuest waits for it only when the ring
    // is full or a frame ahead.
    {Kind::kBig, -8, true},             // capture
}};

// Таблица версии 26.10 (для сравнения A/B), кроме хостового аудио — его не трогаем и здесь.
constexpr std::array<Rule, kThreadIdCount> kClusters = {{
    {Kind::kBigPrime, -6, true},        // render_guest
    {Kind::kBigPrime, -8, true},        // recorder
    {Kind::kBig, -4, true},             // render_work
    {Kind::kBigPrime, -6, true},        // main_xthread
    {Kind::kBig, -2, false},            // sdl_thread
    {Kind::kLittle, kKeepNice, false},  // sdl_helpers
    {Kind::kBig, -8, true},             // audio_guest
    {Kind::kNone, kKeepNice, false},    // host_audio
    {Kind::kWorkers, -16, true},        // xma_decoder
    {Kind::kWorkers, -16, true},        // audio_worker
    {Kind::kWorkers, 0, false},         // rage_stream
    {Kind::kLittle, kKeepNice, false},  // rage_fileio
    {Kind::kWorkers, -2, false},        // kernel_io
    {Kind::kLittleBig, -2, false},      // rage_jobs
    {Kind::kLittleBig, -2, false},      // rage_physics
    {Kind::kWorkers, 0, false},         // rage_other
    {Kind::kWorkers, 0, false},         // rage_sim (as rage_other in this table)
    {Kind::kWorkers, 0, false},         // cmd_retirer
    {Kind::kWorkers, -2, false},        // recorder_helpers
    {Kind::kLittle, kKeepNice, false},  // sdl_activity
    {Kind::kLittle, kKeepNice, false},  // background
    {Kind::kBigPrime, -8, true},        // backend (as the recorder in this table)
    {Kind::kBigPrime, -6, true},        // capture (as render_guest in this table)
}};

// Только приоритеты: nice и удержание как в «полосах», маски не ставятся.
constexpr std::array<Rule, kThreadIdCount> WithoutMasks(const std::array<Rule, kThreadIdCount>& from) {
  std::array<Rule, kThreadIdCount> out{};
  for (size_t i = 0; i < from.size(); ++i) out[i] = Rule{Kind::kNone, from[i].nice, from[i].sticky};
  return out;
}
constexpr std::array<Rule, kThreadIdCount> kPriorityOnly = WithoutMasks(kLanes);

// 29.10, для A/B («lanes2»): «полосы» с двумя отличиями по трейсам 27.10 (888, нагрев).
//  - main_xthread: Big без Prime. На 0xF0 он вытеснял RenderGuest (746 раз за 19 с) и
//    рекордер (977) на ядрах 6-7; сам он занят ~32 %.
//  - render_guest: nice -10, как у рекордера. Узкое место кадра — он (81 % занят,
//    ещё ~15 % ждёт ядро), а не рекордер (простаивает 31 %).
// Аудио не трогаем (4-5, -16): с -8 на Little раньше была тишина в звуке.
constexpr std::array<Rule, kThreadIdCount> MakeLanes2() {
  std::array<Rule, kThreadIdCount> out = kLanes;
  out[kMainXThread] = Rule{Kind::kBig, -8, true};
  out[kRenderGuest] = Rule{Kind::kRecorderLane, -10, true};
  // 31.10 (трейс 20:45): RenderGuest теперь ~1 раз за кадр ждёт Main XThread
  // (~3.3 мс/кадр), а Main ждёт задачи: три RageJobWork стоят в очереди на ядро
  // по ~4.7 с из 19 (wake-latency ~2.8 с каждый). На тех же Little-ядрах
  // GtaCmdRetirer занимает целое ядро (90 %) с nice 0. Ретайрер — фоновая
  // работа без сроков: ему nice 5, задачам игры — -6 (как render_work).
  out[kRageJobs] = Rule{Kind::kJobsLane, -6, false};
  out[kRagePhysics] = Rule{Kind::kJobsLane, -6, false};
  // 32: ретайрер насыщен (97% Little-ядра), nice +5 его душит; возвращаем 0
  out[kCmdRetirer] = Rule{Kind::kLittle, 0, false};
  return out;
}
constexpr std::array<Rule, kThreadIdCount> kLanes2 = MakeLanes2();

enum class Layout : uint8_t { kLanes, kClusters, kPriority, kOff, kLanes2 };

Layout ParseLayout(const std::string& value) {
  if (value == "lanes2") return Layout::kLanes2;
  if (value == "clusters") return Layout::kClusters;
  if (value == "priority" || value == "nice") return Layout::kPriority;
  if (value == "off" || value == "none") return Layout::kOff;
  return Layout::kLanes;
}

const char* LayoutName(Layout layout) {
  switch (layout) {
    case Layout::kLanes2: return "lanes2";
    case Layout::kClusters: return "clusters";
    case Layout::kPriority: return "priority";
    case Layout::kOff: return "off";
    default: return "lanes";
  }
}

const Rule& RuleFor(Layout layout, ThreadId id) {
  switch (layout) {
    case Layout::kLanes2: return kLanes2[id];
    case Layout::kClusters: return kClusters[id];
    case Layout::kPriority: return kPriorityOnly[id];
    default: return kLanes[id];
  }
}

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool Starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

ThreadId Classify(const std::string& name, pid_t tid, pid_t primary_recorder, pid_t primary_sdl) {
  if (Has(name, "Track") || Has(name, "SDLAudio")) return kHostAudio;
  if (name == "AudioGuest") return kAudioGuest;
  if (Has(name, "XMA")) return kXmaDecoder;
  if (Has(name, "Audio")) return kAudioWorker;
  // 46/47: GtaBackend (gta4_native_backend_thread) is the render worker that also records
  // the frame: its own id "backend" (Prime only by default). Threads it starts inherit
  // its name (NativeVertexCacheOptimizer) and stay helpers, as under GtaRecorder.
  if (name == "GtaRecorder") return tid == primary_recorder ? kRecorder : kRecorderHelpers;
  if (name == "GtaBackend") return tid == primary_recorder ? kBackend : kRecorderHelpers;
  if (name == "RenderGuest") return kRenderGuest;
  if (name == "GtaCapture") return kCapture;  // 56
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
  if (name == "Rage_1BA088") return kRageSim;  // 41
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
    if (id == kIds[i]) return i;
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

uint32_t EffectiveMask(const Placement& p, Layout layout, ThreadId id, const CpuTopology& topo) {
  if (p.manual && p.mask[id]) return p.mask[id];
  return layout == Layout::kOff ? 0 : AutoMask(RuleFor(layout, id).kind, topo);
}

void LogPlacement(const Placement& p, Layout layout, const CpuTopology& topo, const char* source) {
  std::string list;
  for (int i = 0; i < kThreadIdCount; ++i) {
    const uint32_t m = EffectiveMask(p, layout, ThreadId(i), topo);
    const Rule& rule = RuleFor(layout, ThreadId(i));
    char item[80];
    if (!m && (rule.nice == kKeepNice || layout == Layout::kOff)) {
      std::snprintf(item, sizeof(item), " %s=-", kIds[i]);
    } else if (!m) {
      std::snprintf(item, sizeof(item), " %s=-/%d", kIds[i], rule.nice);
    } else if (rule.nice == kKeepNice || layout == Layout::kOff) {
      std::snprintf(item, sizeof(item), " %s=0x%X%s", kIds[i], m, p.manual && p.mask[i] ? "*" : "");
    } else {
      std::snprintf(item, sizeof(item), " %s=0x%X/%d%s", kIds[i], m, rule.nice,
                    p.manual && p.mask[i] ? "*" : "");
    }
    list += item;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: layout=%s mode=%s (source: %s; * = manual):%s",
                      LayoutName(layout), p.manual ? "manual" : "auto", source, list.c_str());
  const ThreadId heavy[] = {kRenderGuest, kRecorder, kRenderWork, kMainXThread, kSdlThread, kAudioGuest,
                            kBackend, kCapture};
  for (ThreadId id : heavy) {
    const uint32_t m = EffectiveMask(p, layout, id, topo);
    if (m && !(m & ~topo.little))
      __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: note: %s only on little cores", kIds[id]);
    if (m && __builtin_popcount(m) == 1)
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "Affinity: note: %s limited to one core (0x%X): core parking and throttling "
                          "will stall it; prefer two or more cores",
                          kIds[id], m);
  }
  for (size_t a = 0; a < std::size(heavy); ++a) {
    for (size_t b = a + 1; b < std::size(heavy); ++b) {
      const uint32_t ma = EffectiveMask(p, layout, heavy[a], topo);
      const uint32_t mb = EffectiveMask(p, layout, heavy[b], topo);
      if (ma && mb && __builtin_popcount(ma | mb) == 1)
        __android_log_print(ANDROID_LOG_WARN, kTag, "Affinity: note: %s and %s share one core (0x%X)",
                            kIds[heavy[a]], kIds[heavy[b]], ma | mb);
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

// 56: the primary recorder / SDL thread is the one that started first, and it stays primary
// while it exists. The old rule ("the smallest tid") broke when tids wrapped at pid_max:
// a thread that GtaBackend created later (it inherits the name "GtaBackend": compile helpers,
// the Vulkan driver's threads) got tid 434 against the backend's 32684, became "primary", and
// the real backend went to recorder_helpers (Little 0xF, nice 10): 9 FPS (888, batch 55).
// starttime: field 22 of /proc/self/task/<tid>/stat, in clock ticks since boot. Children start
// no earlier than their creator; within one tick the creator's tid precedes the child's in
// allocation order, which wraps at pid_max.
unsigned long long ThreadStartTicks(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  FILE* file = std::fopen(path, "r");
  if (!file) return ~0ull;
  char buf[512] = {};
  const size_t len = std::fread(buf, 1, sizeof(buf) - 1, file);
  std::fclose(file);
  buf[len] = '\0';
  // comm may contain spaces and parentheses ("Main XThread (F"): fields follow the last ')'.
  const char* p = std::strrchr(buf, ')');
  if (!p) return ~0ull;
  ++p;
  // Field 3 (state) is the first after ')'; starttime is field 22.
  for (int field = 3; field < 22; ++field) {
    while (*p == ' ') ++p;
    while (*p && *p != ' ') ++p;
    if (!*p) return ~0ull;
  }
  while (*p == ' ') ++p;
  char* end = nullptr;
  const unsigned long long ticks = std::strtoull(p, &end, 10);
  return end == p ? ~0ull : ticks;
}

pid_t PidMax() {
  static const pid_t value = [] {
    pid_t v = 32768;
    if (FILE* f = std::fopen("/proc/sys/kernel/pid_max", "r")) {
      int read = 0;
      if (std::fscanf(f, "%d", &read) == 1 && read > 1) v = read;
      std::fclose(f);
    }
    return v;
  }();
  return value;
}

// a was allocated before b, given tids allocated in increasing order modulo pid_max and close
// together (the same clock tick).
bool TidAllocatedBefore(pid_t a, pid_t b) {
  const long long max = PidMax();
  const long long distance = ((static_cast<long long>(b) - a) % max + max) % max;
  return distance != 0 && distance < max / 2;
}

struct PrimaryThread {
  pid_t tid = 0;
  unsigned long long start = ~0ull;
  const char* what = "";

  // Called once per full scan with every candidate; Commit() keeps the current primary while
  // it is still among them, otherwise takes the first-started candidate.
  void Begin() {
    present_ = false;
    best_tid_ = 0;
    best_start_ = ~0ull;
  }
  void Offer(pid_t candidate) {
    if (candidate == tid) {
      present_ = true;
      return;
    }
    const unsigned long long candidate_start = ThreadStartTicks(candidate);
    if (!best_tid_ || candidate_start < best_start_ ||
        (candidate_start == best_start_ && TidAllocatedBefore(candidate, best_tid_))) {
      best_tid_ = candidate;
      best_start_ = candidate_start;
    }
  }
  void Commit() {
    if (present_ || !best_tid_) {
      if (!present_ && tid) tid = 0;  // gone, and nobody with that name left
      return;
    }
    if (tid)
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "Affinity: primary %s tid %d is gone, new primary tid %d", what, tid,
                          best_tid_);
    tid = best_tid_;
    start = best_start_;
  }

 private:
  bool present_ = false;
  pid_t best_tid_ = 0;
  unsigned long long best_start_ = ~0ull;
};

struct Applied {
  std::string name;
  uint32_t mask = 0;
  uint32_t effective = 0;
  uint32_t generation = 0;
  uint32_t restored = 0;
  bool nice_ok = false;
  ThreadId id = kUnknown;
  bool hold = false;
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
    if (env && *env && std::strcmp(env, "auto") != 0) {
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
  std::string spec_text, layout_text;
  Placement placement;
  Layout layout = Layout::kLanes;
  bool spec_parsed = false;
  uint32_t generation = 0;
  // 56: kept across scans (see PrimaryThread).
  PrimaryThread recorder_primary, sdl_primary;
  recorder_primary.what = "recorder/backend";
  sdl_primary.what = "SDLThread";
  const auto started = std::chrono::steady_clock::now();
  bool summary_logged = false;
  uint32_t tick = 0;

  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!REXCVAR_GET(gta4_cluster_pinning)) continue;
    ++tick;

    // Быстрый тик (каждые 100 мс): проверяем и мгновенно восстанавливаем только уже
    // известные sticky-потоки через прямой syscall без чтения /proc/self/task (76 файлов).
    const bool full_scan = (tick % 4 == 1) &&
                           (!summary_logged || (tick % 20 == 1));
    if (!full_scan && spec_parsed && layout != Layout::kOff) {
      for (auto& [tid, state] : applied) {
        if (!state.hold || state.id >= kThreadIdCount) continue;
        const bool manual = placement.manual && placement.mask[state.id] != 0;
        const Rule& rule = RuleFor(layout, state.id);
        const uint32_t mask = manual ? placement.mask[state.id] : AutoMask(rule.kind, topo);
        const int nice = rule.nice;
        bool changed_outside = false;
        uint32_t outside_mask = 0;
        int outside_nice = kKeepNice;
        if (mask && state.effective) {
          const uint32_t current = GetAffinityMask(tid);
          if (current != 0 && current != state.effective) {
            changed_outside = true;
            outside_mask = current;
          }
        }
        if (nice != kKeepNice && state.nice_ok) {
          errno = 0;
          const int prio = getpriority(PRIO_PROCESS, id_t(tid));
          if (errno == 0 && prio != nice) {
            changed_outside = true;
            outside_nice = prio;
          }
        }
        if (!changed_outside) continue;
        if (mask) {
          if (SetAffinityMask(tid, mask)) state.effective = GetAffinityMask(tid);
        }
        if (nice != kKeepNice) state.nice_ok = setpriority(PRIO_PROCESS, id_t(tid), nice) == 0;
        (void)outside_mask;
        (void)outside_nice;
        ++state.restored;
      }
      continue;
    }

    const char* source_name = "";
    const std::string spec = source.Current(source_name);
    const std::string layout_now = Trim(REXCVAR_GET(android_thread_layout));
    if (!spec_parsed || spec != spec_text || layout_now != layout_text) {
      spec_text = spec;
      layout_text = layout_now;
      layout = ParseLayout(layout_now);
      placement = ParseSpec(spec, topo);
      spec_parsed = true;
      LogPlacement(placement, layout, topo, source_name);
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
    recorder_primary.Begin();
    sdl_primary.Begin();
    while (dirent* entry = readdir(dir)) {
      const pid_t tid = pid_t(std::atoi(entry->d_name));
      if (tid <= 0) continue;
      std::string name = GetThreadName(tid);
      if (name.empty()) continue;
      if (name == "GtaRecorder" || name == "GtaBackend") recorder_primary.Offer(tid);
      if (name == "SDLThread") sdl_primary.Offer(tid);
      seen.push_back({tid, std::move(name)});
    }
    closedir(dir);
    recorder_primary.Commit();
    sdl_primary.Commit();
    const pid_t primary_recorder = recorder_primary.tid, primary_sdl = sdl_primary.tid;

    ++generation;
    for (const Seen& s : seen) {
      const ThreadId id = Classify(s.name, s.tid, primary_recorder, primary_sdl);
      Applied& state = applied[s.tid];
      state.generation = generation;
      state.id = id;
      state.hold = false;
      if (id == kUnknown) {
        if (state.name != s.name && unknown_logged.size() < 64 && unknown_logged.insert(s.name).second)
          __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: unclassified thread '%s' (tid %d) left as is",
                              s.name.c_str(), s.tid);
        state.name = s.name;
        state.mask = 0;
        continue;
      }
      const bool manual = placement.manual && placement.mask[id] != 0;
      const Rule& rule = RuleFor(layout, id);
      // off: трогаем только то, что задано вручную (и без nice).
      const uint32_t mask = manual ? placement.mask[id]
                                   : (layout == Layout::kOff ? 0 : AutoMask(rule.kind, topo));
      const int nice = layout == Layout::kOff ? kKeepNice : rule.nice;
      if (!mask && state.effective) {
        // Маску ставили мы, а теперь она не нужна (раскладку сменили на лету): снимаем.
        const bool released = SetAffinityMask(s.tid, topo.all());
        __android_log_print(released ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
                            "Affinity: %s '%s' (tid %d): mask 0x%X released%s", kIds[id],
                            s.name.c_str(), s.tid, state.effective, released ? "" : " FAILED");
        state.effective = 0;
        state.mask = 0;
      }
      if (!mask && nice == kKeepNice) {  // kNone: поток не трогаем вообще
        state.name = s.name;
        state.mask = 0;
        continue;
      }
      const bool fresh = state.name != s.name || state.mask != mask;
      const bool hold = manual || rule.sticky;
      state.hold = hold;
      if (!fresh && !hold) continue;

      bool changed_outside = false;
      uint32_t outside_mask = 0;
      int outside_nice = kKeepNice;
      if (!fresh && (state.effective || !mask)) {
        if (mask) {
          const uint32_t current = GetAffinityMask(s.tid);
          if (current != 0 && current != state.effective) {
            changed_outside = true;
            outside_mask = current;
          }
        }
        if (nice != kKeepNice && state.nice_ok) {
          errno = 0;
          const int prio = getpriority(PRIO_PROCESS, id_t(s.tid));
          if (errno == 0 && prio != nice) {
            changed_outside = true;
            outside_nice = prio;
          }
        }
        if (!changed_outside) continue;
      }

      bool ok = true;
      if (mask) {
        ok = SetAffinityMask(s.tid, mask);
        state.effective = ok ? GetAffinityMask(s.tid) : 0;
      }
      if (nice != kKeepNice) state.nice_ok = setpriority(PRIO_PROCESS, id_t(s.tid), nice) == 0;
      if (fresh) {
        char mask_text[16] = "keep";
        if (mask) std::snprintf(mask_text, sizeof(mask_text), "0x%X", mask);
        __android_log_print(ok && (nice == kKeepNice || state.nice_ok) ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
                            kTag, "Affinity: %s '%s' (tid %d) -> %s nice %s%d%s%s", kIds[id],
                            s.name.c_str(), s.tid, mask_text, nice == kKeepNice ? "keep " : "",
                            nice == kKeepNice ? 0 : nice, manual ? " [manual]" : " [auto]",
                            !ok ? " FAILED" : (nice != kKeepNice && !state.nice_ok ? " (nice FAILED)" : ""));
      } else if (changed_outside && IsPowerOfTen(++state.restored)) {
        // Что именно поменяли снаружи — чтобы отличить cpuset/OEM (маска) от чужого setpriority (nice).
        char what[96] = {};
        if (outside_mask && outside_nice != kKeepNice)
          std::snprintf(what, sizeof(what), "mask 0x%X and nice %d", outside_mask, outside_nice);
        else if (outside_mask)
          std::snprintf(what, sizeof(what), "mask 0x%X", outside_mask);
        else
          std::snprintf(what, sizeof(what), "nice %d", outside_nice);
        char kept[32];
        if (mask && nice != kKeepNice) std::snprintf(kept, sizeof(kept), "0x%X/%d", mask, nice);
        else if (mask) std::snprintf(kept, sizeof(kept), "0x%X", mask);
        else std::snprintf(kept, sizeof(kept), "nice %d", nice);
        const std::string cpuset = ReadSmallFile(s.tid, "cpuset");
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "Affinity: %s '%s' (tid %d): found %s (cpuset=%s), restored %s (%u times)", kIds[id],
                            s.name.c_str(), s.tid, what, cpuset.c_str(), kept, state.restored);
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
        const uint32_t m = EffectiveMask(placement, layout, ThreadId(i), topo);
        if (m) std::snprintf(item, sizeof(item), " %s x%d=0x%X", kIds[i], count[i], m);
        else std::snprintf(item, sizeof(item), " %s x%d=-", kIds[i], count[i]);
        list += item;
      }
      __android_log_print(ANDROID_LOG_INFO, kTag, "Affinity: summary layout=%s (%zu threads):%s unclassified x%d",
                          LayoutName(layout), seen.size(), list.c_str(), unknown);
    }
  }
}

const bool g_pinning_started = [] {
  std::thread(PinningLoop).detach();
  return true;
}();

}  // namespace
