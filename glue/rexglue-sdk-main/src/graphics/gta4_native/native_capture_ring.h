// Package 56: deferred capture, step 4.1 of разбор 55 (gta4_native_capture_thread).
//
// 888, batch 54 P1: RenderGuest is the frame limiter (27.3 ms busy of a 30.2 ms frame). 14.0 ms
// of it is the submission path, gpu_pass::Submit inclusive: 11.6 ms in librexgpu, the rest
// libc / libc++ / TLS. Its cost per command does not follow the core clock (+20 % clock,
// -4 % time): it waits for memory (slot lines from DRAM, the capture cache tables, resource
// lines shared with the backend). Packages 52-54 only moved that wait from one line to the
// next. This removes the whole capture from RenderGuest instead.
//
// RenderGuest (and any other submitting thread) copies into this ring only what the capture
// reads from memory that changes after the submit returns:
//   - the command bytes (they live on the hook's stack);
//   - for draws and clears, the lines of the guest device that the capture reads
//     (kDeviceReadSet, 146 lines of 64 bytes), and of those only the lines that changed since
//     the previous record for that device;
//   - the DrawPrimitiveUp payload (it lives in the title's transient vertex memory).
// GtaCapture replays every record through the unchanged capture code, which reads the device
// from GtaCapture's own image of it instead of guest memory.
//
// Guest objects (buffer / texture / surface headers and their data) are still read by the
// capture, now up to about a frame later. The title changes them only through Lock and frees
// them through Release. Both hooks first wait until every record that referenced the resource
// has been captured (PendingRefs + Ring::WaitDone), as D3D's Lock waits for the GPU on the
// console.
//
// Self-contained (std only) so that it can be tested on its own (проверка/test56_ring.cpp).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace rex::graphics::gta4_native::capture {

inline constexpr uint32_t kLineBytes = 64;

// Lines of the guest device read by ValidateAndCopyCommand, DecodeFixedFunctionState and
// CaptureBulbSource (offsets checked against graphics_system.cpp, package 54):
struct LineRange {
  uint16_t first;
  uint16_t count;
};
inline constexpr LineRange kDeviceReadSet[] = {
    {0x480 / 64, 10},   // texture fetch constants: kTextureFetchBase 0x480 + 26 x 24 -> 0x6F0
    {0x780 / 64, 120},  // float constants: vertex 0x780..0x1780, pixel 0x1780..0x2580
    {0x2780 / 64, 1},   // boolean constants 0x2780 / 0x2790
    {0x2800 / 64, 10},  // fixed state 10272 (clip planes) .. 10847 (back depth bias)
    {0x2E40 / 64, 1},   // blend / scissor / depth / stencil enables 11844..11875
    {0x3080 / 64, 4},   // index 12428, targets 12432..12451, streams 12452..12519,
                        // texture handles 12536..12639, viewport 12640..12663
};
inline constexpr uint32_t kDeviceReadLines = [] {
  uint32_t n = 0;
  for (const auto& range : kDeviceReadSet) n += range.count;
  return n;
}();
static_assert(kDeviceReadLines == 146);
// One past the last byte of the read set.
inline constexpr uint32_t kDeviceReadEnd = (0x3080 / 64 + 4) * kLineBytes;  // 12672

// Words of the device that hold the resources a draw references (index, render targets,
// depth, 17 streams, 4 words between the streams and the textures, 26 texture handles):
// 12428..12639, big-endian guest handles.
inline constexpr uint32_t kDeviceHandleWordsOffset = 12428;
inline constexpr uint32_t kDeviceHandleWords = (12640 - 12428) / 4;  // 53

inline bool LineDiffers(const uint8_t* a, const uint8_t* b) {
#if defined(__aarch64__)
  const uint8x16_t x0 = veorq_u8(vld1q_u8(a), vld1q_u8(b));
  const uint8x16_t x1 = veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16));
  const uint8x16_t x2 = veorq_u8(vld1q_u8(a + 32), vld1q_u8(b + 32));
  const uint8x16_t x3 = veorq_u8(vld1q_u8(a + 48), vld1q_u8(b + 48));
  const uint8x16_t any = vorrq_u8(vorrq_u8(x0, x1), vorrq_u8(x2, x3));
  return vmaxvq_u32(vreinterpretq_u32_u8(any)) != 0;
