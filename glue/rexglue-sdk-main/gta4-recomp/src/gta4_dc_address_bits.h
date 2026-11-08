#pragma once
// 51: which guest addresses hold an entry in gta4_aspect_hooks.cpp's dc_layouts.
//
// FinalizeDc runs for every draw command the game publishes (sub_82146790, ~2 % of Main
// XThread on the 888). Outside UI scopes it only has to drop a stale layout of the same
// address. While any UI layout exists (the HUD, every frame) it used to take dc_mutex and
// erase from an unordered_map of up to 32768 entries for each world command; almost none of
// those addresses has an entry. One bit per 4-byte guest address answers "is there an entry"
// without the lock.
//
// Writers hold dc_mutex (Set / Clear, together with the map change). Readers do not lock:
//   - a clear bit means no entry: the entry for an address is created by the thread that
//     publishes that command, before the command is handed on, so a later publication or
//     execution of the same address is ordered after it;
//   - a set bit means "maybe": the caller takes dc_mutex and looks in the map, as before.
// Memory: a 32 KiB page of bits per MiB of guest addresses that ever held a UI layout,
// allocated on first use and never freed: the object is a global, and game threads may
// still run while static destructors do.
#include <array>
#include <atomic>
#include <cstdint>

namespace gta4::aspect {

class DcAddressBits {
 public:
  DcAddressBits() = default;
  DcAddressBits(const DcAddressBits&) = delete;
  DcAddressBits& operator=(const DcAddressBits&) = delete;

  bool Test(uint32_t address) const {
    const Word* page = pages_[address >> kPageShift].load(std::memory_order_acquire);
    if (!page) return false;
    const uint32_t bit = (address & kPageMask) >> 2;
    return (page[bit >> 6].load(std::memory_order_acquire) >> (bit & 63)) & 1u;
  }
  // Callers hold the writers' lock (dc_mutex).
  void Set(uint32_t address) {
    Word* page = pages_[address >> kPageShift].load(std::memory_order_relaxed);
    if (!page) {
      page = new Word[kWordsPerPage]();
      pages_[address >> kPageShift].store(page, std::memory_order_release);
    }
    const uint32_t bit = (address & kPageMask) >> 2;
    Word& word = page[bit >> 6];
    word.store(word.load(std::memory_order_relaxed) | (uint64_t(1) << (bit & 63)),
               std::memory_order_release);
  }
  void Clear(uint32_t address) {
    Word* page = pages_[address >> kPageShift].load(std::memory_order_relaxed);
    if (!page) return;
    const uint32_t bit = (address & kPageMask) >> 2;
    Word& word = page[bit >> 6];
    word.store(word.load(std::memory_order_relaxed) & ~(uint64_t(1) << (bit & 63)),
               std::memory_order_release);
  }

 private:
  using Word = std::atomic<uint64_t>;
  static constexpr uint32_t kPageShift = 20;  // 1 MiB of guest addresses per page
  static constexpr uint32_t kPageMask = (1u << kPageShift) - 1;
  static constexpr uint32_t kWordsPerPage = (1u << kPageShift) / 4 / 64;  // 4096 = 32 KiB
  std::array<std::atomic<Word*>, (size_t(1) << (32 - kPageShift))> pages_{};
};

}  // namespace gta4::aspect
