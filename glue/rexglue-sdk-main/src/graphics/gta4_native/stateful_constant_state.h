#ifndef REX_GRAPHICS_GTA4_NATIVE_STATEFUL_CONSTANT_STATE_H_
#define REX_GRAPHICS_GTA4_NATIVE_STATEFUL_CONSTANT_STATE_H_

#include <algorithm>
#include <bit>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "dirty_state_delta.h"
#include "native_snapshot_resource.h"

namespace rex::graphics::gta4_native {

// Payload offsets refer to bytes owned by ConstantPayloadDelta. Destination
// offsets refer to the canonical guest-endian constant block. Commands own
// these bytes, so guest memory may change immediately after queue submission.
struct ConstantDeltaRange {
  uint32_t destination_offset = 0;
  uint32_t payload_offset = 0;
  uint32_t byte_count = 0;

  constexpr bool operator==(const ConstantDeltaRange&) const = default;
};

struct ConstantPayloadDelta {
  std::vector<ConstantDeltaRange> ranges;
  std::vector<uint8_t> payload;
  bool complete_snapshot = false;
  // 52: built by CaptureChangedConstantPayloadDelta: every range differs from the
  // consumer's bank as the producer's shadow saw it, so Apply copies it without the
  // memcmp first. If the shadow was stale (after a rejected command, before the resync)
  // an equal range is just copied again; the result is the same bank.
  bool changed_only = false;