#else
  return std::memcmp(a, b, kLineBytes) != 0;
#endif
}

// Copies the read-set lines of `device` that differ from `shadow` (all of them when `full`)
// into `shadow` and, packed, into `out_lines` (64 bytes each) with their line numbers in
// `out_index`. Returns how many.
inline uint32_t DiffDeviceReadSet(const uint8_t* device, uint8_t* shadow, bool full,
                                  uint8_t* out_lines, uint16_t* out_index) {
  uint32_t n = 0;
  for (const auto& range : kDeviceReadSet) {
    for (uint32_t line = range.first; line < uint32_t(range.first + range.count); ++line) {
      const uint8_t* source = device + line * kLineBytes;
      uint8_t* copy = shadow + line * kLineBytes;
      if (full || LineDiffers(source, copy)) {
        std::memcpy(copy, source, kLineBytes);
        std::memcpy(out_lines + size_t(n) * kLineBytes, source, kLineBytes);
        out_index[n++] = uint16_t(line);
      }
    }
  }
  return n;
}

// 57: the same, but the 120 float-constant lines are compared only where the title's dirty
// words say a group changed. Batch 56 Q1: the full compare of 146 lines cost RenderGuest
// 1.97 ms per frame, and on average 4.2 lines per draw differed.
// The dirty words are the guest D3D's own (device + 0..39, consumed by the hook at every draw,
// command.dirty_state): float constants are 64-byte groups, MSB first — vertex group g is bit
// (63 - g) of word 0, pixel group g bit (63 - g) of word 1 (NativeConstantDirtyLayout). Nothing
// else writes the constant banks (no hook stores there), so a group without its bit is
// unchanged; the other 26 lines are always compared. `full`: everything, as before.
inline constexpr uint32_t kVertexConstantFirstLine = 0x780 / 64;  // 30, 64 groups
inline constexpr uint32_t kPixelConstantFirstLine = 0x1780 / 64;  // 94, 56 groups
inline uint32_t DiffDeviceReadSetDirty(const uint8_t* device, uint8_t* shadow, bool full,
                                       const uint64_t* dirty_words, uint8_t* out_lines,
                                       uint16_t* out_index) {
  if (full || !dirty_words) return DiffDeviceReadSet(device, shadow, true, out_lines, out_index);
  uint32_t n = 0;
  auto line = [&](uint32_t index) {
    const uint8_t* source = device + index * kLineBytes;
    uint8_t* copy = shadow + index * kLineBytes;
    if (LineDiffers(source, copy)) {
      std::memcpy(copy, source, kLineBytes);
      std::memcpy(out_lines + size_t(n) * kLineBytes, source, kLineBytes);
      out_index[n++] = uint16_t(index);
    }
  };
  auto groups = [&](uint64_t bits, uint32_t first_line, uint32_t count) {
    while (bits) {
      const uint32_t group = uint32_t(__builtin_clzll(bits));  // MSB = group 0
      bits &= ~(uint64_t{1} << (63 - group));
      if (group < count) line(first_line + group);
    }
  };
  for (const auto& range : kDeviceReadSet) {
    if (range.first == kVertexConstantFirstLine) {  // the constant banks
      groups(dirty_words[0], kVertexConstantFirstLine, 64);
      groups(dirty_words[1], kPixelConstantFirstLine, 56);
      continue;
    }
    for (uint32_t index = range.first; index < uint32_t(range.first + range.count); ++index)
      line(index);
  }
  return n;
}

inline void ApplyDeviceLines(uint8_t* image, const uint8_t* lines, const uint16_t* index,
                             uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    std::memcpy(image + size_t(index[i]) * kLineBytes, lines + size_t(i) * kLineBytes,
                kLineBytes);
}

