#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rex::graphics::gta4_native {

// Tracks commands actually emitted into one command buffer. This owns no GPU
// resources. Reset at recording boundaries and after any external draw path.
// Floating-point state is passed as bits, preserving signed zero and NaN values.
template <size_t DescriptorSetCount>
class NativeDrawStateCache {
 public:
  struct IndexedDescriptorKey {
    uint64_t layout = 0;
    uint32_t descriptor_page = 0;
    uint32_t descriptor_copy = 0;
    uint64_t image_descriptor_epoch = 0;
    uint32_t active_frame_slot = 0;
    bool operator==(const IndexedDescriptorKey&) const = default;
  };
  struct DynamicDrawKey {
    const void* fixed_state = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t color_write_mask = 0;
    uint32_t debug_gpu_attribution = 0;
    uint64_t color_formats_lo = 0;
    uint64_t color_formats_hi = 0;
    bool operator==(const DynamicDrawKey&) const = default;
  };

  void Reset() { *this = {}; }

  bool UpdatePipeline(uint64_t pipeline) {
    if (!pipeline_.Update(pipeline)) {
      return false;
    }
    // A different pipeline can contain static state that invalidates dynamic
    // values. Conservatively re-emit them even for compatible native pipelines.
    viewport_ = {};
    scissor_ = {};
    depth_bias_ = {};
    stencil_ = {};
    blend_constants_ = {};
    push_constants_ = {};
    dynamic_draw_ = {};
    return true;
  }

  bool HasMatchingIndexedDescriptors(const IndexedDescriptorKey& key) const noexcept {
    return indexed_descriptors_.last && *indexed_descriptors_.last == key;
  }
  void RecordIndexedDescriptors(const IndexedDescriptorKey& key) noexcept {
    indexed_descriptors_.last = key;
  }

  bool HasMatchingDynamicDraw(const DynamicDrawKey& key) const noexcept {
    return dynamic_draw_.last && *dynamic_draw_.last == key;
  }
  void RecordDynamicDraw(const DynamicDrawKey& key) noexcept {
    dynamic_draw_.last = key;
  }

  bool UpdateDescriptors(uint64_t layout,
                         const std::array<uint64_t, DescriptorSetCount>& sets) {
    if (!descriptors_.Update({layout, sets})) {
      return false;
    }
    indexed_descriptors_ = {};
    return true;
  }
  bool UpdateViewport(const std::array<uint32_t, 6>& bits) { return viewport_.Update(bits); }
  bool UpdateScissor(const std::array<int64_t, 4>& rectangle) {
    return scissor_.Update(rectangle);
  }
  bool UpdateDepthBias(const std::array<uint32_t, 3>& bits) { return depth_bias_.Update(bits); }
  bool UpdateStencil(const std::array<uint32_t, 6>& faces) { return stencil_.Update(faces); }
  bool UpdateBlendConstants(const std::array<uint32_t, 4>& bits) {
    return blend_constants_.Update(bits);
  }
  bool UpdatePushConstants(uint64_t layout, const std::array<uint64_t, 3>& addresses) {
    return push_constants_.Update({layout, addresses});
  }
  // Vertex buffer bindings are command-buffer state that survives pipeline
  // and render-pass changes; the command buffer is part of the value.
  bool UpdateVertexBuffer(uint64_t command_buffer, uint32_t binding, uint64_t buffer,
                          uint64_t offset) {
    if (binding >= vertex_buffers_.size()) return true;
    return vertex_buffers_[binding].Update({command_buffer, buffer, offset});
  }
  // Index buffer binding is command-buffer state that survives pipeline
  // and render-pass changes.
  bool UpdateIndexBuffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset,
                         uint32_t index_type) {
    return index_buffer_.Update({command_buffer, buffer, offset, uint64_t(index_type)});
  }

 private:
  template <typename Value>
  struct Tracked {
    std::optional<Value> last;
    bool Update(const Value& value) {
      if (last && *last == value) {
        return false;
      }
      last = value;
      return true;
    }
  };
  template <size_t Count>
  struct LayoutValues {
    uint64_t layout;
    std::array<uint64_t, Count> values;
    bool operator==(const LayoutValues&) const = default;
  };

  Tracked<uint64_t> pipeline_;
  Tracked<LayoutValues<DescriptorSetCount>> descriptors_;
  Tracked<IndexedDescriptorKey> indexed_descriptors_;
  Tracked<DynamicDrawKey> dynamic_draw_;
  Tracked<std::array<uint32_t, 6>> viewport_;
  Tracked<std::array<int64_t, 4>> scissor_;
  Tracked<std::array<uint32_t, 3>> depth_bias_;
  Tracked<std::array<uint32_t, 6>> stencil_;
  Tracked<std::array<uint32_t, 4>> blend_constants_;
  Tracked<LayoutValues<3>> push_constants_;
  std::array<Tracked<std::array<uint64_t, 3>>, 32> vertex_buffers_;
  Tracked<std::array<uint64_t, 4>> index_buffer_;
};

}  // namespace rex::graphics::gta4_native