  bool empty() const { return ranges.empty(); }
};

inline bool ValidateConstantPayloadDelta(const ConstantPayloadDelta& delta, size_t block_size,
                                         bool require_complete_snapshot = false) {
  if (require_complete_snapshot && !delta.complete_snapshot) {
    return false;
  }
  if (delta.complete_snapshot &&
      (delta.ranges.size() != 1 || delta.ranges[0].destination_offset != 0 ||
       delta.ranges[0].payload_offset != 0 || delta.ranges[0].byte_count != block_size ||
       delta.payload.size() != block_size)) {
    return false;
  }
  size_t previous_end = 0;
  bool first = true;
  for (const ConstantDeltaRange& range : delta.ranges) {
    if (!range.byte_count || range.destination_offset > block_size ||
        range.byte_count > block_size - range.destination_offset ||
        range.payload_offset > delta.payload.size() ||
        range.byte_count > delta.payload.size() - range.payload_offset) {
      return false;
    }
    if (!first && range.destination_offset < previous_end) {
      return false;
    }
    previous_end = size_t(range.destination_offset) + range.byte_count;
    first = false;
  }
  return true;
}

inline bool CaptureConstantPayloadDelta(std::span<const uint8_t> source,
                                        const DirtyRangeSet& dirty_ranges,
                                        uint32_t bytes_per_element,
                                        ConstantPayloadDelta& result) {
  // 35: back to the 31.10 version (insert per range). The 34 two-pass version had to
  // resize() the cleared payload first, and that zero-fill showed up as memset at ~3 %
  // of RenderGuest on 888 / 855 (more than the insert bookkeeping it replaced).
  // Cleared, not reassigned: recycled commands keep these buffers' capacity.
  result.ranges.clear();
  result.payload.clear();
  result.complete_snapshot = false;
  result.changed_only = false;  // 52
  if (!bytes_per_element) {
    return false;
  }
  // One allocation per vector: payload growth by insert reallocated several
  // times per draw on the producer thread.
  {
    uint64_t total = 0;
    for (const DirtyElementRange& dirty : dirty_ranges.ranges) total += uint64_t(dirty.count) * bytes_per_element;
    // 31.10: powers of two. Exact reserves made a recycled command's buffers
    // creep up a few bytes at a time: malloc + free on the title thread for
    // most draws (malloc/insert/free here were ~5% of RenderGuest). The bank
    // is 4 KB, so the rounding never exceeds it by more than 2x of the need.
    if (total <= source.size() && result.payload.capacity() < total)
      result.payload.reserve(std::bit_ceil(std::max<size_t>(size_t(total), 64)));
    if (result.ranges.capacity() < dirty_ranges.ranges.size())
      result.ranges.reserve(std::bit_ceil(std::max<size_t>(dirty_ranges.ranges.size(), 4)));
  }
  for (const DirtyElementRange& dirty : dirty_ranges.ranges) {
    if (!dirty.count || dirty.first > std::numeric_limits<uint32_t>::max() / bytes_per_element ||
        dirty.count > std::numeric_limits<uint32_t>::max() / bytes_per_element) {
      result = {};
      return false;
    }
    const uint32_t destination_offset = dirty.first * bytes_per_element;
    const uint32_t byte_count = dirty.count * bytes_per_element;
    if (destination_offset > source.size() || byte_count > source.size() - destination_offset ||
        result.payload.size() > std::numeric_limits<uint32_t>::max() - byte_count) {
      result = {};
      return false;
    }
    const uint32_t payload_offset = static_cast<uint32_t>(result.payload.size());
    const bool coalesce =
        !result.ranges.empty() &&
        result.ranges.back().destination_offset <=
            std::numeric_limits<uint32_t>::max() - result.ranges.back().byte_count &&
        result.ranges.back().destination_offset + result.ranges.back().byte_count ==
            destination_offset &&
        result.ranges.back().payload_offset <=
            std::numeric_limits<uint32_t>::max() - result.ranges.back().byte_count &&
        result.ranges.back().payload_offset + result.ranges.back().byte_count == payload_offset &&
        result.ranges.back().byte_count <= std::numeric_limits<uint32_t>::max() - byte_count;
    if (coalesce) {
      result.ranges.back().byte_count += byte_count;
    } else {
      result.ranges.push_back({destination_offset, payload_offset, byte_count});
    }
    result.payload.insert(result.payload.end(), source.begin() + destination_offset,
                          source.begin() + destination_offset + byte_count);
  }
  return ValidateConstantPayloadDelta(result, source.size());
}

// 49 (шаг 3.2): like CaptureConstantPayloadDelta, but only the dirty groups
// whose bytes differ from `shadow` (what the consumer's bank holds after every
// delta queued so far) go into the delta. The guest marks a group dirty on every
// write, also when it writes the same values; those groups are left out, so the
// consumer neither receives nor compares them. `shadow` is not changed here.
// dirty_bytes: bytes the dirty ranges cover (statistics).
inline bool CaptureChangedConstantPayloadDelta(std::span<const uint8_t> source,
                                               const DirtyRangeSet& dirty_ranges,
                                               uint32_t bytes_per_element,
                                               std::span<const uint8_t> shadow,
                                               ConstantPayloadDelta& result,
                                               uint64_t& dirty_bytes) {
  result.ranges.clear();
  result.payload.clear();
  result.complete_snapshot = false;
  result.changed_only = false;
  if (!bytes_per_element || shadow.size() != source.size()) {
    return false;
  }
  // 52: changed groups are gathered into runs and each run is copied with one insert.
  // The 49 version inserted every 64-byte group on its own; on the 888 the payload
  // copies were ~1.2 ms of RenderGuest per frame (vector insert bookkeeping per group).
  size_t run_begin = 0, run_size = 0;
  const auto flush_run = [&] {
    if (!run_size) return;
    const uint32_t payload_offset = static_cast<uint32_t>(result.payload.size());
    if (result.ranges.capacity() == result.ranges.size() && result.ranges.size() < 4)
      result.ranges.reserve(4);
    result.ranges.push_back({static_cast<uint32_t>(run_begin), payload_offset,
                             static_cast<uint32_t>(run_size)});
    result.payload.insert(result.payload.end(), source.data() + run_begin,
                          source.data() + run_begin + run_size);
    run_size = 0;
  };
  for (const DirtyElementRange& dirty : dirty_ranges.ranges) {
    if (!dirty.count || dirty.first > std::numeric_limits<uint32_t>::max() / bytes_per_element ||
        dirty.count > std::numeric_limits<uint32_t>::max() / bytes_per_element) {
      result = {};
      return false;
    }
    const size_t begin = size_t(dirty.first) * bytes_per_element;
    const size_t size = size_t(dirty.count) * bytes_per_element;
    if (begin > source.size() || size > source.size() - begin) {
      result = {};
      return false;
    }
    dirty_bytes += size;
    for (size_t offset = begin; offset < begin + size; offset += bytes_per_element) {
      if (std::memcmp(source.data() + offset, shadow.data() + offset, bytes_per_element) == 0) {
        continue;
      }
      if (run_size && run_begin + run_size == offset) {
        run_size += bytes_per_element;  // adjacent: same run (also across dirty ranges)
      } else {
        flush_run();
        run_begin = offset;
        run_size = bytes_per_element;
      }
    }
  }
  flush_run();
  result.changed_only = true;
  return ValidateConstantPayloadDelta(result, source.size());
}

inline bool CaptureCompleteConstantSnapshot(std::span<const uint8_t> source,
                                            ConstantPayloadDelta& result) {
  if (source.size() > std::numeric_limits<uint32_t>::max()) {
    result = {};
    return false;
  }
  result.ranges = {{0, 0, static_cast<uint32_t>(source.size())}};
  result.payload.assign(source.begin(), source.end());
  result.complete_snapshot = true;
  result.changed_only = false;  // 52
  return ValidateConstantPayloadDelta(result, source.size(), true);
}

struct ConstantStateVersion {
  StateVersionStamp token{};
  uint64_t content_hash = 0;
  size_t byte_size = 0;
  ConstantPayloadDelta delta;
  uint32_t deferred_ancestor_count = 0;
  // The parent and materialized bytes are render-worker-owned memoization.
  // State bytes and token are immutable once the version is published.
  mutable std::shared_ptr<const ConstantStateVersion> parent;
  mutable std::shared_ptr<const std::vector<uint8_t>> materialized;
  // Valid prefix of materialized when the render worker snapshots only what
  // the consuming draws read (the buffer is always byte_size long). Render
  // worker only; while the version is current, the canonical bank equals its
  // content, so the prefix can still be extended for a later draw. 0: all.
  mutable size_t materialized_bytes = 0;
};

// Versions and snapshot control blocks are allocated for nearly every draw
// that changes constants and released on retirement threads. Only the render
// worker (the owner of every AuthoritativeConstantState) allocates from this
// resource. Never destroyed: versions may outlive any static teardown order.
inline NativeSnapshotResource& ConstantVersionResource() {
  static auto* resource = new NativeSnapshotResource();
  return *resource;
}

inline std::shared_ptr<ConstantStateVersion> MakeConstantStateVersion() {
  return std::allocate_shared<ConstantStateVersion>(
      std::pmr::polymorphic_allocator<ConstantStateVersion>(&ConstantVersionResource()));
}

enum class ConstantApplyStatus : uint8_t {
  kApplied,
  kInvalidDelta,
  kNeedsCompleteSnapshot,
  kVersionSpaceExhausted,
};

struct ConstantApplyResult {
  ConstantApplyStatus status = ConstantApplyStatus::kApplied;
  bool changed = false;
  std::shared_ptr<const ConstantStateVersion> version;