// Images of guest devices, one per device address (the title uses one). Producer side: the
// state last sent (the shadow); GtaCapture side: the state rebuilt from the records.
class DeviceImages {
 public:
  explicit DeviceImages(size_t image_bytes)
      : image_bytes_((std::max<size_t>(image_bytes, kDeviceReadEnd) + 63) & ~size_t(63)) {}
  // The image of `device`; `created` when it is new (or was invalidated): the caller must
  // then send / receive the whole read set.
  uint8_t* Get(uint32_t device, bool& created) {
    created = false;
    Slot* victim = &slots_[0];
    for (Slot& slot : slots_) {
      if (slot.bytes && slot.device == device) {
        slot.last_use = ++clock_;
        if (!slot.valid) {
          created = true;
          slot.valid = true;
        }
        return slot.bytes;
      }
      if (!slot.bytes || slot.last_use < victim->last_use) victim = &slot;
    }
    if (!victim->bytes) {
      victim->storage.reset(new uint8_t[image_bytes_ + 63]());
      victim->bytes = reinterpret_cast<uint8_t*>(
          (reinterpret_cast<uintptr_t>(victim->storage.get()) + 63) & ~uintptr_t(63));
    } else {
      std::memset(victim->bytes, 0, image_bytes_);
    }
    victim->device = device;
    victim->valid = true;
    victim->last_use = ++clock_;
    created = true;
    return victim->bytes;
  }
  void Invalidate(uint32_t device) {
    for (Slot& slot : slots_)
      if (slot.bytes && slot.device == device) slot.valid = false;
  }
  void InvalidateAll() {
    for (Slot& slot : slots_) slot.valid = false;
  }
  size_t image_bytes() const { return image_bytes_; }

 private:
  struct Slot {
    std::unique_ptr<uint8_t[]> storage;
    uint8_t* bytes = nullptr;
    uint32_t device = 0;
    bool valid = false;
    uint64_t last_use = 0;
  };
  size_t image_bytes_;
  Slot slots_[4];
  uint64_t clock_ = 0;
};

// Which record last referenced a resource: a direct-mapped table of the low 32 bits of record
// sequences, by handle. A collision only makes a barrier wait longer than necessary.
class PendingRefs {
 public:
  static constexpr uint32_t kBits = 12;
  PendingRefs() : sequence_(new std::atomic<uint32_t>[size_t(1) << kBits]) {
    for (size_t i = 0; i < (size_t(1) << kBits); ++i)
      sequence_[i].store(0, std::memory_order_relaxed);
  }
  static uint32_t SlotOf(uint32_t handle) { return ((handle >> 4) * 2654435761u) >> (32 - kBits); }
  void Mark(uint32_t handle, uint32_t sequence) {
    if (handle) sequence_[SlotOf(handle)].store(sequence, std::memory_order_release);
  }
  uint32_t Get(uint32_t handle) const {
    return handle ? sequence_[SlotOf(handle)].load(std::memory_order_acquire) : 0;
  }

 private:
  std::unique_ptr<std::atomic<uint32_t>[]> sequence_;
};

// 57: render-target / depth surface objects as the capture reads them (11 words,
// CaptureSurfaceDescriptor). Batch 56 V: the title rewrites word 7 (EDRAM address) of a bound
// surface object in place while draws that used the old value are still in the ring (8 595
// times in 386 000 checked draws, always RT0 word 7). So a record carries the surface words of
// its draw, only those that changed since the previous record (per slot).
inline constexpr uint32_t kSurfaceSlots = 5;  // RT0..RT3, depth
struct SurfaceWords {
  uint32_t slot = 0;
  uint32_t handle = 0;
  uint32_t words[11]{};
  uint32_t reserved[3]{};
};
static_assert(sizeof(SurfaceWords) == 64);

enum class RecordKind : uint16_t { kPad = 0, kTitle = 1 };
enum RecordFlags : uint32_t {
  kRecordOrigin = 1u << 0,   // a GpuPassOrigin (32 bytes) follows the header
  kRecordPresent = 1u << 1,  // the command is a Present
  kRecordVerify = 1u << 2,   // a verification block follows the line index
  // bits 8..11: SurfaceWords entries after the line index (57)
};
inline constexpr uint32_t kRecordSurfaceShift = 8;
inline uint32_t RecordSurfaces(uint32_t flags) { return (flags >> kRecordSurfaceShift) & 15u; }

// Records are multiples of 32 bytes:
// [header][origin][command][up payload][lines][index][surfaces][verify], every part padded to 16.
struct alignas(16) RecordHeader {
  uint32_t bytes;
  RecordKind kind;
  uint16_t lines;
  uint32_t command_size;
  uint32_t device;
  uint32_t up_size;
  uint32_t flags;
  uint64_t sequence;
};
static_assert(sizeof(RecordHeader) == 32);

inline constexpr size_t Pad16(size_t bytes) { return (bytes + 15) & ~size_t(15); }
inline constexpr size_t Pad32(size_t bytes) { return (bytes + 31) & ~size_t(31); }

