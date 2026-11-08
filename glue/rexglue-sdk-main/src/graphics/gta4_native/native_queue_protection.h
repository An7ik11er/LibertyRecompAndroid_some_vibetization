#pragma once
// 49 (шаг 3.2): queue-side texture protection by capture epoch.
//
// Before: every queued command held a reference count per texture generation in
// the producer's staging index and then in the queue index (3 index operations
// per texture per draw on the producer, 1 more on the worker, each under a lock).
// Now every capture frame (epoch) keeps one list of the generations its commands
// use, each generation once (the producer stamps the resource). An epoch protects
// while any of its commands has not been taken by the worker; the current epoch
// always protects. A taken command is protected by the worker's own sets.
//
// Threads: one producer (Add / Count / Roll), one worker (Taken), any readers
// (AppendTo). captured has a single writer (producer), taken a single writer
// (worker): plain stores, no read-modify-write. Lists and the deque: mutex_.
// Ordering readers rely on: the producer adds a command's generations before it
// counts it; the worker marks a command taken only after the command is in its
// batch; readers read the epochs before the worker's batch.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace rex::graphics::gta4_native {

class NativeQueueProtectionEpochs {
 public:
  struct Epoch {
    uint32_t id = 0;
    std::vector<uint64_t> generations;
    alignas(64) std::atomic<uint64_t> captured{0};
    alignas(64) std::atomic<uint64_t> taken{0};
  };

  // Producer. The epoch new commands go into (opened on first use).
  Epoch& Current() {
    if (!current_) Roll();
    return *current_;
  }
  // Producer: a new capture frame. Old epochs whose commands were all taken are
  // recycled.
  void Roll() {
    std::lock_guard lock(mutex_);
    while (!epochs_.empty() && epochs_.front().get() != current_ && Drained(*epochs_.front())) {
      free_.push_back(std::move(epochs_.front()));
      epochs_.pop_front();
    }
    std::unique_ptr<Epoch> epoch;
    if (!free_.empty()) {
      epoch = std::move(free_.back());
      free_.pop_back();
    } else {
      epoch = std::make_unique<Epoch>();
    }
    epoch->generations.clear();
    epoch->captured.store(0, std::memory_order_relaxed);
    epoch->taken.store(0, std::memory_order_relaxed);
    epoch->id = next_id_++;
    if (!epoch->id) epoch->id = next_id_++;  // 0 means "never stamped"
    current_ = epoch.get();
    epochs_.push_back(std::move(epoch));
  }
  // Producer: generations first used in this epoch.
  void Add(Epoch& epoch, const uint64_t* generations, size_t count) {
    if (!count) return;
    std::lock_guard lock(mutex_);
    epoch.generations.insert(epoch.generations.end(), generations, generations + count);
  }
  // Producer, after Add: the command is in the epoch.
  static void Count(Epoch& epoch) {
    epoch.captured.store(epoch.captured.load(std::memory_order_relaxed) + 1,
                         std::memory_order_release);
  }
  // Worker, once the command is in its batch.
  static void Taken(Epoch& epoch) {
    epoch.taken.store(epoch.taken.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }
  // Any thread: the generations of the current epoch and of every epoch with
  // commands not taken yet. Read these before the worker's protection.
  void AppendTo(std::unordered_set<uint64_t>& generations) const {
    std::lock_guard lock(mutex_);
    for (const auto& epoch : epochs_) {
      if (epoch.get() != epochs_.back().get() && Drained(*epoch)) continue;
      generations.insert(epoch->generations.begin(), epoch->generations.end());
    }
  }
  // The queue was cleared: its commands will never be taken.
  void Drain() {
    std::lock_guard lock(mutex_);
    for (const auto& epoch : epochs_)
      epoch->taken.store(epoch->captured.load(std::memory_order_acquire), std::memory_order_release);
  }
  size_t size() const {
    std::lock_guard lock(mutex_);
    return epochs_.size();
  }

 private:
  static bool Drained(const Epoch& epoch) {
    // taken first: a taken count is never ahead of the captured count it saw.
    const uint64_t taken = epoch.taken.load(std::memory_order_acquire);
    return taken >= epoch.captured.load(std::memory_order_acquire);
  }
  mutable std::mutex mutex_;
  std::deque<std::unique_ptr<Epoch>> epochs_;  // oldest first, back: current
  std::vector<std::unique_ptr<Epoch>> free_;
  Epoch* current_ = nullptr;                   // producer
  uint32_t next_id_ = 1;
};

}  // namespace rex::graphics::gta4_native