  explicit operator bool() const { return status == ConstantApplyStatus::kApplied; }
};

// One instance is owned by the render worker for each device and shader
// stage. Published versions retain only copied deltas. A contiguous host view
// is reconstructed lazily on first consumer use and then memoized.
// Recycles the full-bank snapshot buffers taken for every draw that changes
// constants (two 3.5-4 KB heap blocks per draw otherwise). Releases may come
// from any thread (command retirement is asynchronous), hence the mutex.
class ConstantSnapshotPool {
 public:
  static std::shared_ptr<const std::vector<uint8_t>> Copy(const std::vector<uint8_t>& source,
                                                          size_t copy_bytes = SIZE_MAX) {
    copy_bytes = std::min(copy_bytes, source.size());
    std::vector<uint8_t>* buffer = nullptr;
    auto& local = local_free_list(source.size());
    if (local.empty()) {
      std::lock_guard lock(mutex());
      auto& shared = free_list(source.size());
      while (!shared.empty() && local.size() < 64) {
        local.push_back(shared.back());
        shared.pop_back();
      }
    }
    if (!local.empty()) {
      buffer = local.back();
      local.pop_back();
      std::memcpy(buffer->data(), source.data(), copy_bytes);
    } else if (copy_bytes == source.size()) {
      buffer = new std::vector<uint8_t>(source);
    } else {
      buffer = new std::vector<uint8_t>(source.size());
      std::memcpy(buffer->data(), source.data(), copy_bytes);
    }
    return std::shared_ptr<const std::vector<uint8_t>>(
        buffer,
        [](const std::vector<uint8_t>* bytes) {
          auto* owned = const_cast<std::vector<uint8_t>*>(bytes);
          auto& loc = local_free_list(owned->size());
          loc.push_back(owned);
          // Flush in batches of 64 so background retirement threads (GtaCmdRetirer)
          // neither hoard 1024 buffers in dead TLS nor lock mutex() per buffer.
          if (loc.size() >= 128) {
            std::lock_guard lock(mutex());
            auto& list = free_list(owned->size());
            while (loc.size() > 64) {
              auto* item = loc.back();
              loc.pop_back();
              if (list.size() < 8192) list.push_back(item);
              else delete item;
            }
          }
        },
        std::pmr::polymorphic_allocator<std::byte>(&ConstantVersionResource()));
  }