struct RecordLayout {
  size_t origin = 0, command = 0, up = 0, lines = 0, index = 0, surfaces = 0, verify = 0, end = 0;
};
inline RecordLayout LayoutRecord(bool origin, size_t command_size, size_t up_size,
                                 uint32_t lines, size_t verify_bytes, uint32_t surfaces = 0) {
  RecordLayout layout;
  size_t at = sizeof(RecordHeader);
  layout.origin = at;
  at += origin ? 32 : 0;
  layout.command = at;
  at += Pad16(command_size);
  layout.up = at;
  at += Pad16(up_size);
  layout.lines = at;
  at += size_t(lines) * kLineBytes;
  layout.index = at;
  at += Pad16(size_t(lines) * sizeof(uint16_t));
  layout.surfaces = at;
  at += size_t(surfaces) * sizeof(SurfaceWords);
  layout.verify = at;
  at += Pad16(verify_bytes);
  layout.end = Pad32(at);
  return layout;
}

inline void CpuRelax() {
#if defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

struct RingStats {
  std::atomic<uint64_t> space_waits{0}, space_wait_ns{0};
  std::atomic<uint64_t> consumer_sleeps{0}, consumer_wakes{0}, notifies{0};
  std::atomic<uint64_t> progress_waits{0}, progress_wait_ns{0}, progress_wait_max_ns{0};
};

// Byte ring: any number of producers that serialize themselves (the caller's lock), one
// consumer. Every wait is bounded (20 ms slices) and ends when the ring stops.
class Ring {
 public:
  using Clock = std::chrono::steady_clock;

  explicit Ring(size_t capacity) {
    size_t c = 64 * 1024;
    while (c < capacity) c <<= 1;
    capacity_ = c;
    mask_ = c - 1;
    storage_.reset(new uint8_t[capacity_ + 63]);
    data_ = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage_.get()) + 63) &
                                       ~uintptr_t(63));
  }
  size_t capacity() const { return capacity_; }
  size_t max_record() const { return capacity_ / 4; }
  void set_wake_records(uint32_t records) { wake_records_ = std::max<uint32_t>(1, records); }
  void set_spin_ns(uint64_t producer_ns, uint64_t consumer_ns) {
    producer_spin_ns_ = producer_ns;
    consumer_spin_ns_ = consumer_ns;
  }
  RingStats& stats() { return stats_; }

  // ---- producer (callers serialize) ----
  // Contiguous space for a record of up to `max_bytes` (<= max_record()). nullptr once stopped.
  uint8_t* Reserve(size_t max_bytes) {
    max_bytes = Pad32(max_bytes);
    if (max_bytes > max_record() || stop_.load(std::memory_order_acquire)) return nullptr;
    const size_t position = size_t(write_ & mask_);
    const size_t tail = capacity_ - position;
    const size_t need = tail < max_bytes ? tail + max_bytes : max_bytes;
    if (capacity_ - size_t(write_ - read_.load(std::memory_order_acquire)) < need &&
        !WaitSpace(need))
      return nullptr;
    if (tail < max_bytes) {
      auto* pad = reinterpret_cast<RecordHeader*>(data_ + position);
      pad->bytes = uint32_t(tail);
      pad->kind = RecordKind::kPad;
      write_ += tail;  // published together with the next record
    }
    return data_ + size_t(write_ & mask_);
  }
  // Publishes the record at the reserved space. `urgent` wakes a sleeping consumer at once,
  // otherwise it is woken once wake_records records are pending.
  void Commit(size_t bytes, bool urgent) {
    write_ += Pad32(bytes);
    ++published_records_;
    published_.store(write_, std::memory_order_seq_cst);
    if (consumer_sleeping_.load(std::memory_order_seq_cst)) {
      const uint64_t pending =
          published_records_ - consumed_records_.load(std::memory_order_relaxed);
      // 57: only the producer that clears the flag wakes it. Batch 56: 1169 wakes per frame
      // for 14 sleeps — every record committed while GtaCapture was waking up signalled again,
      // and bionic's pthread_cond_signal always makes the futex syscall (0.67 ms of RG).
      if ((urgent || pending >= wake_records_) &&
          consumer_sleeping_.exchange(false, std::memory_order_seq_cst))
        WakeConsumer();
    }
  }
  size_t used_bytes() const {
    return size_t(published_.load(std::memory_order_relaxed) -
                  read_.load(std::memory_order_relaxed));
  }

  // ---- consumer ----
  const RecordHeader* Front() {
    uint64_t read = read_.load(std::memory_order_relaxed);
    const uint64_t published = published_.load(std::memory_order_acquire);
    while (read != published) {
      const auto* header = reinterpret_cast<const RecordHeader*>(data_ + size_t(read & mask_));
      if (header->kind != RecordKind::kPad) return header;
      read += header->bytes;
      read_.store(read, std::memory_order_seq_cst);
      NotifySpace(read);
    }
    return nullptr;
  }
  // After the record was processed: frees it and publishes its sequence as done.
  void Pop(const RecordHeader* header) {
    const uint64_t sequence = header->sequence;
    const uint64_t bytes = header->bytes;
    consumed_records_.store(consumed_records_.load(std::memory_order_relaxed) + 1,
                            std::memory_order_relaxed);
    done_sequence_.store(sequence, std::memory_order_seq_cst);
    const uint64_t read = read_.load(std::memory_order_relaxed) + bytes;
    read_.store(read, std::memory_order_seq_cst);
    NotifySpace(read);
    NotifyProgress(sequence);
  }
  // Spins, then sleeps for at most `sleep` (or until woken). The caller re-checks Front().
  void WaitForData(std::chrono::microseconds sleep) {
    const auto spin_end = Clock::now() + std::chrono::nanoseconds(consumer_spin_ns_);
    for (uint32_t i = 0;; ++i) {
      if (published_.load(std::memory_order_acquire) != read_.load(std::memory_order_relaxed))
        return;
      if ((i & 63) == 63 && Clock::now() >= spin_end) break;
      CpuRelax();
    }
    std::unique_lock lock(wait_mutex_);
    consumer_sleeping_.store(true, std::memory_order_seq_cst);
    if (published_.load(std::memory_order_seq_cst) == read_.load(std::memory_order_relaxed) &&
        !stop_.load(std::memory_order_acquire)) {
      stats_.consumer_sleeps.fetch_add(1, std::memory_order_relaxed);
      data_cv_.wait_for(lock, sleep);
    }
    consumer_sleeping_.store(false, std::memory_order_relaxed);
  }

  // ---- anyone ----
  // seq_cst: waiters publish progress_waiters_ and then read this; Pop stores this and then
  // reads progress_waiters_ (both seq_cst, so one of them sees the other).
  uint64_t done_sequence() const { return done_sequence_.load(std::memory_order_seq_cst); }
  // Until the record `sequence` has been processed, or the ring stops. Returns the time waited
  // in ns (0 if it was already done). 57: the waiter registers the sequence it needs, and the
  // consumer signals only when it gets there. Batch 56: GtaCapture signalled on every record
  // while a barrier waited (pthread_cond_broadcast, 2.5 ms per frame on GtaCapture).
  uint64_t WaitDone(uint64_t sequence) {
    if (done_sequence() >= sequence) return 0;
    const auto begin = Clock::now();
    const auto spin_end = begin + std::chrono::nanoseconds(producer_spin_ns_);
    bool done = false;
    for (uint32_t i = 0;; ++i) {
      if (done_sequence() >= sequence) {
        done = true;
        break;
      }
      if ((i & 63) == 63 && Clock::now() >= spin_end) break;
      CpuRelax();
    }
    if (!done) {
      std::unique_lock lock(wait_mutex_);
      progress_waiters_.fetch_add(1, std::memory_order_seq_cst);
      while (!stop_.load(std::memory_order_acquire)) {
        if (sequence < progress_target_.load(std::memory_order_relaxed))
          progress_target_.store(sequence, std::memory_order_seq_cst);
        if (done_sequence() >= sequence) break;
        progress_cv_.wait_for(lock, std::chrono::milliseconds(20));
      }
      progress_waiters_.fetch_sub(1, std::memory_order_relaxed);
    }
    const uint64_t ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    stats_.progress_waits.fetch_add(1, std::memory_order_relaxed);
    stats_.progress_wait_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t max = stats_.progress_wait_max_ns.load(std::memory_order_relaxed);
    while (ns > max && !stats_.progress_wait_max_ns.compare_exchange_weak(
                           max, ns, std::memory_order_relaxed)) {
    }
    return ns;
  }
  void Stop() {
    stop_.store(true, std::memory_order_seq_cst);
    { std::lock_guard lock(wait_mutex_); }
    data_cv_.notify_all();
    space_cv_.notify_all();
    progress_cv_.notify_all();
  }
  bool stopped() const { return stop_.load(std::memory_order_acquire); }

 private:
  bool WaitSpace(size_t need) {
    const auto begin = Clock::now();
    const auto spin_end = begin + std::chrono::nanoseconds(producer_spin_ns_);
    auto has_space = [&] {
      return capacity_ - size_t(write_ - read_.load(std::memory_order_seq_cst)) >= need;
    };
    bool ok = false;
    for (uint32_t i = 0;; ++i) {
      if (has_space()) {
        ok = true;
        break;
      }
      if ((i & 63) == 63 && Clock::now() >= spin_end) break;
      CpuRelax();
    }
    if (!ok) {
      // The consumer may sleep with records pending below wake_records: wake it.
      if (consumer_sleeping_.exchange(false, std::memory_order_seq_cst)) WakeConsumer();
      std::unique_lock lock(wait_mutex_);
      space_waiters_.fetch_add(1, std::memory_order_seq_cst);
      // The read position at which `need` bytes are free (one producer waits at a time).
      space_target_.store(write_ + need - capacity_, std::memory_order_seq_cst);
      while (!(ok = has_space()) && !stop_.load(std::memory_order_acquire))
        space_cv_.wait_for(lock, std::chrono::milliseconds(20));
      space_waiters_.fetch_sub(1, std::memory_order_relaxed);
    }
    stats_.space_waits.fetch_add(1, std::memory_order_relaxed);
    stats_.space_wait_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin)
                     .count()),
        std::memory_order_relaxed);
    return ok;
  }
  void WakeConsumer() {
    { std::lock_guard lock(wait_mutex_); }
    data_cv_.notify_one();
    stats_.consumer_wakes.fetch_add(1, std::memory_order_relaxed);
  }
  // Signals a waiting producer once `read` reaches the position it needs.
  void NotifySpace(uint64_t read) {
    if (!space_waiters_.load(std::memory_order_seq_cst) ||
        read < space_target_.load(std::memory_order_seq_cst))
      return;
    {
      std::lock_guard lock(wait_mutex_);
      space_target_.store(UINT64_MAX, std::memory_order_relaxed);
    }
    space_cv_.notify_all();
    stats_.notifies.fetch_add(1, std::memory_order_relaxed);
  }
  // Signals waiters once `done` reaches the smallest sequence one of them needs; the others
  // register again when they wake.
  void NotifyProgress(uint64_t done) {
    if (!progress_waiters_.load(std::memory_order_seq_cst) ||
        done < progress_target_.load(std::memory_order_seq_cst))
      return;
    {
      std::lock_guard lock(wait_mutex_);
      progress_target_.store(UINT64_MAX, std::memory_order_relaxed);
    }
    progress_cv_.notify_all();
    stats_.notifies.fetch_add(1, std::memory_order_relaxed);
  }

  std::unique_ptr<uint8_t[]> storage_;
  uint8_t* data_ = nullptr;
  size_t capacity_ = 0;
  size_t mask_ = 0;
  uint32_t wake_records_ = 32;
  uint64_t producer_spin_ns_ = 20000;
  uint64_t consumer_spin_ns_ = 20000;
  RingStats stats_;
  // Producer side (callers' lock).
  alignas(64) uint64_t write_ = 0;
  uint64_t published_records_ = 0;
  alignas(64) std::atomic<uint64_t> published_{0};
  // Consumer side.
  alignas(64) std::atomic<uint64_t> read_{0};
  std::atomic<uint64_t> consumed_records_{0};
  std::atomic<uint64_t> done_sequence_{0};
  // Waiting.
  alignas(64) std::mutex wait_mutex_;
  std::condition_variable data_cv_, space_cv_, progress_cv_;
  std::atomic<bool> consumer_sleeping_{false};
  std::atomic<int> space_waiters_{0};
  std::atomic<int> progress_waiters_{0};
  std::atomic<uint64_t> space_target_{UINT64_MAX};
  std::atomic<uint64_t> progress_target_{UINT64_MAX};
  std::atomic<bool> stop_{false};
};

}  // namespace rex::graphics::gta4_native::capture
