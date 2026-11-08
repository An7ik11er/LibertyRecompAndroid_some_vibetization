// Package 54: write-allocate without reading (DC ZVA) for memory whose old contents are dead.
//
// 888, batch 53 (N1 / N2, build 861ba0e2), by instruction address: the PRFM PSTL1KEEP of
// package 53 (PrefetchProducerBuffers / TakeProducerPendingState) stalls RenderGuest for
// 450-490 ns per command, 2.1 ms of its frame, in both modes. It is not the hint that is slow:
// every prefetch needs a miss buffer, and they are all taken by lines coming from DRAM. A slot
// and its buffers come back to RenderGuest 2-3 frames after it wrote them (retired after the
// GPU finished, ~30 MB of commands in flight, 4 MB of L3), so each line it is about to
// overwrite is read from DRAM first. RenderGuest's cycles per command were the same in 52 and
// 53: removing the release barrier and the byte loops only moved this wait elsewhere.
//
// DC ZVA allocates a line in the cache, zeroed, without a linefill: no DRAM read and no miss
// buffer, only the ownership of the line. Used for:
//   - lines 0-4 of a recycled NativeCommand (every field there is reassigned by
//     PrepareForAcquire right after);
//   - the constant delta ranges / payload buffers of the acquired command and the producer's
//     state-command buffer after the swap (all empty: size 0, their contents are dead).
// Only whole 64-byte lines inside the range are zeroed; a partial head or tail line is left
// alone (it may belong to another allocation). Used only when DCZID_EL0 allows DC ZVA and its
// block is 64 bytes; otherwise the 53 prefetch.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::graphics::gta4_native {

// Set once by Gta4NativeGraphicsSystem's constructor (cvar gta4_native_zero_allocate and
// the DCZID check), before any thread uses a command.
inline bool g_native_zero_allocate = false;

// DC ZVA block size in bytes, 0 when the instruction is prohibited at EL0.
inline uint32_t ZeroAllocateBlockBytes() {
#if defined(__aarch64__)
  uint64_t dczid;
  asm volatile("mrs %0, dczid_el0" : "=r"(dczid));
  if (dczid & 16) return 0;  // DZP
  return 4u << (dczid & 15);
#else
  return 0;
#endif
}

// Zeroes, without reading, the whole 64-byte lines inside [data, data + bytes), at most
// max_lines of them. Requires a 64-byte DC ZVA block (g_native_zero_allocate).
inline void ZeroAllocateLines(void* data, size_t bytes, size_t max_lines) {
#if defined(__aarch64__)
  if (!data) return;
  const uintptr_t first = reinterpret_cast<uintptr_t>(data);
  uintptr_t line = (first + 63) & ~uintptr_t(63);
  uintptr_t end = (first + bytes) & ~uintptr_t(63);
  if (end > line + max_lines * 64) end = line + max_lines * 64;
  for (; line < end; line += 64) asm volatile("dc zva, %0" : : "r"(line) : "memory");
#else
  (void)data;
  (void)bytes;
  (void)max_lines;
#endif
}

}  // namespace rex::graphics::gta4_native