 private:
  static std::mutex& mutex() { static std::mutex instance; return instance; }
  static std::vector<std::vector<uint8_t>*>& local_free_list(size_t size) {
    struct FastLocalLists {
      std::vector<std::vector<uint8_t>*> vs_4096;
      std::vector<std::vector<uint8_t>*> ps_3584;
      std::vector<std::pair<size_t, std::vector<std::vector<uint8_t>*>>> other;
    };
    thread_local FastLocalLists lists;
    if (size == 4096) return lists.vs_4096;
    if (size == 3584) return lists.ps_3584;
    for (auto& [list_size, list] : lists.other)
      if (list_size == size) return list;
    lists.other.emplace_back(size, std::vector<std::vector<uint8_t>*>{});
    return lists.other.back().second;
  }
  static std::vector<std::vector<uint8_t>*>& free_list(size_t size) {
    static std::vector<std::pair<size_t, std::vector<std::vector<uint8_t>*>>> lists;
    for (auto& [list_size, list] : lists)
      if (list_size == size) return list;
    lists.emplace_back(size, std::vector<std::vector<uint8_t>*>{});
    return lists.back().second;
  }
};

class AuthoritativeConstantState {
 public:
  static constexpr uint32_t kMaximumDeferredAncestors = 64;
  explicit AuthoritativeConstantState(size_t byte_size = 0,
                                      StateVersionStamp initial_version = {})
      : canonical_(byte_size), version_(initial_version) {}

  size_t byte_size() const { return canonical_.size(); }
  bool initialized() const { return initialized_; }
  const std::vector<uint8_t>& canonical() const { return canonical_; }
  const std::shared_ptr<const ConstantStateVersion>& current_version() const { return current_; }

  // Draw capture owns the authoritative state before later commands mutate it.
  // Copy that complete state once here, rather than reconstructing a chain of
  // deltas on the frame's Vulkan recording critical path. Unconsumed updates
  // stay deferred and allocate no full-size snapshot.
  // bytes: the prefix later consumers read (all of it by default).
  const std::shared_ptr<const ConstantStateVersion>& SnapshotCurrentVersion(
      size_t bytes = SIZE_MAX) const {
    if (!current_) return current_;
    bytes = std::min(bytes, canonical_.size());
    if (!current_->materialized) {
      current_->materialized = ConstantSnapshotPool::Copy(canonical_, bytes);
      current_->materialized_bytes = bytes;
      current_->parent.reset();
    } else if (current_->materialized_bytes && current_->materialized_bytes < bytes) {
      // Bytes past the old prefix are read by no earlier consumer.
      auto* data = const_cast<uint8_t*>(current_->materialized->data());
      std::memcpy(data + current_->materialized_bytes,
                  canonical_.data() + current_->materialized_bytes,
                  bytes - current_->materialized_bytes);
      current_->materialized_bytes = bytes;
    }
    return current_;
  }


  // consume: a draw takes the new state at once. The version then holds the
  // complete bytes and no delta, so nothing is copied twice or allocated for
  // a replay chain that would never be used.
  template <typename HashCallback>
  ConstantApplyResult Apply(const ConstantPayloadDelta& delta, HashCallback&& hash_callback,
                            bool consume = false) {
    if (!ValidateConstantPayloadDelta(delta, canonical_.size())) {
      return {ConstantApplyStatus::kInvalidDelta, false, current_};
    }
    if (!initialized_ && !delta.complete_snapshot) {
      return {ConstantApplyStatus::kNeedsCompleteSnapshot, false, current_};
    }
    if (consume) {
      if (!CanIncrementStateVersion(version_)) {
        // Checked before writing: the state must keep matching current_.
        bool unchanged = initialized_;
        for (const ConstantDeltaRange& range : delta.ranges) {
          unchanged = unchanged && std::memcmp(canonical_.data() + range.destination_offset,
                                               delta.payload.data() + range.payload_offset,
                                               range.byte_count) == 0;
        }
        return {unchanged ? ConstantApplyStatus::kApplied : ConstantApplyStatus::kVersionSpaceExhausted,
                false, current_};
      }
      bool changed = !initialized_;
      for (const ConstantDeltaRange& range : delta.ranges) {
        const uint8_t* source = delta.payload.data() + range.payload_offset;
        uint8_t* destination = canonical_.data() + range.destination_offset;
        // 52: a changed_only delta needs no compare (see ConstantPayloadDelta).
        if (initialized_ && !delta.changed_only &&
            std::memcmp(destination, source, range.byte_count) == 0)
          continue;
        changed = true;
        std::memcpy(destination, source, range.byte_count);
      }
      if (!changed) return {ConstantApplyStatus::kApplied, false, current_};
      IncrementStateVersion(version_);
      initialized_ = true;
      auto next = MakeConstantStateVersion();
      next->token = version_;
      // 29.10: as before 27.10. In normal play hash_callback already returns 0
      // (gta4_native_lazy_constant_hash); only diagnostic captures ask for the
      // whole-bank hash, and they compare versions by it.
      next->content_hash = hash_callback(std::span<const uint8_t>(canonical_));
      next->byte_size = canonical_.size();
      // No delta and no parent: the draw snapshots this version from the
      // canonical bank right away (SnapshotCurrentVersion), while it is current.
      next->delta.complete_snapshot = true;
      current_ = std::move(next);
      return {ConstantApplyStatus::kApplied, true, current_};
    }

    // 29.10: checked before canonical_ is written (as in the consume path above):
    // the single-pass copy below must not change the bank when the version
    // cannot move, or canonical_ would stop matching current_.
    if (!CanIncrementStateVersion(version_)) {
      bool unchanged = initialized_;
      for (const ConstantDeltaRange& range : delta.ranges) {
        unchanged = unchanged && std::memcmp(canonical_.data() + range.destination_offset,
                                             delta.payload.data() + range.payload_offset,
                                             range.byte_count) == 0;
      }
      return {unchanged ? ConstantApplyStatus::kApplied : ConstantApplyStatus::kVersionSpaceExhausted,
              false, current_};
    }
    ConstantPayloadDelta changed_delta;
    changed_delta.complete_snapshot = !initialized_;
    for (const ConstantDeltaRange& range : delta.ranges) {
      const uint8_t* source = delta.payload.data() + range.payload_offset;
      uint8_t* destination = canonical_.data() + range.destination_offset;
      if (initialized_ && !delta.changed_only &&  // 52
          std::memcmp(destination, source, range.byte_count) == 0) {
        continue;
      }
      // Пишем сразу в canonical_ за один проход!
      std::memcpy(destination, source, range.byte_count);
      const uint32_t payload_offset = static_cast<uint32_t>(changed_delta.payload.size());
      changed_delta.ranges.push_back(
          {range.destination_offset, payload_offset, range.byte_count});
      changed_delta.payload.insert(changed_delta.payload.end(), source,
                                   source + range.byte_count);
    }

    if (initialized_ && changed_delta.empty()) {
      return {ConstantApplyStatus::kApplied, false, current_};
    }
    IncrementStateVersion(version_);
    initialized_ = true;

    auto next = MakeConstantStateVersion();
    next->token = version_;
    next->content_hash = hash_callback(std::span<const uint8_t>(canonical_));  // 29.10: see above
    next->byte_size = canonical_.size();
    // Updates without a consuming draw must not retain an unlimited chain.
    // A complete copied base is equivalent to replaying all prior deltas.
    const bool complete_bytes = changed_delta.ranges.size() == 1 &&
        changed_delta.ranges[0].destination_offset == 0 &&
        changed_delta.ranges[0].payload_offset == 0 &&
        changed_delta.ranges[0].byte_count == canonical_.size() &&
        changed_delta.payload.size() == canonical_.size();
    if (complete_bytes) {
      changed_delta.complete_snapshot = true;
    } else if (current_ && current_->deferred_ancestor_count >= kMaximumDeferredAncestors) {
      if (!CaptureCompleteConstantSnapshot(canonical_, changed_delta)) {
        return {ConstantApplyStatus::kInvalidDelta, false, current_};
      }
    }
    next->delta = std::move(changed_delta);
    if (!next->delta.complete_snapshot) {
      next->parent = current_;
      next->deferred_ancestor_count = current_ ? (current_->materialized ? 1 : current_->deferred_ancestor_count + 1) : 0;
    }
    current_ = std::move(next);
    return {ConstantApplyStatus::kApplied, true, current_};
  }

  // The caller must retain the version for the whole use of this view. Draw
  // commands already do so, and need not retain/release its immutable byte
  // allocation again on every binding. Owning/asynchronous consumers continue
  // to use Materialize below.
  static const std::vector<uint8_t>* MaterializeView(
      const std::shared_ptr<const ConstantStateVersion>& version) {
    if (!version) {
      return nullptr;
    }
    if (!version->materialized) {
      Materialize(version);
    }
    return version->materialized.get();
  }

  static std::shared_ptr<const std::vector<uint8_t>> Materialize(
      const std::shared_ptr<const ConstantStateVersion>& version) {
    if (!version) {
      return {};
    }
    if (version->materialized) {
      return version->materialized;
    }

    // The common ordered-draw case has a memoized direct parent. Avoid a
    // temporary owning chain and its allocator traffic for a single delta.
    if (version->parent && version->parent->materialized &&
        version->parent->materialized->size() == version->byte_size &&
        (!version->parent->materialized_bytes ||
         version->parent->materialized_bytes == version->byte_size) &&
        ValidateConstantPayloadDelta(version->delta, version->byte_size)) {
      auto bytes = std::make_shared<std::vector<uint8_t>>(*version->parent->materialized);
      for (const auto& range : version->delta.ranges)
        std::memcpy(bytes->data() + range.destination_offset,
                    version->delta.payload.data() + range.payload_offset, range.byte_count);
      version->materialized = std::move(bytes);
      version->materialized_bytes = version->byte_size;
      version->parent.reset();
      return version->materialized;
    }

    std::vector<std::shared_ptr<const ConstantStateVersion>> chain;
    std::shared_ptr<const ConstantStateVersion> cursor = version;
    while (cursor && !cursor->materialized) {
      chain.push_back(cursor);
      cursor = cursor->parent;
    }
    if (cursor && cursor->materialized_bytes && cursor->materialized_bytes != cursor->byte_size)
      return {};
    auto bytes = std::make_shared<std::vector<uint8_t>>(
        cursor && cursor->materialized ? *cursor->materialized
                                       : std::vector<uint8_t>(version->byte_size));
    for (auto node = chain.rbegin(); node != chain.rend(); ++node) {
      if ((*node)->byte_size != bytes->size() ||
          !ValidateConstantPayloadDelta((*node)->delta, bytes->size())) {
        return {};
      }
      for (const ConstantDeltaRange& range : (*node)->delta.ranges) {
        std::memcpy(bytes->data() + range.destination_offset,
                    (*node)->delta.payload.data() + range.payload_offset, range.byte_count);
      }
    }
    version->materialized = bytes;
    version->materialized_bytes = version->byte_size;
    // The memoized contiguous state is now a complete immutable base. Cutting
    // its ancestry prevents a long-running device from retaining every prior
    // delta version across frames.
    version->parent.reset();
    return version->materialized;
  }

 private:
  std::vector<uint8_t> canonical_;
  StateVersionStamp version_{};
  bool initialized_ = false;
  std::shared_ptr<const ConstantStateVersion> current_;
};

template <typename T>
class AuthoritativeScalarState {
 public:
  struct ApplyResult {
    ConstantApplyStatus status = ConstantApplyStatus::kApplied;
    bool changed = false;
  };

  ApplyResult Apply(const T& value) {
    if (initialized_ && value_ == value) {
      return {};
    }
    if (!CanIncrementStateVersion(version_)) {
      return {ConstantApplyStatus::kVersionSpaceExhausted, false};
    }
    value_ = value;
    initialized_ = true;
    IncrementStateVersion(version_);
    return {ConstantApplyStatus::kApplied, true};
  }

  bool initialized() const { return initialized_; }
  const T& value() const { return value_; }
  StateVersionStamp version() const { return version_; }

 private:
  T value_{};
  StateVersionStamp version_{};
  bool initialized_ = false;
};

// This is the complete semantic input identity for the host-built shared
// constant block. It intentionally includes descriptor meaning epochs and
// page/copy ownership in addition to the indices written into the block.
// Keeping it independently testable makes omissions visible when the shared
// block gains another producer input.
template <size_t TextureStageCount>
struct SharedConstantSemanticKey {
  std::array<uint32_t, TextureStageCount> texture_descriptor_indices{};
  std::array<uint32_t, TextureStageCount> sampler_descriptor_indices{};
  // Sampler objects deliberately exclude bias. Reusing the same descriptor
  // with different guest bias must still produce a different shared block.
  std::array<uint32_t, TextureStageCount> sampler_lod_bias_bits{};
  StateVersionStamp boolean_version{};
  uint64_t image_descriptor_epoch = 0;
  uint64_t sampler_descriptor_epoch = 0;
  uint64_t cached_descriptor_epoch = 0;
  uint64_t environmental_data_hash = 0;
  uint64_t environmental_sequence = 0;
  uint64_t tone_lut_address = 0;
  uint64_t cloud_mask_address = 0;
  uint32_t cloud_mask_width = 0, cloud_mask_height = 0;
  uint32_t modern_effects_enabled = 0;
  uint32_t water_reflection = 0;
  std::array<uint32_t, 6> viewport_bits{};
  uint32_t device = 0;
  uint32_t descriptor_copy = 0;
  uint32_t descriptor_page = std::numeric_limits<uint32_t>::max();
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t logical_width = 0;
  uint32_t logical_height = 0;
  uint32_t sample_count = 0;
  uint32_t alpha_reference_bits = 0;
  uint32_t alpha_to_mask = 0;
  std::array<uint32_t, 4> color_output_info{};
  uint32_t color_output_mask = 0;
  std::array<uint32_t, 4> clip_plane_bits{};
  uint32_t clip_plane_enable_mask = 0;
  uint32_t vertex_booleans = 0;
  uint32_t pixel_booleans = 0;
  uint8_t descriptor_backend = 0;
  uint8_t environment_present = 0;
  uint8_t split_postfx_applied = 0;

  constexpr bool operator==(const SharedConstantSemanticKey&) const = default;
};

inline size_t FindFirstConstantMismatch(std::span<const uint8_t> left,
                                        std::span<const uint8_t> right) {
  const size_t common_size = std::min(left.size(), right.size());
  size_t offset = 0;
  while (offset < common_size && left[offset] == right[offset]) {
    ++offset;
  }
  return offset != common_size || left.size() != right.size() ? offset : kNoSemanticMismatch;
}

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_STATEFUL_CONSTANT_STATE_H_
