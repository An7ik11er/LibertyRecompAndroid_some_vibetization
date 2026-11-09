#pragma once

#include "native_triangle_fan.h"
#include "native_vertex_cache.h"
#include "native_persistent_buffer_map.h"
#include "native_deferred_release.h"
#include "native_flat_set.h"
#include "native_gpu_counters.h"
#include "native_pipeline_stats.h"
#include "native_command_bytes.h"
#include "native_zero_allocate.h"  // 54
#include "native_capture_ring.h"   // 56
#include "native_owned_commands.h"
#include "native_queue_protection.h"
#include "native_snapshot_resource.h"
#include "native_inline_mutex.h"

#include <array>
#include <bit>
#include <cassert>
#include <new>
#include <type_traits>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <algorithm>
#include <coroutine>
#include <exception>
#include <utility>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/graphics/gta4_native/native_black_anomaly.h>
#include <rex/graphics/gta4_native/surface_view.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/vulkan/device.h>

#include "postfx_resource_pool.h"
#include "dirty_state_delta.h"
#include "frame_constant_arena.h"
#include "native_constant_upload.h"
#include "native_working_set.h"
#include "native_immutable_bindings.h"
#include "native_texture_protection.h"
#include "native_prepared_bindings.h"
#include "native_image_reuse.h"
#include "native_resource_reclaimer.h"
#include "native_texture_eviction_index.h"
#include <memory_resource>
#include "stateful_constant_state.h"
#include "native_buffer_arena.h"
#include "native_bulb_appearance.h"
#include "native_aspect_content.h"
#include <rex/graphics/gta4_native/phone_trace.h>
#include <rex/graphics/gta4_native/tv_trace.h>
#include <rex/graphics/gta4_native/fire_escape_trace.h>
#include "native_attachment_policy.h"
#include "native_binding_policy.h"
#include "native_descriptor_backend.h"
#include "native_draw_state_cache.h"
#include "native_frame_context.h"
#include "native_frame_scheduling.h"
#include "native_gpu_attribution.h"
#include <rex/graphics/gta4_native/gpu_pass_origin.h>
#include "native_host_enhancement_policy.h"
#include "native_lighting_lineage.h"
#include "native_memory_samples.h"
#include "native_owner_retirement.h"
#include "native_performance_samples.h"
#include "native_profile_detail.h"
#include "native_pipeline_lookup_memo.h"
#include "native_reflection_registry.h"
#include "native_resolve_policy.h"
#include "native_resolve_reuse.h"
#include "native_resolve_descriptor_cache.h"
#include "native_room_light_probe.h"
#include "native_sampler_cache_key.h"
#include "native_sampler_lod_bias.h"
#include "shader_override_policy.h"
#include "modern_shader_policy.h"
#include "modern_shader_diagnostics.h"
#include "native_virtual_resource_registry.h"
#include "smaa_pipeline.h"
#include "split_postfx_pass.h"
#include "sun_shafts_pass.h"
#include "temporal/command_capture.h"
#include "temporal/fsr_vulkan.h"
#include "temporal/dlss_upscaler.h"
#include "temporal/geometry_history.h"
#include "temporal/shader_archive.h"
#include "temporal/shader_contract.h"
#include "temporal/vulkan_scene.h"
#include "temporal/vulkan_frame_generation.h"

struct ShaderOverrideCacheEntry;

namespace rex::memory {
class Memory;
}
namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::ui {
class GraphicsProvider;
class Presenter;
class WindowedAppContext;
namespace vulkan {
class VulkanSubmissionTracker;
}
}  // namespace rex::ui

namespace rex::graphics::gta4_native {

// 60: the coroutine frames of the two frame tasks hold every local of RecordNativeFrame /
// PrepareFrameTextures and are allocated once per frame: the last blocks are kept for reuse.
struct NativeFrameTaskMemory {
  struct Block {
    void* pointer = nullptr;
    std::size_t size = 0;
  };
  std::mutex mutex;
  std::array<Block, 4> blocks{};
  std::size_t largest = 0;
  static NativeFrameTaskMemory& Get() {
    static NativeFrameTaskMemory memory;
    return memory;
  }
  static void* Allocate(std::size_t size) {
    NativeFrameTaskMemory& memory = Get();
    {
      std::lock_guard lock(memory.mutex);
      memory.largest = std::max(memory.largest, size);
      for (Block& block : memory.blocks) {
        if (block.pointer && block.size == size) return std::exchange(block.pointer, nullptr);
      }
    }
    return ::operator new(size);
  }
  static void Free(void* pointer, std::size_t size) noexcept {
    NativeFrameTaskMemory& memory = Get();
    {
      std::lock_guard lock(memory.mutex);
      for (Block& block : memory.blocks) {
        if (!block.pointer) {
          block = {pointer, size};
          return;
        }
      }
    }
    ::operator delete(pointer);
  }
};

// 60: a frame recording that can stop between two commands and continue when more arrive
// (gta4_native_stream_record). Starts running at once; stays suspended at its end until the
// owner destroys it. Without streaming it never suspends and is done when the call returns.
class NativeFrameTask {
 public:
  struct promise_type {
    bool value = false;
    static void* operator new(std::size_t size) { return NativeFrameTaskMemory::Allocate(size); }
    static void operator delete(void* pointer, std::size_t size) noexcept {
      NativeFrameTaskMemory::Free(pointer, size);
    }
    NativeFrameTask get_return_object() noexcept {
      return NativeFrameTask(std::coroutine_handle<promise_type>::from_promise(*this));
    }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_value(bool result) noexcept { value = result; }
    void unhandled_exception() noexcept { std::terminate(); }
  };
  NativeFrameTask() = default;
  explicit NativeFrameTask(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  NativeFrameTask(NativeFrameTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  NativeFrameTask& operator=(NativeFrameTask&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  NativeFrameTask(const NativeFrameTask&) = delete;
  NativeFrameTask& operator=(const NativeFrameTask&) = delete;
  ~NativeFrameTask() { reset(); }
  bool valid() const { return bool(handle_); }
  bool done() const { return !handle_ || handle_.done(); }
  void resume() {
    if (handle_ && !handle_.done()) handle_.resume();
  }
  bool result() const { return handle_ && handle_.done() && handle_.promise().value; }
  // Destroying a suspended task runs the destructors of its live locals (scope guards).
  void reset() {
    if (handle_) handle_.destroy();
    handle_ = {};
  }

 private:
  std::coroutine_handle<promise_type> handle_;
};

class NativeTextureUploadBatch;
class NativeGpuScopeSummary;

class Gta4NativeGraphicsSystem final : public system::IGraphicsSystem {
 public:
  Gta4NativeGraphicsSystem();
  ~Gta4NativeGraphicsSystem() override;

  X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  ui::GraphicsProvider* provider() const override { return provider_.get(); }
  ui::Presenter* presenter() const override { return presenter_.get(); }

  uint32_t GetTitleCommandAbi(uint32_t title_id) const override;
  bool SubmitTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command,
                          size_t command_size) override;
  bool ExecuteTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command,
                           size_t command_size, void* result, size_t result_size) override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  void Shutdown() override;

 private:
  enum class NativeVertexNumericType : uint8_t {
    kFloat,
    kSignedInteger,
    kUnsignedInteger,
  };

  struct NativeVertexInput {
    uint32_t location = 0;
    NativeVertexNumericType numeric_type = NativeVertexNumericType::kFloat;
    uint32_t component_count = 0;

    bool operator==(const NativeVertexInput&) const = default;
  };

  struct NativeShader {
    ShaderStage stage = ShaderStage::kPixel;
    uint64_t hash = 0;
    uint32_t specialization_constants_mask = 0;
    uint32_t used_texture_mask = 0;
    uint32_t color_output_mask = 0;
    VkShaderModule early_module = VK_NULL_HANDLE;
    VkShaderModule late_module = VK_NULL_HANDLE;
    // 0-3: early, late, override early, override late; 4-7: the same with
    // EarlyFragmentTests (pixel shaders without depth writes only).
    std::array<uint64_t, 12> module_code_hashes{};
    std::array<VkShaderModule, 4> early_test_modules{};
    // Color outputs demoted to private variables, for colorless pipelines.
    std::array<VkShaderModule, 4> colorless_modules{};
    std::string filename;
    std::vector<NativeVertexInput> vertex_inputs;
    // Reads gl_VertexIndex/gl_InstanceIndex: its draws cannot be concatenated.
    bool uses_vertex_index = false;
    // Bytes of this stage's guest constant bank any of its modules can read
    // (SpirvConstantRange::kUnbounded: the whole bank).
    uint32_t constant_bytes = 0xFFFFFFFFu;
    // Bytes of its own guest constant bank the shader can read, measured before
    // the UBO rewrite (whose loads the range pass does not follow). Draws upload
    // only this prefix of the bank; 0xFFFFFFFF means the whole bank.
    uint32_t upload_constant_bytes = 0xFFFFFFFFu;
    // Loops bounded by the loop watchdog in the stock modules (diagnostics).
    uint32_t watchdog_loops = 0;
    bool water_surface = false;  // scene water, the consumer of the water reflection
    // Vertex conversion depends only on the input interface, so converted and
    // persistent vertex data are shared by every shader with the same inputs.
    uint64_t input_signature_hash = 0;

    const ShaderOverrideCacheEntry* override_entry = nullptr;
    uint32_t override_specialization_constants_mask = 0;
    uint32_t override_used_texture_mask = 0;
    uint32_t override_color_output_mask = 0;
    VkShaderModule override_early_module = VK_NULL_HANDLE;
    VkShaderModule override_late_module = VK_NULL_HANDLE;
  };

  struct ProducerShaderMetadata {
    ShaderStage stage = ShaderStage::kPixel;
    uint64_t hash = 0;
    uint32_t used_texture_mask = 0;
  };

  struct ProducerDeviceShaderState {
    uint32_t pixel_shader = 0;
    uint32_t vertex_shader = 0;
  };

  struct NativeVertexDeclaration {
    uint32_t handle = 0;
    uint64_t generation = 0;
    uint64_t content_hash = 0;
    uint32_t maximum_stream = 0;
    std::vector<VertexElement> elements;
  };

  struct NativeFixedFunctionState {
    uint32_t depth_enable = 0;
    uint32_t depth_function = 0;
    uint32_t depth_write_enable = 0;
    uint32_t depth_clamp_enable = 0;
    uint32_t clip_control = 0;
    uint32_t user_clip_plane_enable_mask = 0;
    std::array<uint32_t, 4> clip_plane_bits{};
    uint32_t negative_one_to_one_clip_space = 0;
    uint32_t cull_mode = 0;
    uint32_t polygon_mode = 0;
    uint32_t blend_enable = 0;
    std::array<uint32_t, kRenderTargetCount> blend_controls{};
    uint32_t source_blend = 0;
    uint32_t destination_blend = 0;
    uint32_t blend_operation = 0;
    uint32_t source_blend_alpha = 0;
    uint32_t destination_blend_alpha = 0;
    uint32_t blend_operation_alpha = 0;
    std::array<float, 4> blend_constants{};
    uint32_t alpha_test_enable = 0;
    uint32_t alpha_function = 0;
    float alpha_reference = 0.0f;
    // Xenos RB_COLORCONTROL bit 4. This is deliberately captured separately
    // from alpha test: foliage may request alpha-to-mask while alpha test is
    // disabled.
    uint32_t alpha_to_mask_enable = 0;
    // Native shader layout: four RB_COLORCONTROL offsets in bits 0-7 and the
    // enable in bit 8. This remains dynamic and is not a pipeline-key field.
    uint32_t alpha_to_mask = 0;
    uint32_t stencil_enable = 0;
    uint32_t two_sided_stencil = 0;
    uint32_t stencil_fail = 0;
    uint32_t stencil_depth_fail = 0;
    uint32_t stencil_pass = 0;
    uint32_t stencil_function = 0;
    uint32_t stencil_reference = 0;
    uint32_t stencil_mask = 0;
    uint32_t stencil_write_mask = 0;
    uint32_t back_stencil_reference = 0;
    uint32_t back_stencil_mask = 0;
    uint32_t back_stencil_write_mask = 0;
    uint32_t ccw_stencil_fail = 0;
    uint32_t ccw_stencil_depth_fail = 0;
    uint32_t ccw_stencil_pass = 0;
    uint32_t ccw_stencil_function = 0;
    uint32_t scissor_enable = 0;
    uint32_t slope_scaled_depth_bias_bits = 0;
    uint32_t depth_bias_bits = 0;
    bool depth_bias_enable = false;
    bool depth_bias_representable = true;
    uint32_t color_write_mask = 0;
    uint32_t sample_mask = 0xFFFFu;
    std::array<uint32_t, 6> viewport_bits{};
    std::array<int32_t, 4> scissor{};

    bool operator==(const NativeFixedFunctionState&) const = default;
  };

  // 48 (тонкая команда, часть 1): render targets of a draw as captured by the
  // producer. Shared by consecutive draws, like the fixed state below.
  struct NativeTargetSnapshot {
    std::array<SurfaceDescriptor, kRenderTargetCount> colors{};
    SurfaceDescriptor depth{};
  };
  // 48: a command's view of an immutable state block. Consecutive draws with the
  // same fixed-function state / render targets point at one block instead of
  // carrying ~440 bytes each. Blocks are owned by the capture keep-alive of the
  // producer's frame (CaptureKeepAlive::*_blocks, which the command holds) or by
  // NativeCommand::fixed_owner (worker-side variants). Read-only: a changed state
  // is a new block. No operator==: equal content may live in different blocks,
  // compare *a == *b (same block => equal). Default: a default-constructed
  // state, as a fresh command had before.
  template <typename T>
  class NativeStateRef {
   public:
    NativeStateRef() = default;
    explicit NativeStateRef(const T* block) : block_(block ? block : &kDefault) {}
    const T* operator->() const { return block_; }
    const T& operator*() const { return *block_; }
    const T* get() const { return block_; }
    bool IsDefault() const { return block_ == &kDefault; }
    static inline const T kDefault{};

   private:
    const T* block_ = &kDefault;
  };

  // 34: the pipeline memo of a snapshot. Same lookups as
  // NativePipelineLookupMemo, but a copy starts empty and no entry is
  // initialized before it is stored. The worker copies pipeline_state_ into a
  // new snapshot for nearly every draw, and the inline memo (4 entries, each
  // with a full fixed state and lookup context: ~1.7 KB) was most of those
  // bytes, though a copied memo can never hit (entries carry their owner
  // snapshot's address). SnapshotPipeline was ~10 % of the render worker.
  template <typename FixedState, size_t ColorTargetCount, typename Pipeline>
  class NativeSnapshotPipelineMemo {
   public:
    using Context = NativePipelineLookupContext<ColorTargetCount>;
    NativeSnapshotPipelineMemo() = default;
    NativeSnapshotPipelineMemo(const NativeSnapshotPipelineMemo&) noexcept {}
    NativeSnapshotPipelineMemo& operator=(const NativeSnapshotPipelineMemo&) noexcept {
      count_ = 0;
      next_ = 0;
      return *this;
    }
    Pipeline Find(const void* owner, const FixedState& fixed, const Context& context) const {
      if (!owner || !context.lifetime) return Pipeline{};
      for (uint32_t i = 0; i < count_; ++i) {
        const Entry& entry = At(i);
        if (entry.pipeline && owner == entry.owner && context == entry.context &&
            fixed == entry.fixed)
          return entry.pipeline;
      }
      return Pipeline{};
    }
    Pipeline FindSigned(const void* owner, uint64_t signature, const Context& context) const {
      if (!owner || !context.lifetime || !signature) return Pipeline{};
      for (uint32_t i = 0; i < count_; ++i) {
        const Entry& entry = At(i);
        if (entry.pipeline && entry.signature == signature && owner == entry.owner &&
            context == entry.context)
          return entry.pipeline;
      }
      return Pipeline{};
    }
    void Store(const void* owner, const FixedState& fixed, const Context& context,
               Pipeline pipeline, uint64_t signature = 0) {
      if (!owner || !context.lifetime || !pipeline) return;
      for (uint32_t i = 0; i < count_; ++i) {
        Entry& entry = At(i);
        if (entry.owner == owner && entry.context == context && entry.fixed == fixed) {
          entry.pipeline = pipeline;
          if (signature) entry.signature = signature;
          return;
        }
      }
      const uint32_t index = count_ < kEntries ? count_++ : next_;
      ::new (static_cast<void*>(storage_ + size_t(index) * sizeof(Entry)))
          Entry{owner, fixed, context, pipeline, signature};
      if (count_ == kEntries && index == next_) next_ = (next_ + 1) % kEntries;
    }

   private:
    struct Entry {
      const void* owner;
      FixedState fixed;
      Context context;
      Pipeline pipeline;
      uint64_t signature;
    };
    static_assert(std::is_trivially_copyable_v<Entry> && std::is_trivially_destructible_v<Entry>);
    static constexpr uint32_t kEntries = 4;
    Entry& At(uint32_t i) {
      return *std::launder(reinterpret_cast<Entry*>(storage_ + size_t(i) * sizeof(Entry)));
    }
    const Entry& At(uint32_t i) const {
      return *std::launder(reinterpret_cast<const Entry*>(storage_ + size_t(i) * sizeof(Entry)));
    }
    alignas(Entry) unsigned char storage_[sizeof(Entry) * kEntries];
    uint32_t count_ = 0;
    uint32_t next_ = 0;
  };

  struct NativePipelineState {
    struct VertexStream {
      uint32_t buffer = 0;
      uint32_t offset = 0;
      uint32_t stride = 0;
      uint32_t stride_words = 0;
    };

    std::array<uint32_t, kTextureStageCount> textures{};
    std::array<SurfaceDescriptor, kRenderTargetCount> render_targets{};
    std::array<VertexStream, kVertexStreamCount> vertex_streams{};
    uint32_t pixel_shader = 0;
    uint32_t vertex_shader = 0;
    const NativeShader* pixel_shader_resource = nullptr;
    const NativeShader* vertex_shader_resource = nullptr;
    uint32_t vertex_declaration = 0;
    std::shared_ptr<const NativeVertexDeclaration> vertex_declaration_resource;
    SurfaceDescriptor depth_stencil{};
    uint32_t depth_stencil_trace_wrapper = 0;
    uint32_t depth_stencil_trace_caller = 0;
    uint32_t index_buffer = 0;
    uint64_t version = 0;
    // Only the render worker updates memoization on an immutable snapshot.
    mutable NativeSnapshotPipelineMemo<NativeFixedFunctionState, kRenderTargetCount, VkPipeline>
        pipeline_lookup_memo;
    // Recorder-only memos of the shader-name classifications behind GPU range
    // attribution (string searches over both filenames); 0xFFFF: not computed.
    mutable uint16_t translucent_range_memo = 0xFFFF;
    mutable uint16_t shader_category_memo = 0xFFFF;
  };

  struct NativePersistentBufferEntry;

  struct NativeBufferResource {
    struct ConvertedVertexPayload {
      uint64_t declaration_hash = 0;
      uint64_t shader_hash = 0;
      uint32_t stream = 0;
      uint32_t stream_offset = 0;
      uint32_t stride = 0;
      std::vector<uint8_t> payload;
      uint32_t created_frame = 0;
      uint32_t last_used_frame = 0;
    };

    uint32_t handle = 0;
    uint32_t flags = 0;
    uint32_t guest_address = 0;
    uint32_t guest_size = 0;
    uint64_t content_hash = 0;
    uint64_t generation = 0;
    uint32_t created_frame = 0;
    mutable uint32_t keepalive_serial = 0;
    mutable std::atomic<uint32_t> last_used_frame{0};
    mutable std::atomic<size_t> shadow_validation_offset{0};
    std::vector<uint8_t> payload;
    // Built only by the single render worker. Keeping conversions with the
    // immutable source generation makes their lifetime follow the guest
    // resource and avoids repeating format/endian conversion every frame.
    mutable std::vector<ConvertedVertexPayload> converted_vertex_payloads;
    mutable std::vector<uint8_t> host_index16_payload;
    mutable std::vector<uint8_t> host_index32_payload;
    mutable NativeTriangleFanCache triangle_fan_cache;
    // Recorder-only: host-endian index ranges reordered for the vertex cache
    // (NativeVertexCacheOptimizer), keyed by start << 32 | count.
    mutable std::unordered_map<uint64_t, std::shared_ptr<NativeVertexCacheRange>>
        vertex_cache_ranges;
    mutable NativeOwnerRetirementWatch<NativePersistentBufferEntry> persistent_retirement;
    // Recorder-only: the persistent buffer entry this resource resolved to
    // last (GetOrCreatePersistentBuffer), with its key's bytes and the map's
    // clear epoch. Entries are erased only after their owner expired, so the
    // pointer stays valid while this resource exists.
    mutable std::array<uint64_t, 6> persistent_memo_key{};
    mutable NativePersistentBufferEntry* persistent_memo_entry = nullptr;
    mutable uint64_t persistent_memo_epoch = 0;
  };

  enum class NativeTextureOrigin : uint8_t {
    kGuestSnapshot,
    kResolveOutput,
    kHostReplacement,
    kPackedDepthAlias,
  };

  // 32: enable_shared_from_this so a long-lived holder (the texture image's
  // source) can turn a frame keep-alive alias back into an owning reference
  // (OwningTextureResource). Every resource is made by make_shared.
  struct NativeTextureResource : std::enable_shared_from_this<NativeTextureResource> {
    struct MipLevel {
      uint32_t level = 0;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t depth = 1;
      uint32_t base_array_layer = 0;
      uint32_t layer_count = 1;
      uint32_t buffer_row_length = 0;
      uint32_t buffer_image_height = 0;
      size_t payload_offset = 0;
      size_t payload_size = 0;
    };

    // One subresource converted on the GPU (texture_detile_cs.glsl): its raw
    // guest bytes at raw_offset in payload become the converted bytes at
    // destination_offset (the offsets mip_levels describe).
    struct DetileJob {
      size_t raw_offset = 0;
      size_t destination_offset = 0;
      uint32_t block_width = 0;
      uint32_t block_height = 0;
      uint32_t depth = 1;
      uint32_t pitch_h = 0;
      uint32_t pitch_v = 0;
      uint32_t flags = 0;  // bit 0 tiled, bit 1 3D, bits 4-5 endian, bits 8-12 log2(block bytes)
      uint32_t packed_x = 0;
      uint32_t packed_y = 0;
      uint32_t destination_pitch = 0;
      uint32_t destination_slice = 0;
    };

    uint32_t handle = 0;
    mutable uint32_t keepalive_serial = 0;
    uint64_t content_hash = 0;
    uint64_t generation = 0;
    xenos::xe_gpu_texture_fetch_t fetch{};
    TextureInfo info{};
    // With detile_jobs, payload holds raw guest bytes (gta4_native_gpu_detile)
    // and the converted texels (converted_payload_size bytes) are produced on
    // the GPU before the image copies.
    std::vector<uint8_t> payload;
    std::vector<DetileJob> detile_jobs;
    size_t converted_payload_size = 0;
    // gta4_native_gpu_detile_validate: the CPU conversion, compared with the
    // GPU's output once its frame has completed.
    std::shared_ptr<const std::vector<uint8_t>> detile_reference;
    std::vector<MipLevel> mip_levels;
    NativeTextureOrigin origin = NativeTextureOrigin::kGuestSnapshot;
    uint64_t virtual_lifetime = 0;
    bool gpu_produced = false;
    std::shared_ptr<const NativeTextureResource> packed_depth_source;
    bool vector_font_replacement = false;
    uint32_t vector_font_id = 0;
    // Render worker only: the assembly epoch in which this resource's
    // generations were last added to assembly_texture_protection_.
    mutable uint32_t assembly_protection_epoch = 0;
    // The latest assembly epoch whose commands read this resource, written by
    // the render worker (atomic_ref) and read by the recorder; never decreases.
    mutable uint32_t assembly_read_epoch = 0;
    // 49: RenderGuest only. The queue protection epoch this resource's
    // generations were last added to (ProtectInQueueEpoch).
    mutable uint32_t queue_protection_epoch = 0;
  };

  struct SynchronousCommand {
    std::mutex mutex;
    std::condition_variable condition;
    TextureLockResult result{};
    bool complete = false;
    bool succeeded = false;
  };

  struct NativeDeviceSnapshot {
    std::shared_ptr<const std::vector<uint8_t>> storage;

    size_t size() const { return storage ? storage->size() : 0; }
    const uint8_t* data() const { return storage ? storage->data() : nullptr; }
    const uint8_t& operator[](size_t index) const { return (*storage)[index]; }
    operator const std::vector<uint8_t>&() const {
      static const std::vector<uint8_t> kEmpty;
      return storage ? *storage : kEmpty;
    }
  };

  struct NativeShaderState {
    std::shared_ptr<const ConstantStateVersion> vertex_constants;
    std::shared_ptr<const ConstantStateVersion> pixel_constants;
    uint32_t vertex_booleans = 0;
    uint32_t pixel_booleans = 0;
    StateVersionVector versions{};
  };

  struct NativeShaderConstantDelta {
    ConstantPayloadDelta vertex_constants;
    ConstantPayloadDelta pixel_constants;
    std::array<uint32_t, 2> booleans{};
    bool booleans_present = false;
    bool compare_snapshot = false;
  };

  struct NativeDeviceConstantState {
    NativeDeviceConstantState(size_t vertex_size, size_t pixel_size)
        : vertex_constants(vertex_size), pixel_constants(pixel_size) {}

    AuthoritativeConstantState vertex_constants;
    AuthoritativeConstantState pixel_constants;
    AuthoritativeScalarState<std::array<uint32_t, 2>> booleans;
    StateVersionVector versions{};
  };

  struct NativeRoomLightInputBinding {
    uint32_t texture_stage = UINT32_MAX;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint64_t image_lifetime = 0;
    VkSamplerCreateInfo sampler_info{};
  };

  struct NativeTextureImage;
  struct CaptureKeepAlive;
  struct SnapshotKeepAlive;
  struct alignas(64) NativeCommand {
    // User-provided so pool construction does not zero the whole ~3 KB
    // command before running the member initializers below.
    NativeCommand() {}
    // Fast recycling in place: resets primitive state, clears vectors (preserving
    // their capacity up to a bound), and resets only non-null shared_ptrs.
    // Avoids destructor calls (~NativeCommand), je_malloc in CaptureConstantPayloadDelta,
    // and cross-thread je_free churn on GtaCmdRetirer.
    // 32: the lines Reset() touches, pulled in ahead of time by the retirer
    // (an in-order Cortex-A55 stalls on every miss; the commands were last
    // written on the Big cores). Masked arrays are fetched one command later,
    // once the line with their masks has arrived.
    void PrefetchForRecycle() const {
      // Read-prefetch (rw=0) lines that are only read or conditionally reset,
      // avoiding exclusive cache-line invalidations from Cortex-A55 cores.
      const auto read_line = [](const void* p) { __builtin_prefetch(p, 0, 1); };
      // 48: line 0 (masks) and the owned-reference zone, contiguous.
      read_line(&used_texture_mask);
      const auto* owned = reinterpret_cast<const uint8_t*>(&shader_state);
      const auto* owned_end = reinterpret_cast<const uint8_t*>(&extra_capture_keepalives + 1);
      for (; owned < owned_end; owned += 64) read_line(owned);
      read_line(reinterpret_cast<const uint8_t*>(&bytes) + sizeof(bytes) - 1);
      read_line(&shader_constant_delta);
      read_line(&shader_constant_delta.pixel_constants);
      read_line(&pending_state);
      read_line(&device_snapshot);
      read_line(&room_light_input_bindings);
    }
    // 48: NativeCommandPool, one allocation ahead: the lines the producer
    // writes for every command, for write: the reset zone (lines 0-4) and the
    // title bytes / constant deltas / absorbed state (lines 10-15).
    static void PrefetchForAcquire(const void* storage) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
      constexpr size_t kResetEnd = offsetof(NativeCommand, draw_descriptor_sets);
      constexpr size_t kBytesBegin = offsetof(NativeCommand, bytes);
      constexpr size_t kBytesEnd = offsetof(NativeCommand, payload);
#pragma clang diagnostic pop
      const auto* base = static_cast<const uint8_t*>(storage);
      // 54: with zero-allocation PrepareForAcquire creates lines 0-4 without reading them;
      // fetching them here would only take miss buffers (and DRAM reads) for nothing.
      if (!g_native_zero_allocate)
        for (size_t offset = 0; offset < kResetEnd; offset += 64) __builtin_prefetch(base + offset, 1);
      for (size_t offset = kBytesBegin & ~size_t(63); offset < kBytesEnd; offset += 64)
        __builtin_prefetch(base + offset, 1);
    }
    void PrefetchMaskedForRecycle() const {
      for (uint32_t mask = used_texture_mask; mask; mask &= mask - 1) {
        const uint32_t stage = uint32_t(std::countr_zero(mask));
        if (stage >= textures.size()) break;
        __builtin_prefetch(&textures[stage], 1, 1);
        __builtin_prefetch(&texture_fetches[stage], 1, 1);
      }
      for (uint32_t mask = captured_stream_mask; mask; mask &= mask - 1) {
        const uint32_t stream = uint32_t(std::countr_zero(mask));
        if (stream >= vertex_buffers.size()) break;
        __builtin_prefetch(&vertex_buffers[stream], 1, 1);
      }
    }

    // Phase 1 (runs on GtaCmdRetirer on Little cores): release owned resources
    // and clear used masked slots without dirtying scalar header/tail cache
    // lines that RenderGuest will immediately overwrite on Big/Prime cores.
    void RetireResources() {
      const uint32_t texture_mask = used_texture_mask;
      const uint32_t stream_mask = captured_stream_mask;
      for (uint32_t mask = texture_mask; mask; mask &= mask - 1) {
        const uint32_t stage = uint32_t(std::countr_zero(mask));
        if (stage < texture_fetches.size()) texture_fetches[stage] = xenos::xe_gpu_texture_fetch_t{};
      }
      if (!bytes.empty()) bytes.resize(0);

      constexpr size_t kMaxKeptBytes = 16384;
      const auto clear_keeping_capacity = [](auto& vec) {
        if (!vec.empty()) vec.clear();
        if (vec.capacity() * sizeof(vec[0]) > kMaxKeptBytes) {
          vec.shrink_to_fit();
        }
      };
      clear_keeping_capacity(payload);
      clear_keeping_capacity(pending_state);
      clear_keeping_capacity(shader_constant_delta.vertex_constants.ranges);
      clear_keeping_capacity(shader_constant_delta.vertex_constants.payload);
      clear_keeping_capacity(shader_constant_delta.pixel_constants.ranges);
      clear_keeping_capacity(shader_constant_delta.pixel_constants.payload);

      if (device_snapshot.storage) device_snapshot.storage.reset();
      if (room_light_input_bindings) room_light_input_bindings.reset();
      if (shader_state) shader_state.reset();
      if (pipeline_state) pipeline_state.reset();
      if (index_buffer) index_buffer.reset();
      if (environmental_data) environmental_data.reset();
      if (fire_trace) fire_trace.reset();
      if (bulb_trace) bulb_trace.reset();
      if (phone_trace) phone_trace.reset();
      if (tv_trace) tv_trace.reset();
      if (resolve_destination) resolve_destination.reset();
      if (depth_handoff_source) depth_handoff_source.reset();
      if (present_source) present_source.reset();
      if (postfx_half_scene) postfx_half_scene.reset();
      if (temporal_instance) temporal_instance.reset();
      if (temporal_prefilter) temporal_prefilter.reset();
      if (synchronous) synchronous.reset();
      if (fixed_owner) fixed_owner.reset();  // 48

      for (uint32_t mask = stream_mask; mask; mask &= mask - 1) {
        const uint32_t stream = uint32_t(std::countr_zero(mask));
        if (stream < vertex_buffers.size()) vertex_buffers[stream].reset();
      }
      for (uint32_t mask = texture_mask; mask; mask &= mask - 1) {
        const uint32_t stage = uint32_t(std::countr_zero(mask));
        if (stage < textures.size()) textures[stage].reset();
      }
#ifndef NDEBUG
      for (auto& vb : vertex_buffers) if (vb) vb.reset();
      for (auto& tex : textures) if (tex) tex.reset();
#endif
      // Owners are released last, after every non-owning slot above is cleared.
      if (snapshot_keepalive) snapshot_keepalive.reset();
      if (capture_keepalive) capture_keepalive.reset();
      if (!extra_capture_keepalives.empty()) extra_capture_keepalives.clear();
    }

    void HoldCaptureKeepAlive(const std::shared_ptr<CaptureKeepAlive>& keepalive) {
      if (!keepalive || capture_keepalive.get() == keepalive.get()) return;
      if (!capture_keepalive) {
        capture_keepalive = keepalive;
        return;
      }
      for (const auto& extra : extra_capture_keepalives) {
        if (extra.get() == keepalive.get()) return;
      }
      extra_capture_keepalives.push_back(keepalive);
    }

    // Phase 2 (runs on RenderGuest in NativeCommandPool::Make on Big/Prime
    // cores): initialize scalar fields directly in the producer's L1 cache.
    void PrepareForAcquire() {
      // 54: every field of lines 0-4 (up to draw_descriptor_sets) is reassigned below, so
      // their old contents are dead: allocate them zeroed instead of reading them from DRAM
      // (native_zero_allocate.h). Only whole lines below draw_descriptor_sets (the first field
      // not reset here) are zeroed.
      if (g_native_zero_allocate) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
        constexpr size_t kResetEnd = offsetof(NativeCommand, draw_descriptor_sets);
#pragma clang diagnostic pop
        ZeroAllocateLines(this, kResetEnd, kResetEnd / 64);
      }
      type = CommandType::kPresent;
      bindings_prepared = false;
      temporal_composite_mode = 0;
      split_postfx_applied = false;
      render_phase = RenderPhase::kUnknown;
      render_phase_object = 0;
      used_texture_mask = 0;
      failed_texture_mask = 0;
      realized_image_mask = 0;
      realized_sampler_mask = 0;
      guest_null_texture_mask = 0;
      resolve_swap = UINT32_MAX;
      temporal_scene_stage = UINT32_MAX;
      temporal_scene_binding = nullptr;
      texture_inputs_signature = 0;
      pipeline_memo_signature = 0;
      vertex_constants_upload_hash = 0;
      pixel_constants_upload_hash = 0;
      vertex_constants_upload_bytes = 0;
      pixel_constants_upload_bytes = 0;
      captured_stream_mask = 0;
      worker_vertex_constants_offset = 0;
      worker_pixel_constants_offset = 0;
      worker_ring_serial = 0;
      descriptor_page = UINT32_MAX;
      queue_epoch = nullptr;  // 49
      descriptor_copy = 0;
      image_descriptor_epoch = 0;
      sampler_descriptor_epoch = 0;
      cached_descriptor_epoch = 0;
      environmental_data_hash = 0;
      fixed_function_state = {};  // 48: shared blocks; stale state is never read
      snapshot_targets = {};
      light_trace_id = 0;
      light_trace_technique = 0xFFFFFFFFu;
      light_trace_mode = 0;
      dirty_components = 0;
      captured_fixed_function_state_hash = 0;
      recorded_fixed_function_state_hash = 0;
      captured_vertex_constants_hash = 0;
      captured_pixel_constants_hash = 0;
      vertex_constants_hash = 0;
      pixel_constants_hash = 0;
      released_texture_generation = 0;
      diagnostic_submit_sequence = 0;
      diagnostic_producer_epoch = 0;
      protected_generation_count = 0;
      protected_generations_overflow = false;
      protected_generations_collected = false;
      gpu_pass_origin = GpuPassOrigin{};
      profile_transport = profile::CommandTransport{};
      shader_constant_delta.vertex_constants.complete_snapshot = false;
      shader_constant_delta.pixel_constants.complete_snapshot = false;
      shader_constant_delta.vertex_constants.changed_only = false;  // 52
      shader_constant_delta.pixel_constants.changed_only = false;
      shader_constant_delta.booleans_present = false;
      shader_constant_delta.compare_snapshot = false;
    }

    void Reset() {
      RetireResources();
      PrepareForAcquire();
    }

    void RecycleInPlace() {
      RetireResources();
    }

    // 48 (тонкая команда, часть 1). Layout by who touches what, per command:
    //   lines 0-4   every scalar PrepareForAcquire resets (the producer writes 5
    //               contiguous lines instead of 11 scattered ones; the retirer
    //               never writes them), with the state-block pointers;
    //   lines 5-9   owned references, released by RetireResources (the lines the
    //               retirer writes); typical-draw ones first;
    //   then        the title command bytes, constant deltas and absorbed state
    //               (written by the producer for every draw, read by the worker);
    //   then        the per-stage arrays, read per used stream / stage only;
    //   last        cold sidecars.
    // The fixed-function state (260 B) and the render-target snapshot (180 B) are
    // shared blocks (NativeStateRef); the lighting context is read from the title
    // command bytes (lighting()). alignas(64): a command starts on a cache line,
    // and pool slots never share one.
    CommandType type = CommandType::kPresent;
    bool bindings_prepared = false;
    uint8_t temporal_composite_mode = 0;
    // Render-worker-only result; captured guest state stays immutable.
    mutable bool split_postfx_applied = false;
    mutable uint8_t protected_generation_count = 0;
    RenderPhase render_phase = RenderPhase::kUnknown;
    uint32_t render_phase_object = 0;
    uint32_t used_texture_mask = 0;
    uint32_t failed_texture_mask = 0;
    uint32_t realized_image_mask = 0;
    uint32_t realized_sampler_mask = 0;
    uint32_t guest_null_texture_mask = 0;
    // Index into planned_resolve_swaps_ when PrepareFrameTextures planned this
    // resolve as an image swap (A2); UINT32_MAX otherwise.
    uint32_t resolve_swap = UINT32_MAX;
    uint32_t temporal_scene_stage = UINT32_MAX;
    // 32: streams whose vertex_buffers[] slot the capture filled (it is the
    // only writer), so Reset() visits only those instead of all 16 slots.
    uint32_t captured_stream_mask = 0;
    DirtyStateComponentMask dirty_components = 0;
    mutable bool protected_generations_overflow = false;
    // Filled before the queue lock is taken (CollectTextureProtection).
    mutable bool protected_generations_collected = false;
    uint32_t light_trace_id = 0;
    uint32_t light_trace_technique = 0xFFFFFFFFu;
    uint32_t light_trace_mode = 0;
    // Dynamic state and the pipeline's fixed state (shared block, see above).
    NativeStateRef<NativeFixedFunctionState> fixed_function_state;
    // Render targets bound at capture (shared block): colors / depth.
    NativeStateRef<NativeTargetSnapshot> snapshot_targets;
    NativeTextureImage* temporal_scene_binding = nullptr;
    // Hash of the texture inputs PrepareFrameTextures compares between draws
    // (used mask; per used stage the pipeline's texture handle, the texture
    // resource and its fetch words), taken on the render worker. 0 = none.
    uint64_t texture_inputs_signature = 0;
    // Hash of NativePipelineMemoState(fixed_function_state), taken on the
    // render worker, so the recorder's pipeline memo compares one word instead
    // of the whole fixed state. 0 = none (anything that edits a command's
    // fixed state afterwards must clear it).
    uint64_t pipeline_memo_signature = 0;
    // Hash of the constant prefix this draw uploads, taken on the render
    // worker so the recorder (the slowest stage) need not; 0 bytes: none.
    uint64_t vertex_constants_upload_hash = 0;
    uint64_t pixel_constants_upload_hash = 0;
    uint32_t vertex_constants_upload_bytes = 0;
    uint32_t pixel_constants_upload_bytes = 0;
    // Constants the render worker already wrote into its constant ring
    // (WriteWorkerRingConstants): byte offsets into the ring, valid only for
    // ring frame worker_ring_serial; 0 = the recorder's frame arena path.
    uint32_t worker_vertex_constants_offset = 0;
    uint32_t worker_pixel_constants_offset = 0;
    uint32_t worker_ring_serial = 0;
    uint32_t descriptor_page = UINT32_MAX;
    uint32_t descriptor_copy = 0;
    // Diagnostic-only producer identity. These fields are native sidecars,
    // not part of the title command ABI, and let traces prove whether a title
    // frame was split before its Present command reaches the render worker.
    uint32_t diagnostic_producer_epoch = 0;
    uint64_t diagnostic_submit_sequence = 0;
    uint64_t image_descriptor_epoch = 0;
    uint64_t sampler_descriptor_epoch = 0;
    uint64_t cached_descriptor_epoch = 0;
    uint64_t environmental_data_hash = 0;
    uint64_t captured_fixed_function_state_hash = 0;
    uint64_t recorded_fixed_function_state_hash = 0;
    uint64_t captured_vertex_constants_hash = 0;
    uint64_t captured_pixel_constants_hash = 0;
    uint64_t vertex_constants_hash = 0;
    uint64_t pixel_constants_hash = 0;
    uint64_t released_texture_generation = 0;
    GpuPassOrigin gpu_pass_origin{};
    profile::CommandTransport profile_transport;  // ends at byte 320: lines 0-4

    // Written by the worker's descriptor preparation (not reset per command).
    std::array<VkDescriptorSet, 1> draw_descriptor_sets{};
    // 49: the queue protection epoch that holds this command's texture
    // generations until the worker takes it (nullptr: the reference-counted
    // staging / queue indices, as before).
    NativeQueueProtectionEpochs::Epoch* queue_epoch = nullptr;
    // Owned references. Typical draws: the first four.
    std::shared_ptr<const NativeShaderState> shader_state;
    std::shared_ptr<const NativePipelineState> pipeline_state;
    std::shared_ptr<const NativeBufferResource> index_buffer;
    std::shared_ptr<CaptureKeepAlive> capture_keepalive;
    std::shared_ptr<SnapshotKeepAlive> snapshot_keepalive;
    // Checked for every command by the recorder's merge scans.
    std::shared_ptr<const FireTraceContext> fire_trace;
    std::shared_ptr<const BulbSourceSnapshot> bulb_trace;
    std::shared_ptr<PhoneTraceContext> phone_trace;
    std::shared_ptr<TvTraceContext> tv_trace;
    std::shared_ptr<const EnvironmentalDataV2> environmental_data;
    std::shared_ptr<const NativeTextureResource> resolve_destination;
    std::shared_ptr<const NativeTextureResource> depth_handoff_source;
    std::shared_ptr<const NativeTextureResource> present_source;
    std::shared_ptr<const NativeTextureResource> postfx_half_scene;
    std::shared_ptr<const TemporalCommand> temporal_instance;
    std::shared_ptr<NativeCommand> temporal_prefilter;
    std::shared_ptr<SynchronousCommand> synchronous;
    // 48: owner of a worker-side fixed-state variant (depth clamp, temporal
    // prefilter); fixed_function_state then points into it.
    std::shared_ptr<const NativeFixedFunctionState> fixed_owner;
    std::vector<std::shared_ptr<CaptureKeepAlive>> extra_capture_keepalives;

    NativeCommandBytes bytes;
    NativeShaderConstantDelta shader_constant_delta;
    // Raw title state commands (Set* bytes, each with its header) submitted
    // since the previous queued command. The worker applies them, in order,
    // before this command, exactly as if each had been queued on its own.
    std::vector<uint8_t> pending_state;
    std::vector<uint8_t> payload;
    NativeDeviceSnapshot device_snapshot;

    // Copied into the shared-constant key for every draw.
    std::array<uint32_t, kTextureStageCount> texture_descriptor_indices{};
    std::array<uint32_t, kTextureStageCount> sampler_descriptor_indices{};
    // Read per used stream / stage only.
    std::array<std::shared_ptr<const NativeBufferResource>, kVertexStreamCount> vertex_buffers{};
    std::array<std::shared_ptr<const NativeTextureResource>, kTextureStageCount> textures{};
    std::array<NativeBindingRealization, kTextureStageCount> binding_realization{};
    std::array<xenos::xe_gpu_texture_fetch_t, kTextureStageCount> texture_fetches{};
    // Texture generations retained for this command when it was queued, so
    // the worker's release walks a dense array instead of chasing every
    // texture slot's resource again (under the queue lock).
    // 49: 14, not 16: room for queue_epoch without one more cache line (with
    // epoch protection the array is not used; more generations: overflow path).
    static constexpr uint32_t kProtectedGenerationCapacity = 14;
    mutable std::array<uint64_t, kProtectedGenerationCapacity> protected_generations;
    // Diagnostic-only heap sidecar captured by normal descriptor realization.
    std::shared_ptr<std::vector<NativeRoomLightInputBinding>> room_light_input_bindings;

    // 48: the lighting context is part of the title draw command (bytes); a
    // second 72-byte copy per command is gone. Non-draw commands: default.
    const LightingContext& lighting() const {
      static constexpr LightingContext kNoLighting{};
      size_t offset = 0;
      switch (type) {
        case CommandType::kDrawPrimitive:
          offset = offsetof(DrawPrimitiveCommand, lighting);
          break;
        case CommandType::kDrawPrimitiveUp:
          offset = offsetof(DrawPrimitiveUpCommand, lighting);
          break;
        case CommandType::kDrawIndexedPrimitive:
          offset = offsetof(DrawIndexedPrimitiveCommand, lighting);
          break;
        default:
          return kNoLighting;
      }
      if (bytes.size() < offset + sizeof(LightingContext)) return kNoLighting;
      return *reinterpret_cast<const LightingContext*>(bytes.data() + offset);
    }
  };

  struct NativeUploadBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceAddress device_address = 0;
    VkDeviceSize capacity = 0;
    VkDeviceSize allocation_size = 0;
    VkDeviceSize write_offset = 0;
    uint32_t memory_type = UINT32_MAX;
  };

  struct NativeRoomLightInputTexture {
    NativeRoomLightInputBinding binding;
    uint32_t probe_stage = UINT32_MAX;
    uint32_t descriptor_index = 0;
    uint32_t sampler_index = 0;
    uint32_t handle = 0;
    uint32_t address = 0;
    uint64_t generation = 0;
    uint64_t content_hash = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampleCountFlagBits image_samples = VK_SAMPLE_COUNT_1_BIT;
    VkComponentMapping view_components{};
    VkImageSubresourceRange view_range{};
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t bytes_per_sample = 0;
    uint32_t block_extent = 1;
    uint32_t swizzle = 0;
    std::array<uint32_t, 6> fetch{};
    std::string status = "not-recorded";
    uint32_t packed_source_handle = 0;
    uint64_t packed_source_generation = 0;
    uint64_t packed_source_lifetime = 0;
    uint64_t packed_depth_serial = 0;
    uint64_t packed_stencil_serial = 0;
    uint32_t packed_resolve_frame = 0;
    bool packed_source_resolved = false;
  };

  struct NativeRoomLightInputs {
    std::shared_ptr<NativeRoomLightProbeReceipt> receipt;
    uint32_t frame = 0;
    uint32_t command_index = UINT32_MAX;
    uint32_t context = 0;
    uint32_t required_mask = 0;
    uint32_t captured_mask = 0;
    uint32_t recorded_required_mask = 0;
    uint32_t vertex_texture_mask = 0;
    uint32_t pixel_texture_mask = 0;
    uint32_t descriptor_backend = 0;
    uint32_t descriptor_page = UINT32_MAX;
    uint32_t descriptor_copy = 0;
    uint64_t image_descriptor_epoch = 0;
    uint64_t sampler_descriptor_epoch = 0;
    uint64_t cached_descriptor_epoch = 0;
    uint64_t draw_id = 0;
    uint64_t export_id = 0;
    uint64_t vertex_constants_hash = 0;
    uint64_t pixel_constants_hash = 0;
    uint32_t vertex_booleans = 0;
    uint32_t pixel_booleans = 0;
    std::shared_ptr<const std::vector<uint8_t>> vertex_constants;
    std::shared_ptr<const std::vector<uint8_t>> pixel_constants;
    std::vector<NativeRoomLightInputTexture> textures;
    bool scheduled_complete = false;
    std::string failure;
  };

  struct NativeContentProbeStage {
    std::shared_ptr<BulbProbeIdentity> bulb;
    std::shared_ptr<PhoneProbeIdentity> phone;
    std::shared_ptr<TvProbeIdentity> tv;
    bool valid = false;
    bool reserved = false;
    uint8_t kind = 0;
    uint8_t command_type = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_level = 0;
    uint32_t handle = 0;
    uint32_t address = 0;
    uint32_t command_index = UINT32_MAX;
    uint32_t texture_stage = UINT32_MAX;
    uint32_t checkpoint_ordinal = UINT32_MAX;
    uint32_t checkpoint_count = 0;
    uint32_t render_phase = 0;
    uint64_t draw_id = 0;
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
    uint32_t trace_wrapper = 0;
    uint32_t sample_count = 0;
    uint8_t vertical_band = 0;
    uint8_t vertical_band_count = 1;
    uint64_t resource_generation = 0;
    uint32_t provenance_handle = 0;
    uint32_t provenance_frame = 0;
    uint32_t provenance_command = UINT32_MAX;
    uint64_t provenance_serial = 0;
    uint32_t provenance_phase = 0;
    uint32_t provenance_kind = 0;
    NativeBlackAnomalyPolicy black_anomaly_policy = NativeBlackAnomalyPolicy::kIgnore;
    uint8_t reflection_family = 0;
    uint8_t reflection_role = 0;
    std::array<uint32_t, 4> texture_handles{};
    std::string_view diagnostic_category;
    std::string_view diagnostic_role;
  };

  // An array of probe stages held through a pointer: the frame-slot switch
  // swaps four pairs of probe buffers every frame, and swapping the inline
  // array moved 256 stages of three shared_ptrs each (~2% of the recorder).
  // A null pointer stands for all-default stages; access allocates.
  template <typename Stage, size_t Count>
  class NativeProbeStageArray {
   public:
    using Array = std::array<Stage, Count>;
    NativeProbeStageArray() = default;
    NativeProbeStageArray(const NativeProbeStageArray& other)
        : data_(other.data_ ? std::make_unique<Array>(*other.data_) : nullptr) {}
    NativeProbeStageArray(NativeProbeStageArray&& other) noexcept = default;
    NativeProbeStageArray& operator=(const NativeProbeStageArray& other) {
      if (this != &other) data_ = other.data_ ? std::make_unique<Array>(*other.data_) : nullptr;
      return *this;
    }
    NativeProbeStageArray& operator=(NativeProbeStageArray&& other) noexcept {
      data_.swap(other.data_);
      return *this;
    }
    friend void swap(NativeProbeStageArray& a, NativeProbeStageArray& b) noexcept {
      a.data_.swap(b.data_);
    }
    static constexpr size_t size() { return Count; }
    Stage& operator[](size_t index) { return Mutable()[index]; }
    const Stage& operator[](size_t index) const { return View()[index]; }
    auto begin() { return Mutable().begin(); }
    auto end() { return Mutable().end(); }
    auto begin() const { return View().begin(); }
    auto end() const { return View().end(); }

   private:
    Array& Mutable() {
      if (!data_) data_ = std::make_unique<Array>();
      return *data_;
    }
    const Array& View() const {
      static const Array kDefault{};
      return data_ ? *data_ : kDefault;
    }
    std::unique_ptr<Array> data_;
  };

  struct NativeContentProbeBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceSize allocation_size = 0;
    uint32_t memory_type = UINT32_MAX;
    uint32_t pending_frame = 0;
    NativeProbeStageArray<NativeContentProbeStage, 256> stages{};
    std::shared_ptr<NativeRoomLightInputs> room_light_inputs;
  };

  struct NativeLightStencilHistogramStage {
    bool valid = false;
    bool before_setup = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t command_index = UINT32_MAX;
    uint32_t light_trace_id = 0;
    uint32_t light_trace_technique = 0xFFFFFFFFu;
    uint32_t light_trace_mode = 0;
    uint32_t depth_handle = 0;
    uint32_t depth_address = 0;
    uint64_t actual_image = 0;
    uint64_t surface_lifetime = 0;
    std::shared_ptr<NativeRoomLightProbeReceipt> room_receipt;
    uint64_t bulb_instance_id = 0;
    uint64_t draw_id = 0;
    uint64_t light_trace_record = 0;
    uint64_t vertex_shader_hash = 0;
    NativeFixedFunctionState fixed_function{};
  };

  struct NativeLightStencilHistogramBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceSize capacity = 0;
    VkDeviceSize copy_stride = 0;
    VkDeviceSize allocation_size = 0;
    uint32_t memory_type = UINT32_MAX;
    uint32_t pending_frame = 0;
    std::array<NativeLightStencilHistogramStage, 2> stages{};
  };

  struct NativeLightColorDeltaStage {
    bool before_valid = false;
    bool after_valid = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t command_index = UINT32_MAX;
    uint32_t context = 0;
    uint32_t target_handle = 0;
    uint32_t target_address = 0;
    uint32_t recorded_target_handle = 0;
    uint32_t recorded_target_address = 0;
    uint32_t effective_mode = UINT32_MAX;
    uint64_t actual_image = 0;
    uint64_t surface_lifetime = 0;
    std::shared_ptr<NativeRoomLightProbeReceipt> room_receipt;
    uint32_t light_trace_technique = 0xFFFFFFFFu;
    uint32_t light_trace_mode = 0;
    uint64_t instance_id = 0;
    uint64_t draw_id = 0;
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
  };

  struct NativeLightColorDeltaBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceSize capacity = 0;
    VkDeviceSize copy_stride = 0;
    VkDeviceSize allocation_size = 0;
    uint32_t memory_type = UINT32_MAX;
    uint32_t pending_frame = 0;
    NativeLightColorDeltaStage stage{};
  };

  static constexpr uint32_t kTranslucentQueryCapacity = 1024;

  struct NativeTranslucentQuery {
    bool valid = false;
    bool draw_recorded = false;
    std::shared_ptr<NativeRoomLightProbeReceipt> room_receipt;
    NativeRoomLightProbeKey room_key;
    uint32_t effective_mode = UINT32_MAX;
    uint32_t frame = 0;
    uint32_t command_index = UINT32_MAX;
    uint32_t render_phase = 0;
    uint64_t draw_id = 0;
    uint32_t light_trace_id = 0;
    uint32_t light_trace_technique = 0xFFFFFFFFu;
    uint32_t light_trace_mode = 0;
    uint64_t light_instance_id = 0;
    uint64_t light_trace_record = 0;
    uint32_t shader_override_pair_id = 0;
    uint64_t shader_variant_key = 0;
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
    uint32_t pixel_shader_specialization_constants_mask = 0;
    uint32_t pixel_shader_specialization_value = 0;
    bool pixel_shader_uses_late_module = false;
    uint32_t target_handle = 0;
    uint32_t target_address = 0;
    uint32_t depth_handle = 0;
    uint32_t depth_address = 0;
    NativeFixedFunctionState fixed_function{};
    std::string category;
    std::string variant;
    std::string shader_filename;
  };

  struct NativeTranslucentQueryState {
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t pending_frame = 0;
    uint32_t pending_count = 0;
    // Frame-slot swaps must not materialize the full census on the render
    // worker's stack. Keep fixed-capacity indexed storage on the heap; moving
    // or swapping the state transfers only vector ownership.
    std::vector<NativeTranslucentQuery> queries =
        std::vector<NativeTranslucentQuery>(kTranslucentQueryCapacity);
  };
  static_assert(sizeof(NativeTranslucentQueryState) <= 64,
                "Query state must remain small enough for safe frame-slot swaps");

  static constexpr uint32_t kNativeGpuProfileQueryCapacity =
      uint32_t(performance::kMaximumGpuQueriesPerFrame);

  struct NativeGpuProfileSlice {
    performance::GpuRange range = performance::GpuRange::kUnattributed;
    uint32_t begin_query = UINT32_MAX;
    uint32_t end_query = UINT32_MAX;
    uint32_t attribution_region = UINT32_MAX;
  };

  struct NativeGpuProfileQueryResult {
    uint64_t timestamp = 0;
    uint64_t available = 0;
  };

  struct NativeGpuProfileFramePayload {
    VkQueryPool pool = VK_NULL_HANDLE;
    bool active = false;
    bool pending = false;
    bool detailed_gpu = false;
    uint32_t query_budget = 0;
    profile::FrameDetail detail;
    uint32_t frame = 0;
    uint32_t query_count = 0;
    uint32_t dropped_spans = 0;
    uint32_t frame_begin_query = UINT32_MAX;
    uint32_t current_range_begin_query = UINT32_MAX;
    performance::GpuRange current_range = performance::GpuRange::kUnattributed;
    uint32_t current_attribution_region = UINT32_MAX;
    uint32_t detail_boundary_budget = 0;
    uint32_t detail_boundary_count = 0;
    uint32_t dropped_attribution_regions = 0;
    std::array<NativeGpuProfileSlice, performance::kMaximumGpuQueriesPerFrame> slices{};
    size_t slice_count = 0;
    std::array<NativeGpuProfileQueryResult, performance::kMaximumGpuQueriesPerFrame>
        query_results{};
    performance::FrameBuilder sample_builder;
    uint64_t capture_sequence = 0;
    uint64_t cpu_frame_interval_ticks = 0;
    uint64_t cpu_housekeeping_ticks = 0;
    uint64_t cpu_slot_cleanup_ticks = 0;
    uint64_t cpu_profile_readback_ticks = 0;
    uint64_t cpu_upload_capacity_ticks = 0;
    uint64_t cpu_command_setup_ticks = 0;
    uint64_t cpu_texture_prepare_ticks = 0;
    uint64_t cpu_record_ticks = 0;
    uint64_t cpu_finalize_ticks = 0;
    uint64_t cpu_submit_ticks = 0;
    uint64_t cpu_callback_ticks = 0;
    uint64_t cpu_publish_ticks = 0;
    VkDeviceSize upload_bytes = 0;
    attribution::NativeAttributionPlan attribution_plan;
    std::optional<attribution::NativeDrilldownReservation> attribution_reservation;
  };

  struct NativeGpuProfileState {
    float timestamp_period_ns = 0.0f;
    uint32_t timestamp_valid_bits = 0;
    bool support_checked = false;
    bool supported = false;
    bool capture_complete = false;
    bool capture_armed = false;
    bool export_started = false;
    uint32_t last_sampled_frame = 0;
    std::array<NativeGpuProfileFramePayload, NativeFrameContextRing::kSlotCount> frames{};
    performance::FrameSampleCompletionQueue completed_samples;
    attribution::NativeAttributionCompletionQueue completed_attribution;
    attribution::NativeDrilldownScheduler drilldown_scheduler;
    uint64_t next_capture_sequence = 1;
    uint64_t next_publish_sequence = 1;
    performance::FrameSampleRing sample_ring;
    std::array<performance::FrameSample, performance::kFrameSampleCapacity> export_samples{};
    size_t export_sample_count = 0;
    std::thread export_thread;
    profile::CaptureMetadata detail_metadata;
    std::vector<profile::FrameDetail> detail_frames;
    std::array<std::optional<profile::FrameDetail>, NativeFrameContextRing::kSlotCount> completed_details;
    uint64_t last_publish_host_tick = 0;
  };

  struct NativeImageResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
    uint32_t memory_type = UINT32_MAX;
  };

  struct NativeTextureImage {
    std::shared_ptr<const NativeTextureResource> source;
    uint64_t descriptor_lifetime = 0;
    NativeImageResource resource;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkComponentMapping view_components{};
    VkImageSubresourceRange view_range{};
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t guest_mip_levels = 1;
    uint32_t mip_levels = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    VkDeviceSize allocation_size = 0;
    uint32_t memory_type = UINT32_MAX;
    uint32_t memory_heap = UINT32_MAX;
    bool memory_accounted = false;
    bool allocation_pool_compatible = false;
    NativeImageAllocationKey allocation_key{};
    NativeReflectionTarget reflection{};
    bool is_reflection = false;
    // Resolve destinations persist independently of their transient capture
    // attachments. Content validity applies to every GPU-produced texture;
    // reflections are only one consumer of this general contract.
    NativeResolvedTextureContentState content;
    std::unique_ptr<NativeResolveReuseRecord> identical_resolve;
    NativeSurfaceAspectContent aspect_content{};
    // Last submitted frame that referenced this image; eviction waits out
    // kNativeTextureEvictionGraceFrames beyond this.
    uint32_t last_used_frame = 0;
    uint64_t last_use_serial = 0;
    uint64_t last_used_submission = 0;
    uint32_t created_frame = 0;
    NativeDescriptorSlotHandle descriptor_slot{};
    bool descriptor_retirement_queued = false;
    bool descriptor_reclaimed = true;
    std::vector<VkImageView> mip_views;
    VkImageView packed_stencil_view = VK_NULL_HANDLE;
    NativeSurfaceAspectContent packed_source_content{};
    uint64_t packed_source_image_lifetime = 0;
  };

  struct NativeTextureHeapBudgets {
    bool available = false;
    uint32_t heap_count = 0;
    std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> usage{};
    std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> budget{};
  };

  struct NativeSamplerKey {
    xenos::TextureFilter min_filter = xenos::TextureFilter::kPoint;
    xenos::TextureFilter mag_filter = xenos::TextureFilter::kPoint;
    xenos::TextureFilter mip_filter = xenos::TextureFilter::kPoint;
    xenos::ClampMode clamp_u = xenos::ClampMode::kRepeat;
    xenos::ClampMode clamp_v = xenos::ClampMode::kRepeat;
    xenos::ClampMode clamp_w = xenos::ClampMode::kRepeat;
    xenos::AnisoFilter aniso_filter = xenos::AnisoFilter::kDisabled;
    xenos::BorderColor border_color = xenos::BorderColor::k_ABGR_Black;
    int32_t lod_bias = 0;
    uint32_t mip_min_level = 0;
    uint32_t mip_max_level = 0;

    bool operator==(const NativeSamplerKey&) const = default;
  };

  struct NativeSampler {
    NativeSamplerKey key{};
    VkSampler sampler = VK_NULL_HANDLE;
    NativeDescriptorSlotHandle descriptor_slot{};
    VkSamplerCreateInfo creation_info{};
  };

  struct NativeDescriptorImageSlot {
    uint64_t generation = 0;
    std::array<VkImageView, 4> views{};
    bool operator==(const NativeDescriptorImageSlot&) const = default;
  };

  struct NativeDescriptorSamplerSlot {
    uint64_t generation = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    bool operator==(const NativeDescriptorSamplerSlot&) const = default;
  };

  struct NativeDescriptorPage {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 1> descriptor_sets{};
    std::vector<NativeDescriptorImageSlot> image_slots;
    std::vector<NativeDescriptorSamplerSlot> sampler_slots;
    uint64_t frame_epoch = 0;
    uint32_t published_image_count = 0, published_sampler_count = 0;
    bool images_dirty = false, samplers_dirty = false;
    std::vector<VkDescriptorImageInfo> update_scratch;
    std::vector<NativeDescriptorImageSlot> published_images;
    std::vector<NativeDescriptorSamplerSlot> published_samplers;
    std::vector<VkWriteDescriptorSet> update_writes;
  };

  struct NativeDescriptorRetirement {
    NativeTextureImage* image = nullptr;
    NativeDescriptorSlotHandle handle{};
    uint64_t last_submission = 0;
  };

  enum class NativeFlightResourceKind : uint8_t {
    kCommandPool,
    kCommandBuffer,
    kDescriptorPool,
    kUploadBuffer,
    kTextureImage,
    kTextureView,
    kTextureMipView,
    kTextureDescriptorSlot,
    kSurfaceImage,
    kSurfaceView,
    kSurfaceSampledView,
    kSurfaceScratchBuffer,
    kPersistentBuffer,
  };

  struct NativeFlightResourceKey {
    NativeFlightResourceKind kind = NativeFlightResourceKind::kTextureImage;
    uint64_t object = 0;
    uint32_t page = 0;
    uint32_t index = 0;
    uint32_t generation = 0;

    auto operator<=>(const NativeFlightResourceKey&) const = default;
  };

  struct NativeFlightResourceReference {
    NativeFlightResourceKey key{};
    uint32_t guest_handle = 0;
    uint64_t guest_generation = 0;
  };

  struct NativeFlightSubmission {
    uint64_t submission = 0;
    uint32_t frame = 0;
    uint32_t slot = 0;
    std::map<NativeFlightResourceKey, NativeFlightResourceReference> resources;
  };

  struct NativeCachedDescriptorState;
  struct NativePipelineCompilerState;

  struct NativeSurfaceImage {
    uint64_t lifetime_id = 0;
    SurfaceDescriptor descriptor{};
    NativeImageResource resource;
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkBuffer depth_handoff_stencil_scratch_buffer = VK_NULL_HANDLE;
    VkDeviceMemory depth_handoff_stencil_scratch_memory = VK_NULL_HANDLE;
    VkDeviceSize depth_handoff_stencil_scratch_size = 0;
    VkDeviceSize depth_handoff_stencil_scratch_allocation_size = 0;
    uint32_t depth_handoff_stencil_scratch_memory_type = UINT32_MAX;
    VkDeviceSize allocation_size = 0;
    bool depth_handoff_stencil_scratch_initialized = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    NativeReflectionTarget reflection{};
    NativeReflectionCaptureState reflection_capture{};
    bool is_reflection = false;
    bool ever_written = false;
    NativeSurfaceAspectContent aspect_content{};
    GuestPlacementKey materialized_placement{};
    uint64_t materialized_serial = 0;
    uint32_t created_frame = 0;
    uint32_t last_used_frame = 0;
    uint64_t last_used_submission = 0;
    // Resolve swaps (A2) trade this image with a texture's. The texture-side
    // state that belongs to the image travels with it: its mip views and the
    // stable descriptor slot that samples it.
    std::vector<VkImageView> swap_mip_views;
    NativeDescriptorSlotHandle swap_descriptor_slot{};
    uint64_t swap_descriptor_lifetime = 0;
  };

  struct NativePlacementOwner {
    enum class WriteKind : uint8_t {
      kUnknown,
      kDrawColor,
      kDrawDepth,
      kImplicitAttachmentClear,
      kExplicitClear,
      kResolveClear,
      kExplicitDepthHandoff,
      kDrawStencil,
    };
    NativeSurfaceImage* image = nullptr;
    GuestSurfaceView view{};
    uint64_t serial = 0;
    uint32_t frame = 0;
    size_t command_index = SIZE_MAX;
    RenderPhase render_phase = RenderPhase::kUnknown;
    WriteKind write_kind = WriteKind::kUnknown;
  };

  struct NativeProducerDepthResolve {
    NativeSurfaceImage* source = nullptr;
    NativeTextureImage* destination = nullptr;
    NativeProducerDepthResolveStamp stamp{};
    bool attached = false;
    bool complete = false;
  };

  struct NativeResolveConversionPipeline {
    VkFormat destination_format = VK_FORMAT_UNDEFINED;
    enum class Kind : uint8_t {
      kResolve,
      kResolveMultisampled,
      kResolveHDRMirror,
      kResolveHDRMirrorMultisampled,
      kDepthResolveMultisampled,
      kDepthHandoff,
      kSceneDepthHandoff,
      kPackedDepthAlias,
    } kind = Kind::kResolve;
    VkSampleCountFlagBits destination_samples = VK_SAMPLE_COUNT_1_BIT;
    VkPipeline pipeline = VK_NULL_HANDLE;
  };

  struct NativeResolveConversionConstants {
    int32_t source_x = 0;
    int32_t source_y = 0;
    int32_t destination_x = 0;
    int32_t destination_y = 0;
    // Guest sample-space topology and physical Vulkan sample count are
    // intentionally distinct. Native quality overrides can back a guest 2x
    // placement view with a host 4x image.
    uint32_t source_guest_sample_type = 0;
    uint32_t requested_guest_sample_type = 0;
    uint32_t destination_guest_sample_type = 0;
    uint32_t sample_select = 0;
    uint32_t mode = 0;
    uint32_t physical_source_sample_type = 0;
    uint32_t physical_destination_sample_type = 0;
    uint32_t flags = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t destination_width = 0;
    uint32_t destination_height = 0;
  };
  static_assert(sizeof(NativeResolveConversionConstants) == 64);

  struct NativeHDRPresentConstants {
    int32_t source_width = 0;
    int32_t source_height = 0;
    int32_t destination_width = 0;
    int32_t destination_height = 0;
    float hdr_headroom = 1.0f;
    uint32_t output_mode = 0;
    uint32_t hdr_mode = 0;
    float paper_white_nits = 203.0f;
    float peak_nits = 400.0f;
    float shoulder_start = 0.0f;
    float shoulder_power = 2.5f;
    float sgsr_sharpness = 2.0f; // Добавили параметр резкости SGSR
  };

  struct NativeRenderingTarget {
    bool temporal_shader = false, temporal_motion = false, temporal_reactive = false,
         temporal_jitter = false;
    std::array<NativeSurfaceImage*, kRenderTargetCount> color_surfaces{};
    std::array<VkImageView, kRenderTargetCount> color_views{};
    std::array<VkFormat, kRenderTargetCount> color_formats{};
    NativeSurfaceImage* depth_surface = nullptr;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits guest_samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    // Realized attachments normally follow the command's access mask. A
    // stencil-only light setup may borrow already-active color attachments to
    // keep the pass open, while its zero write mask prevents ownership claims.
    // Inactive guest MRT aliases are never materialized for this optimization.
    uint32_t color_attachment_mask = 0;
    uint32_t color_write_mask = 0;
    bool depth_stencil_attachment_active = false;
    NativeReflectionTarget reflection{};
    bool is_reflection = false;
    bool uses_presenter = false;
  };

  struct NativePipelineKey {
    bool temporal_shader = false, temporal_motion = false, temporal_reactive = false;
    bool temporal_auxiliary = false;
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
    uint64_t shader_variant_key = 0;
    uint64_t vertex_declaration_hash = 0;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    std::array<uint32_t, kVertexStreamCount> vertex_strides{};
    std::array<VkFormat, kRenderTargetCount> color_formats{};
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool user_pointer = false;
    bool indexed_descriptors = false;
    uint32_t depth_enable = 0;
    uint32_t depth_function = 0;
    uint32_t depth_write_enable = 0;
    uint32_t depth_clamp_enable = 0;
    uint32_t negative_one_to_one_clip_space = 0;
    uint32_t cull_mode = 0;
    uint32_t polygon_mode = 0;
    uint32_t blend_enable_mask = 0;
    std::array<uint32_t, kRenderTargetCount> blend_controls{};
    uint32_t source_blend = 0;
    uint32_t destination_blend = 0;
    uint32_t blend_operation = 0;
    uint32_t source_blend_alpha = 0;
    uint32_t destination_blend_alpha = 0;
    uint32_t blend_operation_alpha = 0;
    uint32_t alpha_test_enable = 0;
    uint32_t alpha_function = 0;
    uint32_t alpha_to_mask_enable = 0;
    uint32_t stencil_enable = 0;
    uint32_t two_sided_stencil = 0;
    uint32_t stencil_fail = 0;
    uint32_t stencil_depth_fail = 0;
    uint32_t stencil_pass = 0;
    uint32_t stencil_function = 0;
    uint32_t stencil_mask = 0;
    uint32_t stencil_write_mask = 0;
    uint32_t ccw_stencil_fail = 0;
    uint32_t ccw_stencil_depth_fail = 0;
    uint32_t ccw_stencil_pass = 0;
    uint32_t ccw_stencil_function = 0;
    uint32_t color_write_mask = 0;
    uint32_t sample_mask = 0;
    bool depth_bias_enable = false;
    bool primitive_restart_enable = false;
    bool early_fragment_tests = false;
    bool colorless_fragment = false;
    bool empty_fragment = false;

    bool operator==(const NativePipelineKey&) const = default;
  };

  struct NativePipelineKeyHash {
    size_t operator()(const NativePipelineKey& key) const noexcept;
  };

  struct NativePipeline {
    NativePipelineKey key{};
    VkPipeline pipeline = VK_NULL_HANDLE;
  };

  struct NativeUploadAllocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceAddress device_address = 0;
    uint8_t* mapping = nullptr;
    const uint8_t* host_data = nullptr;
  };

  enum class NativePersistentBufferKind : uint8_t {
    kVertex,
    kIndex16,
    kIndex32,
    kTriangleFan16,
    kTriangleFan32,
    kVertexCacheIndex16,  // One draw range reordered for the vertex cache.
    kVertexCacheIndex32,
  };

  struct NativePersistentBufferKey {
    uint64_t generation = 0;
    uint64_t declaration_hash = 0;
    uint64_t shader_hash = 0;
    uint32_t stream = 0;
    uint32_t stream_offset = 0;
    uint32_t stride = 0;
    NativePersistentBufferKind kind = NativePersistentBufferKind::kVertex;

    bool operator==(const NativePersistentBufferKey&) const = default;
  };

  struct NativePersistentBufferKeyHash {
    size_t operator()(const NativePersistentBufferKey& key) const noexcept;
  };

  struct NativePersistentBufferBlock {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceAddress device_address = 0;
    VkDeviceSize capacity = 0;
    VkDeviceSize allocation_size = 0;
    uint32_t memory_type = UINT32_MAX;
  };

  struct NativePersistentBufferEntry {
    NativePersistentBufferKey key{};
    uint64_t allocation_id = 0;
    uint64_t block_id = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    uint64_t last_used_submission = 0;
    uint32_t last_used_frame = 0;
  };

  using NativeConstantBufferKind = FrameConstantKind;

  using NativeSharedConstantSemanticKey = SharedConstantSemanticKey<kTextureStageCount>;

  struct NativeSharedConstantSemanticKeyHash {
    size_t operator()(const NativeSharedConstantSemanticKey& key) const noexcept;
  };

  struct NativeFrameConstantArena {
    NativeUploadBuffer storage;
    FrameConstantArenaIndex index;
    NativeConstantUploadTracker uploads;
    bool compare_upload_bytes = false;
    FrameGenerationMap<NativeSharedConstantSemanticKey, uint64_t,
                       NativeSharedConstantSemanticKeyHash>
        shared_versions;
    NativeImmutableBindings<NativeUploadAllocation> immutable_bindings;
    uint64_t next_shared_identity = 1;
    // The previous draw's shared key and its identity: consecutive draws
    // usually share them. Valid until the arena resets (identities restart).
    NativeSharedConstantSemanticKey last_shared_key{};
    uint64_t last_shared_identity = 0;
  };

  enum class NativeUploadKind : uint8_t {
    kOther,
    kTexture,
    kVertex,
    kIndex,
    kVertexConstants,
    kPixelConstants,
    kSharedConstants,
    kDrawUp,
  };

  struct NativePushConstants {
    VkDeviceAddress vertex_constants = 0;
    VkDeviceAddress pixel_constants = 0;
    VkDeviceAddress shared_constants = 0;
  };

  struct NativeVertexUpload {
    uint64_t declaration_hash = 0;
    uint64_t shader_hash = 0;
    uint32_t stream = 0;
    uint32_t stream_offset = 0;
    uint32_t stride = 0;
    NativeUploadAllocation allocation{};
  };

  struct NativeFrameResources {
    FrameGenerationMap<NativePersistentBufferKey, NativeUploadAllocation, NativePersistentBufferKeyHash> vertex_uploads;
    FrameGenerationMap<uint64_t, NativeUploadAllocation> index16_uploads, index32_uploads;
    bool has_last_shared_key = false;
    NativeSharedConstantSemanticKey last_shared_key{};
    NativeUploadAllocation last_shared_allocation{};
    static constexpr size_t kSharedMruSize = 4;
    struct SharedMruEntry {
      bool valid = false;
      NativeSharedConstantSemanticKey key{};
      NativeUploadAllocation allocation{};
    };
    std::array<SharedMruEntry, kSharedMruSize> shared_mru{};
    uint32_t shared_mru_cursor = 0;
    bool Reset() {
      has_last_shared_key = false;
      last_shared_key = {};
      last_shared_allocation = {};
      for (auto& entry : shared_mru) entry.valid = false;
      shared_mru_cursor = 0;
      if (!vertex_uploads.CanResetGeneration() || !index16_uploads.CanResetGeneration() ||
          !index32_uploads.CanResetGeneration()) return false;
      return vertex_uploads.ResetGeneration() && index16_uploads.ResetGeneration() &&
             index32_uploads.ResetGeneration();
    }
  };

  // 56: deferred capture (gta4_native_capture_thread): native_capture_ring.h and
  // native_capture_thread.inc. What GtaCapture replays in place of guest memory.
  struct CaptureReplay {
    uint32_t device = 0;                     // guest device of the record
    const uint8_t* device_image = nullptr;   // GtaCapture's image of that device
    const uint8_t* up_payload = nullptr;     // DrawPrimitiveUp vertices copied at submit
    uint32_t up_size = 0;
    const GpuPassOrigin* origin = nullptr;   // gpu-pass envelope unpacked at submit
    // 57: render-target / depth surface objects as they were at submit (kSurfaceSlots).
    const capture::SurfaceWords* surfaces = nullptr;
  };
  struct CaptureThreadState;
  struct CaptureThreadStateDeleter {
    void operator()(CaptureThreadState* state) const;
  };
  bool CaptureTitleCommand(uint32_t abi_version, const void* command, size_t command_size,
                           const CaptureReplay* replay);
  int DeferTitleCommand(uint32_t abi_version, const void* command, size_t command_size);
  int CaptureSynchronousFallback();
  void DrainCaptureThread();
  void CaptureBarrier(uint32_t resource, uint32_t kind);  // kind 1 Lock, 2 final Release
  void StartCaptureThread();
  void StopCaptureThread();
  void CaptureThreadMain();
  void ReplayCaptureRecord(CaptureThreadState& state, const capture::RecordHeader& header);
  void ReportCaptureThread(CaptureThreadState& state);
  const uint8_t* CaptureDeviceMemory(uint32_t device, const CaptureReplay* replay) const;
  SurfaceDescriptor CaptureSurfaceDescriptor(uint32_t handle, const CaptureReplay* replay) const;
  std::unique_ptr<CaptureThreadState, CaptureThreadStateDeleter> capture_thread_;
  std::atomic<bool> capture_thread_active_{false};
  bool capture_thread_configured_ = false;

  bool ValidateAndCopyCommand(const void* command, size_t command_size,
                              NativeCommand& native_command,
                              const CaptureReplay* replay = nullptr);
  void TraceNativeRendererEvent(std::string_view point, std::string_view details = {});
  void ObserveNativeGpuProfileDraw(const NativeCommand& command, VkPipeline pipeline,
                                   VkSampleCountFlagBits samples, uint32_t vertices, uint32_t indices);
  NativeGpuScopeSummary* native_gpu_scope_summary_ = nullptr;
  // gpu_flight::IsEnabled() as of the frame being published (a call into the
  // runtime library otherwise, several times per draw).
  std::atomic<bool> native_flight_enabled_{false};

  bool NativeRendererEventTraceEnabled() const {
    return deterministic_trace_active_ || fire_event_active_ || bool(phone_frame_trace_) || bool(tv_frame_trace_);
  }
  NativeFixedFunctionState DecodeFixedFunctionState(std::span<const uint8_t> device_state) const;
  static uint64_t HashFixedFunctionState(const NativeFixedFunctionState& state);
  std::shared_ptr<const NativeTextureResource> CreateResolvedTextureResource(
      const ResolveCommand& command);
  std::shared_ptr<const NativeBufferResource> CaptureBufferResource(uint32_t handle);
  SurfaceDescriptor CaptureSurfaceDescriptor(uint32_t handle) const;
  std::shared_ptr<const NativeTextureResource> CaptureTextureResource(
      uint32_t handle, const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t stage);
  std::shared_ptr<const NativeTextureResource> CaptureTextureResourceUncached(
      uint32_t handle, const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t stage,
      bool& cacheable);
  void StartRenderWorker();
  void RenderWorkerMain();
  // 58: gta4_native_direct_record. No render worker thread: the thread that submits a command
  // runs the worker loop over it right away (kDirectPump), including the frame recording at
  // Present. kDirectShutdown runs what is left and the worker's shutdown tail.
  enum class WorkerLoopMode { kThread, kDirectPump, kDirectShutdown };
  void RenderWorkerLoop(WorkerLoopMode mode);
  // Runs the worker loop over what is queued; one pump at a time (direct_pump_mutex_). Gives
  // up when the worker stops (shutdown).
  void PumpDirectRecord();
  // 60: gta4_native_stream_record (with direct record). The frame is prepared and recorded
  // command by command while it is assembled, into this slot's command buffers; Present only
  // records the tail and the presentation, then submits.
  bool StreamRecordAllowed();
  bool StreamOpenFrame();
  // After the worker retained a command into the frame: prepare and record what is ready.
  void StreamPump();
  // Runs both tasks to their ends. present=false: no presenter image (the frame is flushed).
  bool StreamFinish(bool present);
  // Ends and submits the slot's command buffers; context is null for a flush.
  // (RefreshContext: VulkanPresenter::VulkanGuestOutputRefreshContext; defined in the .cpp.)
  template <typename RefreshContext>
  bool StreamSubmit(RefreshContext* context);
  // A frame that cannot be streamed to its end any more: flushes what is recorded.
  void StreamFlush(const char* reason);
  bool ClearGuestOutputStreamed(uint32_t width, uint32_t height, uint32_t display_width,
                                uint32_t display_height, const PresentCommand& present,
                                const std::shared_ptr<const NativeTextureResource>& present_source,
                                const std::shared_ptr<const EnvironmentalDataV2>& environmental_data);
  void ReportStreamRecord();
  void BeginModernShaderFrame();
  void TraceModernShaderDraw(const NativeCommand& command, const NativeRenderingTarget& target);
  void TraceModernShaderFailure(const NativeCommand& command, std::string_view point, std::string_view reason);
  void ApplyStateCommand(const NativeCommand& command);
  void ApplyStateCommandBytes(CommandType type, const uint8_t* bytes);
  void ApplyPendingStateCommands(const std::vector<uint8_t>& pending);
  bool CoalesceStateCommand(const void* command, size_t command_size);
  void RegisterVectorFontTexture(uint32_t texture, uint32_t vector_font_id);
  bool ApplyShaderConstantDelta(NativeCommand& command, uint32_t device);
  bool SnapshotDrawConstants(NativeCommand& command, uint32_t device);
  // Why the next PrepareSurfaceContent may materialize (statistics only):
  // 0 draw scope, 1 clear scope, 2 resolve clear, 3 other.
  uint32_t materialize_trigger_ = 3;
  // Index count RecordIndexedPrimitive uses instead of the command's own when
  // consecutive equivalent draws are recorded as one (0: none).
  uint32_t indexed_merge_count_override_ = 0;
  void BindNativeVertexBuffer(VkCommandBuffer command_buffer, uint32_t binding, VkBuffer buffer,
                              VkDeviceSize offset);
  void BindNativeIndexBuffer(VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
                             VkIndexType index_type);
  bool InitializeShaderCache();
  static bool ReflectVertexInputs(const std::vector<uint32_t>& spirv,
                                  std::vector<NativeVertexInput>& inputs);
  ShaderOverrideSelection ResolvePipelineShaderOverrides(
      const NativeShader* vertex_shader, const NativeShader* pixel_shader,
      VkSampleCountFlagBits rasterization_samples) const;
  static VkFormat GetCompatibleVertexFormat(uint32_t element_type,
                                            NativeVertexNumericType numeric_type);
  static VkFormat GetDefaultVertexFormat(NativeVertexNumericType numeric_type,
                                         uint32_t component_count);
  void RegisterShader(const RegisterShaderCommand& command);
  void RegisterVertexDeclaration(const RegisterVertexDeclarationCommand& command);
  const NativeShader* FindRegisteredShader(uint32_t handle, ShaderStage stage) const;
  void DestroyShaderResources();
  bool PublishFrame(const PresentCommand& present,
                    const std::shared_ptr<const NativeTextureResource>& present_source = nullptr,
                    const std::shared_ptr<const EnvironmentalDataV2>& environmental_data = nullptr);
  bool ClearGuestOutput(uint32_t width, uint32_t height, uint32_t display_width,
                        uint32_t display_height, const PresentCommand& present,
                        const std::shared_ptr<const NativeTextureResource>& present_source,
                        const std::shared_ptr<const EnvironmentalDataV2>& environmental_data);
  bool InitializeNativeRendererObjects();
  bool InitializeNativePipelineCache();
  void SaveNativePipelineCache();
  void ScheduleNativePipelineCheckpoint();
  void DrainNativePipelineCompiles();
  void StopNativePipelineCompiler();
  void ReplayNativePipelineRecipes();
  void LoadNativePipelineRecipes();
  VkPipeline PublishNativePipeline(const NativePipelineKey& key, VkPipeline pipeline,
                                    uint64_t compile_ticks);
  void RecordNativePipelineTiming(uint64_t compile_ticks, uint64_t wait_ticks = 0);
  bool CreateNativeUploadBuffer(VkDeviceSize capacity, NativeUploadBuffer& upload_buffer,
                                bool prefer_cached = false);
  void DestroyNativeUploadBuffer(NativeUploadBuffer& upload_buffer);
  bool EnsureFrameUploadCapacityFast(
      const std::shared_ptr<const NativeTextureResource>& present_source);
  struct CapacityVertexKey {
    const NativeBufferResource* resource = nullptr;
    uint64_t declaration_hash = 0;
    uint64_t shader_hash = 0;
    uint32_t stream = 0;
    uint32_t offset = 0;
    uint32_t stride = 0;
    bool operator==(const CapacityVertexKey&) const = default;
  };
  struct CapacityVertexKeyHash {
    size_t operator()(const CapacityVertexKey& k) const {
      return size_t(NativeMixHash(reinterpret_cast<uintptr_t>(k.resource) ^ k.declaration_hash * 3 ^
                                  k.shader_hash * 5 ^ (uint64_t(k.stream) << 56) ^
                                  (uint64_t(k.offset) << 24) ^ k.stride));
    }
  };
  struct CapacityIndexKey {
    const NativeBufferResource* resource = nullptr;
    bool index32 = false;
    bool operator==(const CapacityIndexKey&) const = default;
  };
  struct CapacityIndexKeyHash {
    size_t operator()(const CapacityIndexKey& k) const {
      return size_t(NativeMixHash(reinterpret_cast<uintptr_t>(k.resource) * 2 + k.index32));
    }
  };
  struct CapacityGenerationHash {
    size_t operator()(uint64_t generation) const { return size_t(NativeMixHash(generation)); }
  };
  NativeFlatSet<uint64_t, CapacityGenerationHash> capacity_texture_set_;
  NativeFlatSet<CapacityVertexKey, CapacityVertexKeyHash> capacity_vertex_set_;
  NativeFlatSet<CapacityIndexKey, CapacityIndexKeyHash> capacity_index_set_;
  bool EnsureFrameUploadCapacity(
      const std::shared_ptr<const NativeTextureResource>& present_source);
  bool InitializeContentProbeBuffer(NativeContentProbeBuffer* buffer = nullptr);
  void DestroyContentProbeBuffer();
  void AnalyzePendingContentProbe(uint32_t slot);
  bool InitializeLightStencilHistogramBuffer(uint32_t width, uint32_t height);
  void DestroyLightStencilHistogramBuffers();
  void AnalyzePendingLightStencilHistogram(uint32_t slot);
  uint64_t SelectedLightColorDeltaInstance() const;
  void PrepareRoomLightProbes(uint32_t submitted_frame);
  bool RecordLightStencilHistogramProbe(VkCommandBuffer command_buffer, NativeSurfaceImage& image,
                                        const NativeCommand& command, uint32_t submitted_frame,
                                        uint32_t command_index, uint64_t draw_id,
                                        uint64_t light_trace_record, uint64_t bulb_instance_id,
                                        bool after_setup);
  bool InitializeLightColorDeltaBuffer(uint32_t width, uint32_t height, VkFormat format);
  void DestroyLightColorDeltaBuffers();
  void AnalyzePendingLightColorDelta(uint32_t slot);
  bool RecordLightColorDeltaProbe(VkCommandBuffer command_buffer,
                                  NativeSurfaceImage& image,
                                  const NativeCommand& command,
                                  uint32_t submitted_frame,
                                  uint32_t command_index,
                                  uint64_t draw_id,
                                  uint64_t instance_id,
                                  bool after_draw);
  bool InitializeTranslucentQueryPool();
  void DestroyTranslucentQueryPool();
  void AnalyzePendingTranslucentQueries(uint32_t slot);
  bool EnqueueDeferredDiagnosticTask(std::function<void()> task);
  void DeferredDiagnosticWorkerMain();
  void ShutdownDeferredDiagnosticWorker();
  void PublishDeferredNativeTrace();
  bool IsNativeFlightRecorderEnabled() const;
  void StageNativeFlightResource(NativeFlightResourceKind kind, uint64_t object,
                                 uint32_t guest_handle = 0, uint64_t guest_generation = 0);
  void StageNativeFlightDescriptorSlot(NativeDescriptorSlotHandle handle, uint32_t guest_handle,
                                       uint64_t guest_generation);
  void StageNativeTextureFlightResources(const NativeTextureImage& image);
  void StageNativeSurfaceFlightResources(const NativeSurfaceImage& image);
  void MarkNativeSurfaceImageUsed(NativeSurfaceImage& image);
  void CommitNativeFlightSubmission(uint32_t slot, uint64_t submission, uint32_t frame);
  bool CompleteNativeFlightSubmission(uint32_t slot, uint64_t submission);
  void DiscardStagedNativeFlightResources();
  void TraceNativeFlightMutation(std::string_view action, NativeFlightResourceKind kind,
                                 uint64_t object, uint32_t page = 0, uint32_t index = 0,
                                 uint32_t generation = 0);
  void NameNativeFlightObject(VkObjectType type, uint64_t object, std::string_view name);
  bool InitializeNativeGpuProfiler();
  void DestroyNativeGpuProfiler();
  void ExportNativeGpuProfile();
  void AnalyzeCompletedNativeGpuProfile(uint32_t slot,
                                        NativeFrameContextRing::QueryReadbackResources resources);
  void PublishCompletedNativeGpuProfileSamples();
  bool BeginNativeGpuProfileFrame(VkCommandBuffer command_buffer, uint32_t submitted_frame,
                                  uint64_t cpu_wait_ticks);
  void SwitchNativeGpuProfileRange(VkCommandBuffer command_buffer, performance::GpuRange range);
  bool SwitchNativeGpuProfileRegion(VkCommandBuffer command_buffer, performance::GpuRange range,
                                    uint32_t attribution_region);
  void EndNativeGpuProfileFrame(VkCommandBuffer command_buffer);
  void CancelNativeGpuProfileFrame();
  void CancelNativeGpuProfileFrame(uint32_t slot);
  bool IsNativeGpuProfileFrameActive() const;
  void AddNativeGpuProfileCounter(performance::Counter counter, uint64_t value = 1);
  void UpdateNativeMemoryProfile(uint32_t submitted_frame, bool force_sample = false);
  memory::Snapshot CollectNativeMemorySnapshot(uint32_t submitted_frame, uint32_t marker);
  void RecordNativeMemoryLifecycle(memory::ResourceKind kind, memory::LifecycleAction action,
                                   memory::LifecycleReason reason, uint64_t identity,
                                   uint64_t generation, uint64_t parent_identity,
                                   uint64_t logical_bytes, uint64_t retained_bytes,
                                   uint64_t allocation_bytes, uint32_t last_used_frame = 0,
                                   uint32_t auxiliary = 0);
  void CaptureNativeRetainedResources(uint32_t submitted_frame, uint32_t marker);
  void RequestNativeProcessVmScan();
  void JoinNativeProcessVmScan();
  void ExportNativeMemoryProfile(std::vector<memory::Snapshot> samples,
                                 std::vector<memory::LifecycleEvent> events,
                                 std::vector<memory::RetainedResource> retained,
                                 uint64_t overwritten_samples, uint64_t overwritten_events,
                                 uint64_t dropped_retained);
  void FinalizeNativeMemoryProfile(uint32_t submitted_frame);
  bool RecordContentProbe(VkCommandBuffer command_buffer, uint32_t submitted_frame,
                          NativeSurfaceImage* final_surface,
                          NativeTextureImage* final_composite_input,
                          const std::shared_ptr<const NativeTextureResource>& present_source,
                          VkImage presenter_image, VkImageLayout presenter_layout,
                          uint32_t presenter_width, uint32_t presenter_height, bool force = false);
  bool RecordContentProbeImage(VkCommandBuffer command_buffer, uint32_t stage_index, VkImage image,
                               VkFormat format, VkImageLayout layout, uint32_t width,
                               uint32_t height, uint32_t handle, uint32_t address, uint8_t kind,
                               uint32_t mip_level = 0, VkImageAspectFlags aspect_override = 0,
                               uint32_t vertical_band = 0, uint32_t vertical_band_count = 1,
                               NativeContentProbeBuffer* probe_buffer = nullptr,
                               const VkRect2D* sample_region = nullptr);
  bool RecordRoomLightInputs(VkCommandBuffer command_buffer, const NativeCommand& command,
                             uint32_t submitted_frame, uint32_t command_index, uint64_t draw_id);
  static void PublishRoomLightInputs(const std::shared_ptr<NativeRoomLightInputs>& inputs,
                              const std::vector<uint8_t>& payload, bool memory_visible);
  bool RecordDepthStencilDiagnosticProbe(VkCommandBuffer command_buffer, NativeSurfaceImage& image,
                                         uint8_t depth_kind, uint8_t stencil_kind,
                                         std::string_view role = {}, uint32_t trace_wrapper = 0,
                                         uint64_t resource_generation = 0,
                                         const NativePlacementOwner* owner = nullptr);
  bool RecordDepthStencilDiagnosticProbe(VkCommandBuffer command_buffer, NativeTextureImage& image,
                                         uint8_t depth_kind, uint8_t stencil_kind,
                                         std::string_view role = {}, uint32_t trace_wrapper = 0,
                                         uint64_t resource_generation = 0,
                                         const NativePlacementOwner* owner = nullptr);
  bool RecordStencilDiagnosticProbe(VkCommandBuffer command_buffer, NativeSurfaceImage& image,
                                    uint8_t kind);
  bool CreateNativeDescriptors();
  bool EnsureIndexedDescriptorPages(uint32_t page_count);
  bool ActivateCachedDescriptorFallback(NativeDescriptorStatus status, const char* reason);
  bool UpdateIndexedImageDescriptor(NativeDescriptorSlotHandle handle,
                                    const std::array<VkImageView, 4>& views);
  bool UpdateIndexedSamplerDescriptor(NativeDescriptorSlotHandle handle, VkSampler sampler);
  bool ClearIndexedTextureDescriptor(NativeDescriptorSlotHandle handle);
  bool ReclaimCompletedNativeTextureDescriptors(uint64_t completed_submission);
  bool AllocateNativeTextureDescriptor(NativeTextureImage& image);
  bool AllocateNativeSamplerDescriptor(NativeSampler& sampler);
  void RetireNativeTextureDescriptor(NativeTextureImage& image);
  // A2: a resolve that copies a whole surface into a same-format texture and
  // then clears the surface exchanges the two images instead of copying.
  struct PlannedResolveSwap {
    NativeSurfaceImage* surface = nullptr;
    NativeTextureImage* texture = nullptr;
    NativeCommand* command = nullptr;
    bool executed = false;
    bool slots_swapped = false;  // stable-slot mode exchanged descriptor slots
  };
  bool PlanResolveSwap(NativeCommand& command, NativeTextureImage& texture,
                       const std::vector<NativeCommand*>& commands, size_t command_index);
  void ExecuteResolveSwap(PlannedResolveSwap& swap);
  void FinalizePlannedResolveSwaps();
  void UndoPlannedResolveSwaps();
  std::vector<PlannedResolveSwap> planned_resolve_swaps_;
  // Fragment stage for pipelines without a pixel shader but with color
  // attachments: writes nothing, instead of leaving the color undefined.
  VkShaderModule empty_fragment_module_ = VK_NULL_HANDLE;
  // Paged descriptors are built from views during preparation: after a planned
  // swap, the image (view and descriptor lifetime) each object will hold then.
  struct PreparedImageView {
    VkImageView view = VK_NULL_HANDLE;
    uint64_t lifetime = 0;
  };
  std::unordered_map<const NativeTextureImage*, PreparedImageView> prepared_texture_views_;
  std::unordered_map<const NativeSurfaceImage*, PreparedImageView> prepared_surface_views_;
  // A11: the frame's last resolve into the frontbuffer is skipped and the
  // present samples this surface, repeating the RGBA8 conversion in its shader.
  NativeSurfaceImage* present_surface_override_ = nullptr;
  bool present_source_unorm8_ = false;
  std::optional<NativeCommand> deferred_frontbuffer_resolve_;
  bool CreateResolveConversionObjects();
  bool CreateTextureDetileObjects();
  // Converts a detile-job texture's raw bytes (raw) into converted, ahead of
  // the image copies that read converted.
  bool RecordTextureDetile(VkCommandBuffer command_buffer,
                           const std::shared_ptr<const NativeTextureResource>& texture,
                           const NativeUploadAllocation& raw,
                           const NativeUploadAllocation& converted);
  void CheckTextureDetileValidations(VkBuffer reused_buffer);
  bool CreateNullImage(VkImageType image_type, VkImageViewType view_type, uint32_t array_layers,
                       VkImageCreateFlags flags, NativeImageResource& resource);
  bool RecordNullImageInitialization(VkCommandBuffer command_buffer);
  bool PrepareFrameDescriptorPool(uint32_t draw_count, uint32_t combined_descriptor_count,
                                  uint32_t combined_set_count);
  bool SynchronizeCachedDescriptorSlot(uint32_t frame_copy);
  bool EnsureCachedDescriptorCapacity(uint32_t frame_copy, uint32_t additional_entries);
  void InvalidateCachedDescriptors(uint64_t image_lifetime);
  VkFormatProperties GetNativeFormatProperties(VkFormat format);
  bool PrepareFrameTextures(VkCommandBuffer command_buffer, bool prepare_present,
                            uint32_t submitted_frame, bool trace_reflections);
  // 60: the body of PrepareFrameTextures. With streaming it prepares the frame's commands as
  // they are assembled (suspending when it runs out of them) until stream_.closing.
  NativeFrameTask PrepareFrameTexturesTask(VkCommandBuffer command_buffer, bool prepare_present,
                                           uint32_t submitted_frame, bool trace_reflections,
                                           bool streaming);
  NativeSampler* GetOrCreateSampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                                    const NativeTextureImage* image = nullptr,
                                    NativeSamplerKey* effective_key = nullptr);
  void ReleaseUnusedTextureImages(
      uint32_t submitted_frame, const std::shared_ptr<const NativeTextureResource>& present_source);
  void ReleaseUnusedBufferResources(uint32_t submitted_frame);
  // 34: same generations as VisitNativeCommandTextureGenerations, but the 32
  // sampler slots are visited by used_texture_mask: the capture fills
  // textures[stage] only for stages in that mask and the mask never loses bits
  // (see NativeCommand::Reset). The full walk read 8 cache lines of every
  // command on the title thread (CollectTextureProtection) and again on the
  // worker / recorder.
  template <typename Visitor>
  static void VisitProtectedTextureGenerations(const NativeCommand& command, Visitor&& visit) {
    const auto texture = [&](const std::shared_ptr<const NativeTextureResource>& resource) {
      if (!resource) return;
      visit(resource->generation);
      if (resource->packed_depth_source) visit(resource->packed_depth_source->generation);
    };
    texture(command.resolve_destination);
    texture(command.depth_handoff_source);
    texture(command.present_source);
    texture(command.postfx_half_scene);
    for (uint32_t mask = command.used_texture_mask; mask; mask &= mask - 1) {
      const uint32_t stage = uint32_t(std::countr_zero(mask));
      if (stage < command.textures.size()) texture(command.textures[stage]);
    }
  }
  void QueueTextureProtection(const NativeCommand& command, bool retain);
  void QueueTextureProtectionLocked(const NativeCommand& command, bool retain);
  static void CollectTextureProtection(const NativeCommand& command);
  void AppendQueuedTextureProtection(std::unordered_set<uint64_t>& generations) const;
  // 32: staged + queued + worker protection without render_mutex_.
  void AppendInFlightTextureProtection(std::unordered_set<uint64_t>& generations);
  // 29.10: retains of captured, not yet published commands (producer side,
  // staged_texture_protection_ under staged_protection_mutex_).
  void StageTextureProtection(const NativeCommand& command);
  // Caller holds render_mutex_: the staged references of staged_draw_commands_
  // (and `also`) move to the queue index, one queue-index lock per batch.
  void PublishStagedTextureProtectionLocked(const NativeCommand* also);
  static void ApplyTextureProtection(NativeTextureProtectionIndex& index,
                                     const NativeCommand& command, bool retain,
                                     const char* index_name);
  void ClearNativeFrameCommands();
  void TakeProducerPendingState(NativeCommand& command);  // 34
  void PrefetchProducerBuffers(NativeCommand& command);  // 53, 54
  static void AddProtectedTextureGenerations(const NativeCommand& command,
                                             std::unordered_set<uint64_t>& generations);
  void CollectProtectedTextureGenerations(
      const std::shared_ptr<const NativeTextureResource>& present_source,
      std::unordered_set<uint64_t>& generations);
  NativeTextureHeapBudgets QueryNativeTextureHeapBudgets() const;
  bool AllocateNativeTextureImage(const VkImageCreateInfo& image_info, NativeTextureImage& image);
  void EvictNativeTextureImages(uint32_t submitted_frame, bool allocation_recovery);
  void RetireNativeTextureImage(std::unique_ptr<NativeTextureImage> image);
  bool ReleaseRetiredTextureImages(bool drain_all = false);
  void DestroyNativeTextureImage(NativeTextureImage& image);
  void DestroyNativeTextureViews(NativeTextureImage& image);
  void DestroyNativeSurfaceImage(NativeSurfaceImage& image);
  void ReleasePendingSurfaceImages();
  void QueueSurfaceImageRelease(uint32_t handle);
  NativeTextureImage* GetOrCreateTextureImage(
      VkCommandBuffer command_buffer, const std::shared_ptr<const NativeTextureResource>& texture,
      NativeTextureUploadBatch* upload_batch = nullptr);
  void FlushTextureUploads(VkCommandBuffer command_buffer, NativeTextureUploadBatch& batch);
  NativeSurfaceImage* GetOrCreateSurfaceImage(
      const SurfaceDescriptor& descriptor, bool depth,
      VkSampleCountFlagBits host_sample_override = VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM,
      bool allow_allocation = true);
  VkImageView GetOrCreateTextureMipView(NativeTextureImage& image, uint32_t mip_level);
  VkPipeline GetOrCreateFullscreenPipeline(
      VkFormat destination_format, NativeResolveConversionPipeline::Kind kind,
      VkSampleCountFlagBits destination_samples = VK_SAMPLE_COUNT_1_BIT);
  VkPipeline GetOrCreateResolveConversionPipeline(
      VkFormat destination_format, bool multisampled_source,
      VkSampleCountFlagBits destination_samples = VK_SAMPLE_COUNT_1_BIT);
  VkPipeline GetOrCreateDepthResolvePipeline(VkFormat destination_format);
  VkPipeline GetOrCreateDepthHandoffPipeline(
      VkFormat destination_format,
      VkSampleCountFlagBits destination_samples = VK_SAMPLE_COUNT_1_BIT);
  VkPipeline GetOrCreateHDRPresentPipeline();
    // SGSR: отдельный пайплайн для Snapdragon Game Super Resolution
  VkPipeline GetOrCreateSGSRPresentPipeline();
  bool RecordResolveConversion(VkCommandBuffer command_buffer, NativeSurfaceImage& source,
                               NativeTextureImage& destination, uint32_t destination_level,
                               int32_t source_left, int32_t source_top, uint32_t source_width,
                               uint32_t source_height, int32_t destination_x, int32_t destination_y,
                               uint32_t destination_width, uint32_t destination_height,
                               const GuestSurfaceView& source_view,
                               const GuestSurfaceView& requested_view,
                               xenos::CopySampleSelect sample_select,
                               int32_t color_exponent,
                               NativeTextureImage* hdr_mirror = nullptr);
  bool RecordDepthResolveConversion(VkCommandBuffer command_buffer, NativeSurfaceImage& source,
                                    NativeTextureImage& destination, uint32_t destination_level,
                                    int32_t source_left, int32_t source_top, int32_t destination_x,
                                    int32_t destination_y, uint32_t copy_width,
                                    uint32_t copy_height, const GuestSurfaceView& source_view,
                                    const GuestSurfaceView& requested_view,
                                    xenos::CopySampleSelect sample_select);
  NativeProducerDepthResolve PrepareProducerDepthResolve(
      VkCommandBuffer command_buffer, size_t command_index, const NativeRenderingTarget& target,
      VkRenderingAttachmentInfo& depth_attachment, VkRenderingAttachmentInfo& stencil_attachment,
      NativeProducerDepthResolveScan& scan);
  NativeProducerDepthResolveStamp CaptureProducerDepthResolveStamp(
      size_t command_index, const NativeSurfaceImage& source,
      const NativeTextureImage& destination) const;
  void CompleteProducerDepthResolve(VkCommandBuffer command_buffer,
                                    NativeProducerDepthResolve& producer);
  bool ConsumeProducerDepthResolve(const NativeProducerDepthResolve& producer,
                                  NativeSurfaceImage& source, NativeTextureImage& destination);
  bool RecordSurfaceMaterialization(VkCommandBuffer command_buffer,
                                    const NativePlacementOwner& owner,
                                    NativeSurfaceImage& destination,
                                    const GuestSurfaceView& destination_view);
  bool EnsureDepthHandoffStencilScratch(NativeSurfaceImage& destination);
  bool RecordDepthSurfaceHandoff(VkCommandBuffer command_buffer,
                                 const NativeCommand& native_command, uint32_t submitted_frame);
  bool RecordPackedDepthAlias(VkCommandBuffer command_buffer, NativeTextureImage& destination);
  bool RefreshPackedDepthAliases(VkCommandBuffer command_buffer, uint64_t source_generation);
  bool PrepareSurfaceContent(VkCommandBuffer command_buffer, NativeSurfaceImage& surface,
                             const SurfaceDescriptor& descriptor, bool depth,
                             uint32_t submitted_frame, RenderPhase render_phase);
  void ClaimSurfaceContent(NativeSurfaceImage& surface, const SurfaceDescriptor& descriptor,
                           bool depth, uint32_t submitted_frame, RenderPhase render_phase,
                           NativePlacementOwner::WriteKind write_kind,
                           VkImageAspectFlags written_aspects = 0,
                           uint64_t source_serial = 0);
  const NativePlacementOwner* FindPlacementOwner(const SurfaceDescriptor& descriptor,
                                                 bool depth) const;
  bool HasCurrentPlacementContent(const NativeSurfaceImage& surface,
                                  const SurfaceDescriptor& descriptor, bool depth,
                                  uint32_t submitted_frame) const;
  bool RecordResolveClears(VkCommandBuffer command_buffer, const NativeCommand& command,
                           const ResolveCommand& resolve, uint32_t submitted_frame);
  bool ResolveRenderingTarget(const NativeCommand& command, VkImageView presenter_view,
                              uint32_t presenter_width, uint32_t presenter_height,
                              NativeRenderingTarget& target, bool allow_allocation = true,
                              const NativeRenderingTarget* active_target = nullptr);
  bool ResolveRenderingTargetMemo(const NativeCommand& command, VkImageView presenter_view,
                                  uint32_t presenter_width, uint32_t presenter_height,
                                  NativeRenderingTarget& target,
                                  const NativeRenderingTarget& active_target);
  NativeAttachmentUsage GetRenderingTargetUsage(const NativeCommand& command) const;
  bool TransitionRenderingTarget(VkCommandBuffer command_buffer, NativeRenderingTarget& target);
  bool AllocateUpload(VkDeviceSize size, VkDeviceSize alignment, NativeUploadAllocation& allocation,
                      NativeUploadKind kind = NativeUploadKind::kOther);
  VkPipeline GetOrCreatePipeline(const NativePipelineState& state,
                                 const NativeFixedFunctionState& fixed_function_state,
                                 uint32_t primitive_type, const NativeRenderingTarget& target,
                                 uint32_t user_pointer_stride = 0,
                                 bool primitive_restart_enable = false, bool prewarm = false);
  VkPipeline GetOrCreateDrawPipeline(const NativeCommand& command, uint32_t primitive_type,
                                     const NativeRenderingTarget& target,
                                     uint32_t user_pointer_stride = 0,
                                     bool primitive_restart_enable = false, bool prewarm = false);
  void TryPrewarmDrawPipeline(const NativeCommand& command);
  bool GetRequiredVertexStreams(const NativePipelineState& state,
                                std::array<bool, kVertexStreamCount>& required_streams) const;
  bool UploadBufferResource(const std::shared_ptr<const NativeBufferResource>& resource,
                            VkCommandBuffer command_buffer, bool index_buffer, bool index32,
                            const NativePipelineState* vertex_state, uint32_t vertex_stream,
                            NativeFrameResources& resources, NativeUploadAllocation& allocation);
  bool CreateNativePersistentBufferBlock(uint64_t block_id, VkDeviceSize capacity);
  void DestroyNativePersistentBufferBlock(uint64_t block_id);
  void DestroyNativePersistentBuffers();
  // The persistent entry the owner's memo holds for key, marked as used by the
  // frame being recorded; null if the memo is empty, stale or for another key.
  NativePersistentBufferEntry* UsePersistentMemo(const NativeBufferResource& owner,
                                                 const NativePersistentBufferKey& key);
  void TouchPersistentEntry(NativePersistentBufferEntry& entry);
  bool GetOrCreatePersistentBuffer(VkCommandBuffer command_buffer,
                                   const std::shared_ptr<const NativeBufferResource>& owner,
                                   const NativePersistentBufferKey& key, const uint8_t* source,
                                   VkDeviceSize size, NativeUploadKind upload_kind,
                                   NativeUploadAllocation& allocation);
  bool EnsureFrameConstantArenaCapacity();
  bool FindFrameConstantBuffer(NativeConstantBufferKind kind, uint64_t immutable_identity,
                               NativeUploadAllocation& allocation) const;
  bool GetOrCreateFrameConstantBuffer(NativeConstantBufferKind kind, uint64_t immutable_identity,
                                      std::span<const uint8_t> source_bytes, bool guest_word_order,
                                      NativeUploadAllocation& allocation,
                                      size_t reserve_bytes = 0);
  bool ResetFrameConstantArena(uint32_t slot, uint64_t completed_submission, bool unsubmitted);
  void DestroyNativeFrameConstantArenas();
  void ReleaseUnusedPersistentBuffers(uint64_t completed_submission);
  bool ActivateNativeFrameSlot(uint32_t slot);
  bool CompleteNativeFrameSlot(uint32_t slot, uint64_t submission,
                               uint64_t* profile_processing_ticks = nullptr,
                               uint64_t* actual_wait_ticks = nullptr);
  bool CompleteActiveNativeFrameSlot(uint64_t* profile_processing_ticks = nullptr,
                                     uint64_t* actual_wait_ticks = nullptr);
  bool CompleteSecondaryNativeFrameSlot(uint64_t* profile_processing_ticks = nullptr,
                                        uint64_t* actual_wait_ticks = nullptr);
  void RollbackActiveNativeFrameSlot();
  bool RecoverFailedNativeFrameRecording();
  bool BindCommonDrawState(VkCommandBuffer command_buffer, const NativeCommand& command,
                           VkPipeline pipeline, uint32_t width, uint32_t height,
                           uint32_t logical_width, uint32_t logical_height,
                           VkSampleCountFlagBits samples, NativeFrameResources& resources,
                           const NativeRenderingTarget& target);
  void LogVectorFontDraw(const NativeCommand& command, uint32_t submitted_frame,
                         size_t command_index) const;
  bool IsHoveGantryGeometryCandidate(const NativeCommand& command) const;
  bool RecordNativeFrame(VkCommandBuffer command_buffer, uint32_t width, uint32_t height,
                         VkImage presenter_image, VkImageView presenter_view,
                         uint32_t submitted_frame,
                         const std::shared_ptr<const NativeTextureResource>& present_source,
                         const std::shared_ptr<const EnvironmentalDataV2>& environmental_data,
                         bool hdr_output, float hdr_headroom, bool& presenter_transfer_written,
                         bool& presenter_written, bool trace_stages, uint32_t trace_sequence,
                         bool force_content_probe);
  // 60: the body of RecordNativeFrame. The presenter arguments are references: with streaming
  // they live in stream_ and receive their final values at Present, before the tail runs.
  NativeFrameTask RecordNativeFrameTask(
      VkCommandBuffer command_buffer, const uint32_t& width, const uint32_t& height,
      const VkImage& presenter_image, const VkImageView& presenter_view, uint32_t submitted_frame,
      const std::shared_ptr<const NativeTextureResource>& present_source,
      const std::shared_ptr<const EnvironmentalDataV2>& environmental_data,
      const bool& hdr_output, const float& hdr_headroom, bool& presenter_transfer_written,
      bool& presenter_written, const bool& trace_stages, const uint32_t& trace_sequence,
      const bool& force_content_probe, bool streaming);
  bool RecordPrimitive(VkCommandBuffer command_buffer, const NativeCommand& command, uint32_t width,
                       uint32_t height, const NativeRenderingTarget& target,
                       NativeFrameResources& resources);
  bool RecordPrimitiveUp(VkCommandBuffer command_buffer, const NativeCommand& command,
                         uint32_t width, uint32_t height, const NativeRenderingTarget& target,
                         NativeFrameResources& resources);
  // Consecutive DrawPrimitiveUp commands with identical state drawn as one
  // indexed triangle list over their concatenated vertices.
  bool RecordPrimitiveUpBatch(VkCommandBuffer command_buffer,
                              const std::vector<const NativeCommand*>& run, uint32_t width,
                              uint32_t height, const NativeRenderingTarget& target,
                              NativeFrameResources& resources);
  bool RecordIndexedPrimitive(VkCommandBuffer command_buffer, const NativeCommand& command,
                              uint32_t width, uint32_t height, const NativeRenderingTarget& target,
                              NativeFrameResources& resources);
  bool RecordClear(VkCommandBuffer command_buffer, const NativeCommand& command,
                   const NativeRenderingTarget& target,
                   bool skip_depth_stencil = false);
  bool RecordResolve(VkCommandBuffer command_buffer, const NativeCommand& command,
                     uint32_t submitted_frame,
                     NativeTextureImage* high_precision_destination = nullptr,
                     bool* high_precision_written = nullptr,
                     const NativeProducerDepthResolve* producer_depth_resolve = nullptr);
  NativeTextureImage* EnsureHDRPresentMirror(uint32_t width, uint32_t height);
  void DestroyHDRPresentMirrors();
  bool RecordPresent(VkCommandBuffer command_buffer, VkImage presenter_image,
                     VkImageView presenter_view, uint32_t presenter_width,
                     uint32_t presenter_height,
                     const std::shared_ptr<const NativeTextureResource>& present_source,
                     NativeTextureImage* high_precision_source, bool hdr_output, float hdr_headroom,
                     bool& transfer_written, NativeTextureImage* authoritative_source = nullptr);
  bool ReadbackTextureToGuest(const TextureLockCommand& command, TextureLockResult& result);
  void DestroyTextureReadbackObjects();
  void DestroyNativeRendererObjects();
  void DestroyVulkanWorkerObjects();

  rex::memory::Memory* memory_ = nullptr;
  ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<ui::GraphicsProvider> provider_;
  std::unique_ptr<ui::Presenter> presenter_;

  // 29.10: own cache line. The title thread locks it per published batch; the
  // members after it (snapshot pool, command pool) are written by the worker and
  // the retirer thousands of times per frame. worker_waiting_ is guarded by it.
  alignas(64) std::mutex render_mutex_;
  std::condition_variable render_condition_;
  // 31.10: the render worker waits only on worker_condition_ (queue non-empty);
  // render_condition_ keeps the producer backpressure / present waits. With one
  // shared variable, notify_one for the worker could wake a producer waiting
  // in backpressure instead, and the worker slept until the next frame-end
  // notify_all. Both are used under render_mutex_.
  std::condition_variable worker_condition_;
  bool worker_waiting_ = false;
  NativeSnapshotResource snapshot_pool_;  // aligned to 64 inside (29.10)
  alignas(64) NativeCommandPool<NativeCommand> native_command_pool_;
  NativeOwnedCommands<NativeCommand, true> render_queue_;
  // Draws captured but not yet published to render_queue_ (guarded by
  // command_capture_mutex_); flushed in batches and before any other command.
  std::vector<NativeCommandPool<NativeCommand>::Owner> staged_draw_commands_;
  // Worker-owned bulk handoff. Pending commands remain resource-protected
  // across presents, allocation recovery and synchronous readback boundaries.
  NativeOwnedCommands<NativeCommand> worker_command_batch_;
  size_t worker_command_cursor_ = 0;
  void AppendWorkerTextureProtection(std::unordered_set<uint64_t>& generations) const;
  // Its own lock, not render_mutex_: the title thread retains a command's
  // generations before queueing it and the worker releases them after taking
  // it, so neither holds the queue lock for this. In between, the command is
  // counted twice (queue index and staging or worker batch), which only
  // widens the protected set. Lock order: render_mutex_, then this.
  alignas(64) NativeTextureProtectionIndex queued_texture_protection_;
  mutable std::mutex queued_protection_mutex_;
  // 29.10: written by the producer under render_mutex_ (per batch / per command)
  // and read by the worker; kept off the lines of the lock above.
  alignas(64) uint32_t queued_title_presents_ = 0;
  bool producer_waiting_ = false;
  uint64_t diagnostic_submit_sequence_ = 0;
  uint32_t diagnostic_producer_epoch_ = 1;
  // Read by the worker on every loop; written only at start/stop.
  alignas(64) std::atomic<bool> render_worker_running_{false};
  std::thread render_worker_;
  // 58: direct record. The worker loop's locals that must outlive one pump.
  bool direct_record_ = false;
  bool worker_loop_started_ = false;
  // Lock order: command_capture_mutex_ -> direct_pump_mutex_ (the synchronous texture lock
  // pumps with the capture lock held; a pump never takes the capture lock).
  std::mutex direct_pump_mutex_;
  std::atomic<std::thread::id> direct_pump_owner_{};
  // The thread that submits Presents (RenderGuest): it pumps after every submission. Other
  // submitters (Main's render-phase markers) leave their commands queued for its next pump,
  // so they never wait for a frame recording; until a Present was seen, everyone pumps.
  std::atomic<std::thread::id> direct_recorder_thread_{};
  struct WorkerLoopLocals {
    std::vector<std::pair<RenderPhase, uint32_t>> render_phase_stack;
    uint64_t startup_texture_lock_count = 0;
    bool startup_present_follows_texture_lock_flush = false;
    size_t queued_after_batch = 0;
  } worker_loop_;
  uint64_t direct_pumps_ = 0, direct_frames_ = 0, direct_report_pumps_ = 0, direct_report_frames_ = 0;
  uint64_t direct_report_tick_ = 0;
  // 60: stream record state. Only the pumping thread touches it (direct_pump_mutex_).
  bool stream_record_ = false;
  bool stream_disabled_ = false;  // a permanent reason (backend, failure) turned it off
  struct NativeStreamFrame {
    bool open = false;
    bool closing = false;     // no more commands: the tasks run to their ends
    bool no_present = false;  // closing without a presenter image (a flush)
    uint32_t frame = 0;       // predicted submitted_frame
    uint32_t resource_frame = 0;
    size_t prep_source = 0;   // frame commands handed to the preparation task
    uint32_t lookahead = 64;
    VkCommandBuffer prep_command_buffer = VK_NULL_HANDLE;
    // RecordNativeFrameTask's presenter arguments (bound by reference).
    uint32_t width = 0, height = 0;
    VkImage presenter_image = VK_NULL_HANDLE;
    VkImageView presenter_view = VK_NULL_HANDLE;
    std::shared_ptr<const NativeTextureResource> present_source;
    std::shared_ptr<const EnvironmentalDataV2> environmental_data;
    bool hdr_output = false;
    float hdr_headroom = 0.0f;
    bool presenter_transfer_written = false, presenter_written = false;
    bool trace_stages = false;
    uint32_t trace_sequence = 0;
    bool force_content_probe = false;
    NativeFrameTask prep, record;
  } stream_;
  // Predictions for the next streamed frame: the previous Present's.
  uint32_t stream_next_frame_ = 0;
  uint32_t stream_last_width_ = 0, stream_last_height_ = 0;
  std::shared_ptr<const NativeTextureResource> stream_last_present_source_;
  bool stream_last_hdr_output_ = false;
  float stream_last_hdr_headroom_ = 0.0f;
  bool stream_presenter_seen_ = false;  // a callback ran since the last flush
  bool stream_has_water_ = false;
  // Preparation command buffers, one per command pool (pools alternate with the slots).
  std::array<std::pair<VkCommandPool, VkCommandBuffer>, 2> stream_prep_buffers_{};
  // Where the commands being recorded are: assembly_frame_ while streaming (it follows the
  // swaps of SwapAssemblyFrame), current_frame_ otherwise.
  NativeOwnedCommands<NativeCommand>* recording_frame_ = &current_frame_;
  VkDeviceSize stream_constant_reserve_ = VkDeviceSize(32) << 20;
  uint32_t stream_draw_reserve_ = 0, stream_resolve_reserve_ = 0;
  uint64_t stream_frames_ = 0, stream_flushes_ = 0, stream_classic_frames_ = 0,
           stream_frame_mismatches_ = 0, stream_source_mismatches_ = 0, stream_pumps_ = 0,
           stream_presenter_fallbacks_ = 0;
  uint64_t stream_failed_frames_ = 0;
  uint64_t stream_report_tick_ = 0;
  // EnsureFrameConstantArenaCapacity for a streamed frame: sized before it is assembled.
  bool stream_capacity_planning_ = false;
  const char* stream_last_reason_ = "";
  // Authoritative render-worker state. State commands mutate this object in
  // place; immutable snapshots are created only for commands that are retained
  // for later frame recording.
  NativePipelineState pipeline_state_{};
  uint64_t pipeline_snapshot_reuses_=0,shader_snapshot_reuses_=0;
  uint64_t zero_dof_skips_=0,postfx_direct_writes_=0;
  std::shared_ptr<const NativePipelineState> last_pipeline_snapshot_;
  const NativeTargetSnapshot* last_pipeline_snapshot_targets_ = nullptr;
  std::shared_ptr<const NativeShaderState> last_shader_snapshot_;
  // 33: worker snapshot keep-alive. The worker makes a NativeShaderState and a
  // NativePipelineState for nearly every draw (constants and texture / stream
  // bindings change per draw), and the command retirer on a Little core used
  // to drop the last references: ~50 % of its time went to those reference
  // counts (control blocks written on the worker's core) and destructors,
  // including the constant versions behind each shader state. Now the worker
  // keeps the frame's snapshots in one keep-alive and commands hold aliases of
  // it; the retirer only decrements the keep-alive's own count. Once every
  // command of a frame is retired (use_count() == 1), the worker itself drops
  // the keep-alive at a later Present, so the snapshots die on the core that
  // made them, from its own allocator caches. Render worker only.
  struct SnapshotKeepAlive {
    SnapshotKeepAlive();
    ~SnapshotKeepAlive();
    std::vector<std::shared_ptr<const void>> refs;
  };
  std::shared_ptr<SnapshotKeepAlive> snapshot_keepalive_;
  std::deque<std::shared_ptr<SnapshotKeepAlive>> retired_snapshot_keepalives_;
  const void* snapshot_keepalive_last_shader_ = nullptr;
  const void* snapshot_keepalive_last_pipeline_ = nullptr;
  std::atomic<uint32_t> snapshot_keepalives_waiting_{0};  // diagnostics (retire stats line)
  void RollSnapshotKeepAlive();
  template <typename T>
  std::shared_ptr<const T> KeepSnapshot(std::shared_ptr<const T> strong, const void*& last,
                                        NativeCommand& command) {
    if (!strong || !snapshot_keepalive_) return strong;
    const T* raw = strong.get();
    // A reused snapshot (last_*_snapshot_) is already in this keep-alive; the
    // keep-alive holds it, so its address cannot be reused meanwhile.
    if (raw != last) {
      snapshot_keepalive_->refs.push_back(std::move(strong));
      last = raw;
    }
    if (!command.snapshot_keepalive) {
      command.snapshot_keepalive = snapshot_keepalive_;
    }
    return std::shared_ptr<const T>(std::shared_ptr<const T>{}, raw);
  }
  // Render worker: the last prefix hashed per bank (vertex, pixel), reused
  // while consecutive draws upload the same version.
  struct ConstantUploadHashMemo {
    std::shared_ptr<const ConstantStateVersion> version;
    uint32_t bytes = 0;
    uint64_t hash = 0;
  };
  std::array<ConstantUploadHashMemo, 2> constant_upload_hash_memo_{};
  std::shared_ptr<const NativePipelineState> SnapshotPipeline(const NativeCommand&, bool);
  NativeOwnedCommands<NativeCommand> current_frame_;
  DirtyStateDelta producer_dirty_delta_;     // command_capture_mutex_
  struct CapacityPointerHash {
    size_t operator()(uintptr_t value) const noexcept { return size_t(NativeMixHash(value)); }
  };
  NativeFlatSet<uintptr_t, CapacityPointerHash> capacity_vertex_versions_;
  NativeFlatSet<uintptr_t, CapacityPointerHash> capacity_pixel_versions_;
  std::vector<NativeCommand*> capacity_constant_commands_;
  // Upper bound of the frame's constant arena use, accumulated by the render
  // worker as it assembles draws (pipelined recording), so the recorder need
  // not walk every command before recording. A version change between
  // consecutive draws counts a new bank: versions never recur once replaced
  // on one device, and interleaved devices only overcount.
  struct NativeConstantBound {
    VkDeviceSize bytes = 0;
    size_t draws = 0;
    const void* last_vertex = nullptr;
    const void* last_pixel = nullptr;
    // PrepareFrameTextures' descriptor pool sizing inputs.
    uint32_t all_draws = 0;  // Including draws without constant state.
    uint32_t resolves = 0;
    uint32_t depth_handoffs = 0;
    std::vector<uint64_t> packed_alias_generations;  // Distinct.
    // RecordNativeFrame's scans: a draw with a water-surface shader, and the
    // epoch stamped on every texture the frame reads (assembly_read_epoch).
    bool has_water = false;
    uint32_t read_epoch = 0;
    bool valid = false;
  };
  NativeConstantBound assembly_constant_bound_;  // Render worker.
  NativeConstantBound frame_constant_bound_;     // current_frame_'s, set at swap.
  uint64_t upload_on_demand_growths_ = 0;
  uint64_t upload_growths_at_last_plan_ = 0;
  uint32_t upload_frames_since_plan_ = 0;
  // 33: one cache line per slot. At 56 / 72 bytes, slots straddled lines and
  // a lookup in these 512 / 288 KB tables (well past L2) often cost two misses.
  struct alignas(64) BufferCaptureCacheSlot {
    uint32_t handle = 0;
    uint32_t flags = 0;
    uint32_t address = 0;
    uint32_t size = 0;
    uint64_t epoch = 0;
    std::weak_ptr<const NativeBufferResource> resource;
    // 32: `resource` is already held by the current capture keep-alive
    // (capture_keepalive_serial_ == alias_serial): hand out an alias of the
    // keep-alive without touching the resource's reference count.
    const NativeBufferResource* alias = nullptr;
    uint32_t alias_serial = 0;
    bool recent = false;
  };
  // Two-way sets under a multiplicative hash: far views draw a few thousand
  // distinct buffers a frame, and a direct-mapped table indexed by handle bits
  // sent colliding ones to the locked map on every draw.
  template <typename Slots>
  static auto& CaptureCacheWay(Slots& slots, uint32_t handle) {
    constexpr size_t kSets = std::tuple_size_v<Slots> / 2;
    static_assert(kSets && !(kSets & (kSets - 1)));
    const size_t set = size_t((handle * 0x9E3779B1u) >> (32 - std::countr_zero(kSets)));
    auto& first = slots[set * 2];
    // 54: a hit in the first way touches only its own line. The table is 512 / 256 KB, a
    // lookup is a miss, and the old code also wrote second.recent on every lookup: a second
    // line fetched for a hit in the first. The LRU bit now lives in the first slot alone
    // (first.recent: the first way is the more recent); after every lookup the old pair was
    // complementary, so the choice of victim is the same. second.recent is unused.
    if (first.handle == handle) {
      if (!first.recent) first.recent = true;
      return first;
    }
    auto& second = slots[set * 2 + 1];
    auto* way = second.handle == handle ? &second
                : !first.handle ? &first
                : !second.handle ? &second
                : first.recent ? &second : &first;
    first.recent = way == &first;
    return *way;
  }
  template <typename Slots>
  static void ForgetCaptureCache(Slots& slots, uint32_t handle) {
    constexpr size_t kSets = std::tuple_size_v<Slots> / 2;
    const size_t set = size_t((handle * 0x9E3779B1u) >> (32 - std::countr_zero(kSets)));
    for (size_t way = 0; way < 2; ++way) {
      if (slots[set * 2 + way].handle == handle) {
        slots[set * 2 + way].handle = 0;
        slots[set * 2 + way].alias = nullptr;
        slots[set * 2 + way].alias_serial = 0;
      }
    }
  }
  struct BufferMruEntry {
    uint32_t handle = 0;
    uint32_t alias_serial = 0;
    uint64_t epoch = 0;
    const NativeBufferResource* alias = nullptr;
  };
  std::array<BufferMruEntry, 4> buffer_capture_mru_{};  // command_capture_mutex_
  uint32_t buffer_capture_mru_cursor_ = 0;

  std::array<BufferCaptureCacheSlot, 8192> buffer_capture_cache_{};  // command_capture_mutex_
  std::atomic<uint64_t> buffer_resources_epoch_{1};
  BufferCaptureCacheSlot& BufferCaptureSlot(uint32_t handle) {
    return CaptureCacheWay(buffer_capture_cache_, handle);
  }
  void ForgetBufferCapture(uint32_t handle) {
    for (auto& entry : buffer_capture_mru_) {
      if (entry.handle == handle) {
        entry.handle = 0;
        entry.alias = nullptr;
        entry.alias_serial = 0;
      }
    }
    ForgetCaptureCache(buffer_capture_cache_, handle);
  }
  // Producer-only memo of clean texture captures: same handle and identical
  // image fetch words with no texture-map change since give the same resource, so
  // Prepare, both texture_resource_mutex_ acquisitions and three hash lookups
  // are skipped. Producer-side mutations forget the handle; any other thread
  // that changes the maps bumps the epoch.
  struct alignas(64) TextureCaptureCacheSlot {
    uint32_t handle = 0;
    uint32_t alias_serial = 0;  // 32: see BufferCaptureCacheSlot
    std::array<uint32_t, 6> fetch{};
    // 33: low 32 bits of texture_resources_epoch_ (fits the slot in 64 bytes;
    // the epoch would have to advance 2^32 times between two uses to alias).
    uint32_t epoch = 0;
    bool recent = false;
    std::weak_ptr<const NativeTextureResource> resource;
    const NativeTextureResource* alias = nullptr;
  };
  static_assert(sizeof(TextureCaptureCacheSlot) == 64 && sizeof(BufferCaptureCacheSlot) == 64);
  std::array<TextureCaptureCacheSlot, 4096> texture_capture_cache_{};  // command_capture_mutex_
  std::atomic<uint64_t> texture_resources_epoch_{1};
  TextureCaptureCacheSlot& TextureCaptureSlot(uint32_t handle) {
    return CaptureCacheWay(texture_capture_cache_, handle);
  }
  void ForgetTextureCapture(uint32_t handle) { ForgetCaptureCache(texture_capture_cache_, handle); }
  // 32: capture keep-alive. Every command used to hold its own reference to
  // each captured texture / vertex / index buffer: the title thread took it
  // with weak_ptr::lock (a CAS) and the retirer dropped it, both on the
  // resource's control block, from different clusters, thousands of times a
  // frame. Now the producer keeps one reference per distinct resource per
  // frame in a keep-alive object, and commands hold aliases of the keep-alive
  // (std::shared_ptr aliasing constructor): same pointer, same lifetime
  // guarantee (until the frame's last command retires), but the reference
  // counting happens on one control block that only the producer touches
  // while it captures the frame. A holder that outlives the frame must take
  // an owning reference (OwningTextureResource). command_capture_mutex_.
  struct CaptureKeepAlive {
    CaptureKeepAlive();
    ~CaptureKeepAlive();
    std::vector<std::shared_ptr<const void>> refs;
    // 48: state blocks of the commands captured into this keep-alive. Appended
    // by RenderGuest only; deque elements never move, and a block is read-only
    // once a command pointing at it is queued. Cleared with refs when the
    // keep-alive is reused (no command holds it then).
    std::deque<NativeFixedFunctionState> fixed_blocks;
    std::deque<NativeTargetSnapshot> target_blocks;
  };
  static constexpr size_t kCaptureKeepAliveLimit = 32768;  // roll early beyond this
  std::shared_ptr<CaptureKeepAlive> capture_keepalive_;
  std::deque<std::shared_ptr<CaptureKeepAlive>> retired_capture_keepalives_;
  NativeCommand* active_capture_command_ = nullptr;  // command_capture_mutex_
  uint32_t capture_keepalive_serial_ = 0;  // 0: none; slots compare against it
  void RollCaptureKeepAlive();
  // 48 (тонкая команда): RenderGuest, command_capture_mutex_. The block of the
  // current capture keep-alive equal to `value`: the last one when the state did
  // not change since the previous draw (and the keep-alive did not roll), a new
  // one otherwise. The command then holds the keep-alive.
  const NativeFixedFunctionState* CaptureFixedStateBlock(NativeCommand& command,
                                                         const NativeFixedFunctionState& value,
                                                         bool unchanged);
  const NativeTargetSnapshot* CaptureTargetBlock(NativeCommand& command,
                                                 const NativeTargetSnapshot& value);
  const NativeFixedFunctionState* producer_fixed_block_ = nullptr;
  uint32_t producer_fixed_block_serial_ = 0;
  const NativeTargetSnapshot* producer_target_block_ = nullptr;
  uint32_t producer_target_block_serial_ = 0;
  // 48: render worker (assembly) only. A command whose fixed state the worker
  // changes (depth clamp) gets a variant block it owns (fixed_owner); the last
  // variant is reused while consecutive draws need the same one.
  void SetWorkerFixedState(NativeCommand& command, const NativeFixedFunctionState& value);
  std::shared_ptr<const NativeFixedFunctionState> worker_fixed_variant_;
  // 48: any thread, for a command copy it owns exclusively (diagnostic draws, the
  // temporal prefilter): a private writable copy of the fixed state that the
  // command owns (fixed_owner). The block it pointed at stays untouched.
  static NativeFixedFunctionState& MutableOwnedFixedState(NativeCommand& command) {
    auto owned = std::make_shared<NativeFixedFunctionState>(*command.fixed_function_state);
    NativeFixedFunctionState& state = *owned;
    command.fixed_owner = std::move(owned);
    command.fixed_function_state = NativeStateRef<NativeFixedFunctionState>(&state);
    return state;
  }
  // 48: what the merge scans compared before (all bytes); one block => equal.
  static bool NativeFixedStateBytesEqual(const NativeStateRef<NativeFixedFunctionState>& a,
                                         const NativeStateRef<NativeFixedFunctionState>& b) {
    return a.get() == b.get() ||
           std::memcmp(a.get(), b.get(), sizeof(NativeFixedFunctionState)) == 0;
  }
  template <typename T>
  std::shared_ptr<const T> AliasCaptured(const T* raw) {
    if (active_capture_command_ && capture_keepalive_) {
      active_capture_command_->HoldCaptureKeepAlive(capture_keepalive_);
      return std::shared_ptr<const T>(std::shared_ptr<const T>{}, raw);
    }
    return std::shared_ptr<const T>(capture_keepalive_, raw);
  }
  template <typename T>
  std::shared_ptr<const T> KeepCaptured(std::shared_ptr<const T>&& strong, const T*& alias,
                                        uint32_t& alias_serial) {
    if (!strong) return std::move(strong);
    if (!capture_keepalive_ || capture_keepalive_->refs.size() >= kCaptureKeepAliveLimit)
      RollCaptureKeepAlive();
    const T* raw = strong.get();
    if (raw->keepalive_serial != capture_keepalive_serial_) {
      raw->keepalive_serial = capture_keepalive_serial_;
      capture_keepalive_->refs.push_back(std::move(strong));
    }
    alias = raw;
    alias_serial = capture_keepalive_serial_;
    return AliasCaptured(raw);
  }
  static std::shared_ptr<const NativeTextureResource> OwningTextureResource(
      const std::shared_ptr<const NativeTextureResource>& texture) {
    if (!texture) return texture;
    if (auto owned = texture->weak_from_this().lock()) return owned;
    return texture;
  }
  DirtyDeltaScratch producer_dirty_scratch_;  // command_capture_mutex_
  // Vertex-shader textures: the frame each was last re-read and the generation
  // it produced. command_capture_mutex_
  struct VertexTextureRecheck {
    uint32_t frame = 0;
    uint64_t generation = 0;
  };
  std::unordered_map<uint32_t, VertexTextureRecheck> vertex_texture_rechecks_;
  // Pipelined recording: the worker assembles frame N+1 while the recorder
  // thread records and submits frame N from current_frame_. Commands that
  // touch recorder-owned state wait for the recorder to go idle, swap the
  // assembled frame into current_frame_, and run their original worker path.
  struct NativeReleaseEffect {
    ReleaseResourceCommand release{};
    uint64_t released_texture_generation = 0;
    bool frame_was_empty = false;
  };
  struct NativeRecorderJob {
    NativeCommandPool<NativeCommand>::Owner present_command;
    PresentCommand present{};
    uint32_t ring_serial = 0;  // worker constant ring frame of this job's commands
  };
  void FrameRecorderMain();
  void RunRecorderJob(NativeRecorderJob& job);  // 46: shared by GtaRecorder and GtaBackend
  void WaitForFrameRecorderIdle();
  void StopFrameRecorder();
  void SwapAssemblyFrame();
  void RunReleaseEffects(std::vector<NativeReleaseEffect>& effects);
  void ApplyReleaseResourceEffects(const NativeReleaseEffect& effect);
  void FinishPresent(NativeCommand& command, const PresentCommand& present);
  void SignalGuestFrameComplete(uint32_t device, uint32_t frame);
  void UpdateDynamicDrawDistance();
  NativeGpuCounters gpu_counters_;
  NativePipelineStats pipeline_stats_;
  bool pipeline_stats_attempted_ = false;
  uint32_t pipeline_stats_reports_ = 0;
  bool gpu_counters_attempted_ = false;
  double dynamic_draw_distance_factor_ = 1.0;
  double dynamic_draw_distance_published_ = 1.0;
  double dynamic_draw_distance_interval_ms_ = 33.3;
  uint64_t dynamic_draw_distance_last_tick_ = 0;
  // One-second windows of presented frames; the controller steps once per window.
  uint32_t dynamic_draw_distance_window_frames_ = 0;
  double dynamic_draw_distance_window_ms_ = 0.0;
  std::array<double, 3> dynamic_draw_distance_window_fps_{30.0, 30.0, 30.0};
  uint32_t dynamic_draw_distance_windows_since_drop_ = 1000;
  // B1 30 FPS guard: a ladder of content reductions above the draw distance.
  void UpdateFpsGuard(double interval_ms, uint64_t now, uint64_t frequency);
  void ApplyFpsGuardLevel(uint32_t level);
  uint32_t fps_guard_level_ = 0;
  uint32_t fps_guard_applied_level_ = UINT32_MAX;
  uint64_t fps_guard_changed_tick_ = 0;
  uint64_t fps_guard_slow_since_tick_ = 0;
  uint64_t fps_guard_fast_since_tick_ = 0;
  NativeOwnedCommands<NativeCommand> assembly_frame_;
  // Worker-side protection visible to the recorder's eviction passes. Guarded
  // by worker_protection_mutex_ together with the worker batch and cursor.
  std::unordered_set<uint64_t> assembly_texture_protection_;
  // Bumped whenever assembly_texture_protection_ is replaced, so a resource
  // stamped with the current epoch is known to be in the set already.
  uint32_t assembly_protection_epoch_ = 1;
  std::vector<NativeReleaseEffect> assembly_release_effects_;
  std::vector<NativeReleaseEffect> recording_release_effects_;
  alignas(64) mutable std::mutex worker_protection_mutex_;
  std::thread frame_recorder_;
  alignas(64) std::mutex frame_recorder_mutex_;
  std::condition_variable frame_recorder_condition_;
  std::optional<NativeRecorderJob> frame_recorder_job_;
  bool frame_recorder_busy_ = false;
  bool frame_recorder_stop_ = false;
  bool pipelined_recording_ = false;
  // 46: gta4_native_backend_thread: the render worker records frames itself.
  bool inline_recording_ = false;
  bool assembly_modern_frame_pending_ = false;
  uint64_t pipelined_sync_waits_ = 0;
  uint64_t pipeline_sync_wait_ticks_ = 0;
  uint64_t pipeline_present_wait_ticks_ = 0;
  uint32_t pipeline_report_frames_ = 0;
  std::array<uint32_t, 64> pipeline_sync_type_counts_{};
  std::unordered_map<uint32_t, EnvironmentalDataV2> recording_environmental_data_by_device_;
  NativeCommandRetirer<NativeCommand> command_retirer_{native_command_pool_};
  NativeDeferredReleaser<ConstantStateVersion> constant_releaser_;
  bool ResetImmutableBindings(NativeFrameConstantArena& arena);
  // Moves staged draws into render_queue_. Requires command_capture_mutex_ and
  // render_mutex_. also_published: the command queued right after the batch in
  // the same critical section (its staged texture references move with it).
  void PublishStagedDrawCommandsLocked(const NativeCommand* also_published = nullptr);
  NativeFrameResources recording_resources_;
  std::unordered_set<uint64_t> frame_texture_protection_;
  const NativeCommand* active_worker_command_ = nullptr;
  NativeLightingLineageLedger semantic_light_setup_lineage_;
  std::unordered_map<uint32_t, EnvironmentalDataV2> environmental_data_by_device_;
  std::unordered_map<uint32_t, uint64_t> environmental_data_hash_by_device_;
  std::unordered_map<uint32_t, NativeDeviceConstantState> device_constant_states_;
  std::vector<uint8_t> shader_cache_data_;
  std::vector<std::unique_ptr<NativeShader>> shader_resources_;
  std::unordered_map<uint64_t, NativeShader*> pixel_shaders_by_hash_;
  std::unordered_map<uint64_t, NativeShader*> vertex_shaders_by_hash_;
  std::unordered_map<uint32_t, NativeShader*> shader_handles_;
  std::unordered_map<uint32_t, ProducerShaderMetadata> producer_shader_metadata_;
  std::unordered_map<uint32_t, ProducerDeviceShaderState> producer_device_shader_states_;
  std::unordered_map<uint32_t, std::shared_ptr<NativeVertexDeclaration>> vertex_declarations_;
  std::mutex buffer_resource_mutex_;
  std::unordered_map<uint32_t, std::shared_ptr<const NativeBufferResource>> buffer_resources_;
  std::unordered_set<uint32_t> dirty_buffer_handles_;
  uint64_t next_buffer_generation_ = 1;
  std::mutex texture_resource_mutex_;
  std::unordered_map<uint32_t, std::shared_ptr<const NativeTextureResource>> texture_resources_;
  std::unordered_set<uint32_t> dirty_texture_handles_;
  std::unordered_map<uint32_t, uint32_t> vector_font_ids_;
  NativeVirtualResourceRegistry virtual_resource_registry_;
  uint64_t next_texture_generation_ = 1;
  alignas(64) gta4::InlineMutex command_capture_mutex_;
  // State commands absorbed since the last queued command (command_capture_mutex_).
  std::vector<uint8_t> producer_pending_state_;
  // 29.10: texture references of captured commands until they are published
  // (StageTextureProtection / PublishStagedTextureProtectionLocked). The worker
  // never takes this lock; readers take it after render_mutex_.
  // Lock order: render_mutex_, staged_protection_mutex_, queued_protection_mutex_.
  alignas(64) NativeTextureProtectionIndex staged_texture_protection_;
  mutable std::mutex staged_protection_mutex_;
  // 49 (шаг 3.2): queue-side texture protection by capture epoch
  // (native_queue_protection.h) instead of the staging / queue indices.
  void ProtectInQueueEpoch(NativeCommand& command);  // RenderGuest, per queued command
  NativeQueueProtectionEpochs queue_epochs_;
  // 49 (шаг 3.2): RenderGuest's copy of the constant banks the worker holds
  // (AuthoritativeConstantState::canonical), i.e. what every queued delta, in
  // queue order, left there. Dirty groups whose bytes are unchanged are not
  // sent: the worker neither receives nor compares them. command_capture_mutex_.
  struct ProducerConstantShadow {
    std::vector<uint8_t> vertex, pixel;
    bool vertex_valid = false, pixel_valid = false;
  };
  void CommitProducerConstantShadow(const NativeCommand& command);
  std::unordered_map<uint32_t, ProducerConstantShadow> producer_constant_shadows_;
  uint32_t producer_constant_commit_device_ = 0;
  bool producer_constant_commit_ = false;
  uint32_t producer_constant_resync_seen_ = 0;
  uint64_t producer_constant_dirty_bytes_ = 0, producer_constant_sent_bytes_ = 0;  // this frame
  // The worker asks for complete banks after it rejected a delta (the shadow
  // may then differ from its banks): RenderGuest resynchronizes on its next draw.
  std::atomic<uint32_t> constant_resync_requests_{0};
  std::atomic<uint64_t> last_frame_constant_dirty_bytes_{0}, last_frame_constant_sent_bytes_{0};
  std::mutex device_snapshot_mutex_;
  std::unordered_map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>> last_device_snapshots_;
  std::unordered_set<uint32_t> initialized_constant_capture_devices_;
  // Last device known to be in the set above, so the per-draw check and
  // insert skip device_snapshot_mutex_. Zero after any erase or clear.
  std::atomic<uint32_t> producer_initialized_constant_device_{0};
  uint64_t next_vertex_declaration_generation_ = 1;
  bool shader_cache_load_attempted_ = false;
  bool shader_cache_initialized_ = false;
  ShaderOverrideMode shader_override_mode_ = ShaderOverrideMode::kPair;
  ModernShaderFramePolicy modern_shader_frame_;
  uint32_t producer_postfx_half_scene_ = 0;
  PostFxScheduler postfx_scheduler_;
  std::shared_ptr<const NativeTextureResource> postfx_filtered_scene_;
  ModernShaderDiagnostics modern_diagnostics_;
  uint32_t shader_registration_count_ = 0;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkCommandPool secondary_command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer secondary_command_buffer_ = VK_NULL_HANDLE;
  VkRenderPass clear_render_pass_ = VK_NULL_HANDLE;
  VkFramebuffer clear_framebuffer_ = VK_NULL_HANDLE;
  uint64_t clear_framebuffer_image_version_ = 0;
  std::shared_ptr<ui::vulkan::VulkanSubmissionTracker> submission_tracker_;
  uint64_t command_buffer_submission_ = 0;
  uint64_t secondary_command_buffer_submission_ = 0;
  uint64_t completed_command_buffer_submission_ = 0;
  bool native_renderer_recovery_failed_ = false;
  uint32_t active_frame_slot_ = 0;
  // Occlusion of the scene water draws, per frame slot (A13 reflection on demand).
  static constexpr uint32_t kWaterQueriesPerSlot = 64;
  VkQueryPool water_query_pool_ = VK_NULL_HANDLE;
  std::array<uint32_t, 2> water_query_counts_{};
  uint32_t water_hidden_frames_ = 0;
  // Last frame each texture generation was an input (A3 lazy resolves), and the
  // guest textures the title ever read back on the CPU (never skipped).
  std::unordered_map<uint64_t, uint32_t> texture_input_frames_;
  std::mutex cpu_read_textures_mutex_;
  std::unordered_set<uint32_t> cpu_read_textures_;
  NativeFrameContextRing frame_context_ring_;
  std::array<std::optional<NativeFrameContextRing::FrameToken>, NativeFrameContextRing::kSlotCount>
      frame_context_tokens_{};
  std::array<uint64_t, NativeFrameContextRing::kSlotCount> frame_context_serials_{};
  NativeUploadBuffer fusion_tone_lut_;
  std::vector<NativeUploadBuffer> fusion_cloud_buffers_;
  uint32_t fusion_cloud_buffer_index_ = UINT32_MAX;
  PostFxExtent fusion_cloud_extent_{};
  bool fusion_cloud_ready_ = false;
  NativeUploadBuffer upload_buffer_;
  NativeUploadBuffer secondary_upload_buffer_;
  std::vector<NativeUploadBuffer> overflow_upload_buffers_;
  std::vector<NativeUploadBuffer> secondary_overflow_upload_buffers_;
  std::unique_ptr<NativeBufferArena> persistent_buffer_arena_;
  std::shared_ptr<NativeOwnerRetirementQueue<NativePersistentBufferEntry>>
      persistent_buffer_retirements_;
  std::unordered_map<uint64_t, NativePersistentBufferBlock> persistent_buffer_blocks_;
  uint64_t persistent_arena_reclamation_epoch_ = 0;
  NativePersistentBufferMap<NativePersistentBufferKey, NativePersistentBufferEntry,
                            NativePersistentBufferKeyHash>
      persistent_buffers_;
  // Bumped when persistent_buffers_ is cleared; owners' memos of older epochs
  // are ignored (NativeBufferResource::persistent_memo_entry).
  uint64_t persistent_buffer_epoch_ = 1;
  std::array<NativeFrameConstantArena, NativeFrameContextRing::kSlotCount> frame_constant_arenas_{};
  uint64_t persistent_buffer_hits_ = 0;
  uint64_t persistent_buffer_misses_ = 0;
  uint64_t persistent_buffer_upload_bytes_ = 0;
  uint32_t upload_underutilized_frame_count_ = 0;
  uint32_t secondary_upload_underutilized_frame_count_ = 0;
  uint32_t constant_arena_underutilized_frames_ = 0;
  NativeContentProbeBuffer content_probe_buffer_;
  NativeContentProbeBuffer secondary_content_probe_buffer_;
  NativeContentProbeBuffer tv_content_probe_buffer_, secondary_tv_content_probe_buffer_;
  std::shared_ptr<TvTraceContext> tv_frame_trace_;
  // Worker-only diagnostic provenance; never participates in image ownership.
  std::shared_ptr<TvTraceContext> tv_lifecycle_trace_;
  std::set<uint32_t> tv_lifecycle_plane_handles_;
  uint64_t tv_lifecycle_id_ = 0, tv_lifecycle_command_sequence_ = 0;
  uint32_t tv_lifecycle_batch_frame_ = 0;
  bool IsTvLifecycleImage(const NativeTextureImage& image) const;
  void TraceTvImageLifecycle(std::string_view point, const NativeTextureImage& image,
                            std::string_view reason, std::string_view details = {});
  std::set<std::pair<uint64_t,uint64_t>> tv_exported_cpu_generations_;
  uint32_t TvCommandRole(const NativeCommand& command) const;
  void TraceTvNativeCommand(const NativeCommand& command);
  void RecordTvImage(VkCommandBuffer cb,const NativeCommand& command,uint32_t checkpoint,
      uint32_t attachment,NativeSurfaceImage* surface,NativeTextureImage* texture,std::string_view role);
  void RecordTvTarget(VkCommandBuffer cb,const NativeCommand& command,
      const NativeRenderingTarget& target,uint32_t checkpoint);
  void RecordTvInputs(VkCommandBuffer cb,const NativeCommand& command);
  void RecordTvResolve(VkCommandBuffer cb,const NativeCommand& command,uint32_t checkpoint);
  static void PublishTvProbe(const NativeContentProbeStage& stage,const uint8_t* bytes,bool visible);
  void AnalyzePendingTvProbe(uint32_t slot,uint64_t submission);
  struct NativeBulbFullProbe {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceSize allocation_size = 0;
    uint32_t memory_type = UINT32_MAX;
    uint32_t frame = 0, command_index = 0, fixture = 0, width = 0, height = 0;
    uint32_t format = 0, aspect = 0, block_extent = 1, bytes_per_block = 0, layout = 0, handle = 0;
    uint64_t run = 0, source_sequence = 0, sequence = 0, submission = 0, bytes = 0;
    uint64_t image = 0, view = 0, lifetime = 0, generation = 0, pixel_shader = 0, vertex_shader = 0;
    std::array<uint32_t, 4> components{};
    std::string role;
  };
  struct FireFullProbe {
    NativeBulbFullProbe data;
    VkImage scratch = VK_NULL_HANDLE;
    VkDeviceMemory scratch_memory = VK_NULL_HANDLE;
    uint64_t occurrence = 0, event = 0;
    uint32_t mip = 0, samples = 1;
    uint32_t crop_x = 0, crop_y = 0, original_width = 0, original_height = 0;
  };
  struct FireQueryRow {
    uint32_t frame = 0, command = 0;
    uint64_t sequence = 0, occurrence = 0, event = 0;
  };
  struct FireQueryBatch {
    VkQueryPool pool = VK_NULL_HANDLE;
    uint64_t submission = 0;
    bool ready = false;
    std::vector<FireQueryRow> rows;
  };
  std::array<std::vector<FireFullProbe>, 2> fire_probes_;
  std::array<FireQueryBatch, 2> fire_queries_;
  bool fire_frame_ = false, fire_images_ = false, fire_event_active_ = false;
  uint32_t fire_last_logged_frame_ = 0;
  size_t fire_first_draw_index_ = SIZE_MAX, fire_last_ui_index_ = SIZE_MAX;
  uint64_t fire_frame_bytes_ = 0, fire_cpu_bytes_ = 0, fire_capture_request_ = 0;
  std::unordered_map<std::string, uint32_t> fire_checkpoints_;
  std::unordered_set<uint64_t> fire_input_images_, fire_cpu_resources_;
  std::chrono::steady_clock::time_point fire_last_seen_, fire_last_images_;
  void BeginFireFrame(VkCommandBuffer cb, uint32_t frame);
  void TraceFireCommand(std::string_view point, const NativeCommand& command, std::string_view result);
  bool SelectFireCheckpoint(const NativeCommand& command, const NativeRenderingTarget& target);
  void FireImage(VkCommandBuffer cb, std::string_view role, const NativeCommand* command = nullptr,
      NativeSurfaceImage* surface = nullptr, NativeTextureImage* texture = nullptr,
      VkImage direct = VK_NULL_HANDLE, VkFormat format = VK_FORMAT_UNDEFINED,
      VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED, uint32_t width = 0, uint32_t height = 0,
      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t mip = 0);
  void FireTarget(VkCommandBuffer cb, const NativeCommand& command, const NativeRenderingTarget& target, bool after);
  void FireInputs(VkCommandBuffer cb, const NativeCommand& command);
  uint32_t BeginFireQuery(VkCommandBuffer cb, const NativeCommand& command, uint32_t existing);
  void EndFireQuery(VkCommandBuffer cb, uint32_t index);
  void PublishFireProbes(uint32_t slot, uint64_t submission);
  void DestroyFireProbes();

  // Indexed by physical frame slot; never swapped with the hot-member aliases.
  std::array<std::vector<NativeBulbFullProbe>, 2> bulb_full_probes_;
  uint32_t bulb_full_frame_ = 0;
  uint64_t bulb_full_bytes_ = 0;
  bool NeedsBulbInterference(const NativeCommand& command, const NativeRenderingTarget& target) const;
  void RecordBulbFullImage(VkCommandBuffer cb, std::string_view role, const NativeCommand* command = nullptr,
      NativeSurfaceImage* surface = nullptr, NativeTextureImage* texture = nullptr,
      VkImage direct = VK_NULL_HANDLE, VkFormat format = VK_FORMAT_UNDEFINED,
      VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED, uint32_t width = 0, uint32_t height = 0,
      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
  void RecordBulbInterferenceBefore(VkCommandBuffer cb, const NativeCommand& command, const NativeRenderingTarget& target);
  void PublishBulbFullProbe(uint32_t slot, uint64_t submission);
  void DestroyBulbFullProbes();
  struct BulbRoute {
    std::shared_ptr<const BulbSourceSnapshot> source;
    uint64_t sequence = 0;
    uint32_t command_index = 0, writers = 0;
    bool recorded = false, budget_reported = false;
    BulbScreenRegion region;
    std::set<std::pair<uint64_t, uint64_t>> images;
    std::set<uint64_t> generations;
  };
  std::vector<BulbRoute> bulb_routes_;
  std::optional<BulbRoute> bulb_prospective_route_;
  bool bulb_trace_frame_ = false;
  std::atomic<bool> bulb_source_capture_done_{false};
  uint32_t bulb_capture_count_ = 0, bulb_last_frame_ = 0;
  uint64_t bulb_run_ = 0;
  uint32_t bulb_selected_fixture_ = UINT32_MAX;
  std::array<uint32_t,kBulbFixtures.size()> bulb_fixture_visits_{};
  const NativeCommand* bulb_current_command_ = nullptr;
  NativeContentProbeBuffer bulb_content_probe_buffer_, secondary_bulb_content_probe_buffer_;
  void CaptureBulbSource(NativeCommand& command, std::span<const uint8_t> device_state);
  void BeginBulbFrame(uint32_t frame);
  void TraceBulbCommand(std::string_view point, const NativeCommand& command, std::string_view reason);
  bool PrepareBulbDraw(const NativeCommand& command, const NativeRenderingTarget& target);
  bool BulbRouteTouches(const BulbRoute& route, const NativeCommand& command, const NativeRenderingTarget& target) const;
  bool NeedsBulbCheckpoint(const NativeCommand& command, const NativeRenderingTarget& target) const;
  void RecordBulbImage(VkCommandBuffer cb, BulbRoute& route, std::string_view role,
      NativeSurfaceImage* surface = nullptr, NativeTextureImage* texture = nullptr,
      VkImage direct_image = VK_NULL_HANDLE, VkFormat direct_format = VK_FORMAT_UNDEFINED,
      VkImageLayout direct_layout = VK_IMAGE_LAYOUT_UNDEFINED, uint32_t direct_width = 0, uint32_t direct_height = 0,
      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, bool full_texture = false);
  void RecordBulbPostFx(VkCommandBuffer cb, const NativeCommand& command, NativeTextureImage* image, std::string_view role, bool recorded);
  void RecordBulbBefore(VkCommandBuffer cb, const NativeCommand& command, const NativeRenderingTarget& target);
  void RecordBulbAfter(VkCommandBuffer cb, const NativeCommand& command, const NativeRenderingTarget& target, bool recorded);
  void TraceBulbResolve(VkCommandBuffer cb, const NativeCommand& command, NativeSurfaceImage* source,
      NativeTextureImage* destination, std::string_view result, std::string_view reason);
  void RecordBulbPresentation(VkCommandBuffer cb, NativeSurfaceImage* final_surface, NativeTextureImage* composite_input,
      const std::shared_ptr<const NativeTextureResource>& present_source, VkImage image, VkImageLayout layout,
      uint32_t width, uint32_t height);
  static void PublishBulbProbe(const NativeContentProbeStage& stage, const uint8_t* bytes, bool visible);
  void AnalyzePendingBulbProbe(uint32_t slot, uint64_t submission);
  NativeContentProbeBuffer phone_content_probe_buffer_;
  NativeContentProbeBuffer secondary_phone_content_probe_buffer_;
  std::shared_ptr<PhoneTraceContext> phone_frame_trace_;
  uint64_t phone_record_event_ = 0;
  void TracePhoneNativeCommand(std::string_view point, const NativeCommand& command);
  void RecordPhoneProbe(VkCommandBuffer command_buffer, const NativeCommand& command,
                        const NativeRenderingTarget& target, uint32_t frame,
                        uint32_t command_index, uint32_t point);
  static void PublishPhoneProbe(const NativeContentProbeStage& stage,
                                const uint8_t* bytes, bool visible);
  void AnalyzePendingPhoneProbe(uint32_t slot, uint64_t submission);
  void RecordPhoneLineageImage(VkCommandBuffer command_buffer, const NativeCommand& command,
      uint32_t frame, uint32_t command_index, uint32_t point, uint32_t attachment,
      NativeSurfaceImage* surface, NativeTextureImage* texture, std::string_view role);
  void RecordPhoneLineageInputs(VkCommandBuffer command_buffer, const NativeCommand& command,
      uint32_t frame, uint32_t command_index);
  void RecordPhoneHandoffProbe(VkCommandBuffer command_buffer, const NativeCommand& command,
      uint32_t frame, uint32_t command_index, uint32_t point);
  bool phone_lineage_draw_active_ = false;
  NativeLightStencilHistogramBuffer light_stencil_histogram_buffer_;
  NativeLightStencilHistogramBuffer secondary_light_stencil_histogram_buffer_;
  uint32_t light_stencil_histogram_capture_frame_ = 0;
  bool light_stencil_histogram_capture_complete_ = false;
  uint32_t light_stencil_histogram_readback_frame_ = 0;
  NativeLightColorDeltaBuffer light_color_delta_buffer_;
  NativeLightColorDeltaBuffer secondary_light_color_delta_buffer_;
  uint32_t light_color_delta_capture_frame_ = 0;
  bool light_color_delta_capture_complete_ = false;
  uint32_t light_color_delta_readback_frame_ = 0;
  NativeRoomLightProbeQueue room_light_probe_queue_;
  std::set<NativeRoomLightProbeKey> room_light_probe_unmapped_;
  std::optional<NativeRoomLightProbeKey> room_light_probe_selection_;
  std::shared_ptr<NativeRoomLightProbeReceipt> room_light_probe_receipt_;
  std::vector<std::shared_ptr<NativeRoomLightProbeReceipt>> room_light_probe_receipts_;
  std::set<NativeRoomLightProbeKey> room_light_probe_previous_visible_;
  std::map<size_t, NativeRoomLightProbeKey> room_light_probe_visible_commands_;
  bool room_light_probe_stop_requested_ = false;
  bool room_light_probe_drained_ = false;
  uint32_t room_light_probe_frame_ = 0;
  size_t room_light_probe_command_index_ = SIZE_MAX;
  size_t room_light_probe_setup_index_ = SIZE_MAX;
  bool room_light_probe_stencil_expected_ = false;
  uint32_t light_depth_matrix_capture_frame_ = 0;
  uint8_t light_depth_matrix_capture_mask_ = 0;
  bool light_depth_matrix_capture_complete_ = false;
  NativeTranslucentQueryState translucent_query_state_;
  NativeTranslucentQueryState secondary_translucent_query_state_;
  NativeGpuProfileState native_gpu_profile_state_;
  profile::CpuRecorder native_cpu_recorder_;
  profile::TransportSummary native_profile_transport_;
  uint64_t native_profile_clock_probe_ticks_ = UINT64_MAX;
  struct NativeMemoryProfileState {
    bool active = false;
    bool autostart_consumed = false;
    uint32_t marker = 0;
    uint32_t observed_marker_request = 0;
    uint64_t start_host_tick = 0;
    uint64_t last_sample_host_tick = 0;
    uint64_t last_vm_scan_request_host_tick = 0;
    uint64_t next_retained_capture_index = 0;
    memory::SnapshotSeries samples;
    memory::LifecycleEventSeries events;
    std::vector<memory::RetainedResource> retained;
    uint64_t dropped_retained = 0;
    std::mutex event_mutex;
    std::mutex vm_scan_mutex;
    memory::Snapshot latest_vm_scan;
    std::atomic<bool> vm_scan_running{false};
    std::thread vm_scan_thread;
    std::thread export_thread;
  } native_memory_profile_state_;
  PostFxResourcePool postfx_resource_pool_;
  temporal::CommandCapture temporal_command_capture_;
  std::mutex temporal_provider_mutex_;
  temporal::FsrUpscalerVulkan fsr_upscaler_;
  temporal::DlssUpscaler dlss_upscaler_;
  temporal::GeometryHistory temporal_geometry_history_;
  temporal::ShaderArchive temporal_shader_archive_;
  std::map<std::tuple<uint64_t, uint32_t, bool, bool>, VkShaderModule> temporal_shader_modules_;
  temporal::VulkanScene temporal_scene_;
  std::unique_ptr<NativeTextureImage> temporal_output_binding_;
  temporal::VulkanFrameGeneration temporal_frame_generator_;
  temporal::FrameGenerationOutput temporal_generated_frame_;
  std::array<std::vector<std::shared_ptr<void>>, 2> temporal_frame_leases_;
  NativeSurfaceImage* temporal_composite_source_ = nullptr;
  NativeTextureImage temporal_display_texture_;
  bool temporal_display_ready_ = false, temporal_ui_exact_ = true;
  VkImageMemoryBarrier temporal_presenter_acquire_{};
  VkPipelineStageFlags temporal_presenter_acquire_stages_ = 0;
  bool temporal_presenter_acquired_ = false, temporal_provider_recreate_ = false;
  VkFence temporal_partial_fence_ = VK_NULL_HANDLE;
  bool temporal_partial_pending_ = false;
  uint64_t temporal_display_present_generation_ = 0;
  TemporalCommand temporal_metadata_{};
  TemporalUpscalerProvider temporal_provider_ = TemporalUpscalerProvider::kFsr3;
  temporal::Quality temporal_quality_ = temporal::Quality::kQuality;
  temporal::Configuration temporal_configuration_{};
  temporal::FrameInput temporal_encoded_frame_{};
  NativeSurfaceImage* temporal_depth_source_ = nullptr;
  uint64_t temporal_composite_scope_ = 0, temporal_last_sequence_ = 0,
           temporal_last_time_ns_ = 0, temporal_last_epoch_ = 0;
  std::array<float, 16> temporal_previous_projection_{}, temporal_previous_inverse_{};
  std::array<float, 2> temporal_half_pixel_{};
  bool temporal_assets_checked_ = false, temporal_assets_ready_ = false,
       temporal_frame_prepared_ = false, temporal_active_ = false, temporal_resolved_ = false,
       temporal_encoded_ = false, temporal_motion_complete_ = false,
       temporal_camera_adopted_ = false, temporal_history_valid_ = false;
  bool temporal_capabilities_checked_ = false;
  uint32_t temporal_capabilities_ = 0;
  struct TemporalQuerySelection {
    TemporalUpscalerProvider provider;
    temporal::Quality quality;
    temporal::Extent render, output;
  };
  std::map<uint64_t, TemporalQuerySelection> temporal_query_selections_;
  uint64_t temporal_world_draws_ = 0, temporal_history_draws_ = 0;
  bool EnsureTemporalAssets();
  uint32_t QueryTemporalCapabilities();
  bool PrepareTemporalFrame();
  void ObserveTemporalCommand(VkCommandBuffer, const TemporalCommand&);
  void ClassifyTemporalDraw(const NativeCommand&, NativeRenderingTarget&);
  bool ResolveTemporalScene(VkCommandBuffer, const NativeCommand&, const NativeRenderingTarget&,
                            NativeFrameResources&);
  bool BindTemporalDraw(VkCommandBuffer, const NativeCommand&, const NativeRenderingTarget&,
                       VkPipelineLayout, const NativeUploadAllocation&,
                       const NativeUploadAllocation&, const NativeUploadAllocation&);
  VkShaderModule GetTemporalShaderModule(const NativeShader*, bool overridden, bool late);
  void SubmitTemporalFrame();
  void DiscardTemporalFrame();
  void DestroyTemporalResources();
  bool ReplayTemporalDisplayDraw(VkCommandBuffer, const NativeCommand&,
                                const NativeRenderingTarget&, NativeFrameResources&);
  bool ReplayTemporalDraw(VkCommandBuffer, const NativeCommand&,
                          const NativeRenderingTarget&, NativeFrameResources&,
                          temporal::Image&, bool clear);
  bool EncodeTemporalGeneration(VkCommandBuffer);
  void AcquireNativePresenterImage(VkCommandBuffer);
  bool CompleteTemporalCoverage(VkCommandBuffer, bool& valid);
  SmaaPipeline smaa_pipeline_;
  SplitPostFxPass split_postfx_pass_;
  SunShaftsPass sun_shafts_pass_;
  NativeImageResource null_texture_2d_;
  NativeImageResource null_texture_2d_array_;
  NativeImageResource null_texture_3d_;
  NativeImageResource null_texture_cube_;
  VkSampler null_sampler_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, 2> descriptor_set_layouts_{};
  std::array<VkDescriptorSetLayout, 1> cached_descriptor_set_layouts_{};
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<std::array<VkDescriptorSet, 2>, 2> descriptor_sets_{};
  NativeDescriptorBackend native_descriptor_backend_ = NativeDescriptorBackend::kCached;
  std::unique_ptr<NativeStableDescriptorSlotTable> native_stable_image_descriptor_table_;
  std::unique_ptr<NativeStableDescriptorSlotTable> native_stable_sampler_descriptor_table_;
  std::vector<NativeDescriptorPage> native_descriptor_pages_;
  bool native_descriptor_paging_ = false;
  std::array<NativeDescriptorWorkingSet<kTextureStageCount>, NativeFrameContextRing::kSlotCount>
      native_descriptor_working_sets_;
  bool BeginIndexedWorkingSet();
  template <typename DescriptorKey>
  bool AssignIndexedWorkingSet(NativeCommand& command, const DescriptorKey& key);
  bool PublishIndexedWorkingSet();
  std::unique_ptr<NativeCachedDescriptorState> native_cached_descriptor_state_;
  std::map<uint64_t, std::vector<NativeDescriptorRetirement>> native_descriptor_retirement_journal_;
  bool native_descriptor_layouts_update_after_bind_ = false;
  uint32_t native_descriptor_maximum_page_count_ = 0;
  NativeDrawStateCache<2> native_draw_state_cache_;
  NativeDescriptorSlotHandle native_null_image_descriptor_{};
  NativeDescriptorSlotHandle native_null_sampler_descriptor_{};
  uint32_t active_descriptor_copy_ = 0;
  NativeResolveDescriptorCache<VkDescriptorSet> resolve_descriptor_cache_;
  uint64_t resolve_recording_epoch_ = 0;
  uint64_t resolve_reuse_candidates_ = 0, resolve_reuse_hits_ = 0;
  uint64_t resolve_reuse_pixels_ = 0, resolve_shared_hdr_mirrors_ = 0;
  VkDescriptorPool frame_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorPool secondary_frame_descriptor_pool_ = VK_NULL_HANDLE;
  uint32_t frame_descriptor_draw_capacity_ = 0;
  uint32_t frame_descriptor_resolve_capacity_ = 0;
  uint32_t frame_descriptor_combined_set_capacity_ = 0;
  uint32_t secondary_frame_descriptor_draw_capacity_ = 0;
  uint32_t frame_descriptor_overflow_idle_frames_ = 0;
  uint32_t secondary_frame_descriptor_overflow_idle_frames_ = 0;
  uint32_t secondary_frame_descriptor_resolve_capacity_ = 0;
  uint32_t secondary_frame_descriptor_combined_set_capacity_ = 0;
  uint32_t frame_descriptor_draws_requested_ = 0;
  uint32_t frame_descriptor_unique_draws_ = 0;
  uint32_t frame_descriptor_sets_allocated_ = 0;
  uint32_t frame_descriptor_entries_written_ = 0;
  uint64_t frame_descriptor_pool_reset_count_ = 0;
  uint64_t frame_descriptor_pool_create_count_ = 0;
  uint64_t next_cached_descriptor_epoch_ = 1;
  uint64_t next_native_image_lifetime_ = 0;
  struct ReusableNativeImage {
    NativeImageResource resource{};
    VkComponentMapping view_components{};
    VkImageSubresourceRange view_range{};
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    std::vector<VkImageView> mip_views;
    VkImageView packed_stencil_view = VK_NULL_HANDLE;
    VkDeviceSize bytes=0;
    uint32_t memory_type=UINT32_MAX, memory_heap=UINT32_MAX;
  };
  NativeImageReusePool<ReusableNativeImage> texture_allocation_pool_{33554432, 64};
  std::unique_ptr<NativeResourceReclaimer<ReusableNativeImage>> texture_reclaimer_;
  bool image_allocation_pool_enabled_=true;
  uint64_t texture_allocation_reuses_=0;
  void TrimTextureAllocationPool(bool all);
  uint64_t command_pool_reset_count_ = 0;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  // Guest constant banks as dynamic uniform buffers (descriptor set 6).
  VkDescriptorSetLayout ubo_constants_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool ubo_constants_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 2> ubo_constants_sets_{};
  std::array<VkBuffer, 2> ubo_constants_set_buffers_{};
  // Render-worker constant ring: the worker byte-swaps each draw's guest
  // constants into a per-frame segment while it assembles the frame, so the
  // recorder (the frame's slowest stage) only binds their addresses. Six
  // segments: a segment is rewritten six frames later, when the recorder has
  // waited for the GPU to finish frames well past it (two frame slots).
  NativeUploadBuffer worker_constant_ring_{};
  // Per frame slot: the worker ring's banks (bindings 0/1) with that slot's
  // arena for the shared constants (binding 2).
  std::array<VkDescriptorSet, 2> worker_ring_ubo_sets_{};
  std::array<VkBuffer, 2> worker_ring_set_shared_buffers_{};
  std::mutex worker_ring_mutex_;  // worker writes vs. ring teardown
  std::atomic<bool> worker_ring_valid_{false};
  struct WorkerRingKey {
    uint64_t hash = 0;
    uint32_t bytes = 0;
    uint32_t kind = 0;
    bool operator==(const WorkerRingKey&) const = default;
  };
  struct WorkerRingKeyHash {
    size_t operator()(const WorkerRingKey& key) const noexcept {
      return size_t((key.hash ^ (uint64_t(key.bytes) << 40) ^ key.kind) * 0x9E3779B97F4A7C15ull);
    }
  };
  struct WorkerRingLast {
    const void* version = nullptr;
    uint64_t hash = 0;
    uint32_t bytes = 0;
    uint32_t offset = 0;
  };
  // Worker-thread state.
  uint32_t worker_ring_serial_ = 1;
  uint32_t worker_ring_segment_ = 0;
  VkDeviceSize worker_ring_cursor_ = 0;
  FrameGenerationMap<WorkerRingKey, uint32_t, WorkerRingKeyHash> worker_ring_dedupe_;
  std::array<WorkerRingLast, 2> worker_ring_last_{};
  // Recorder-thread: the ring frame of the frame being recorded.
  uint32_t recording_ring_serial_ = 0;
  // Whether a frame holds kTemporalUpdate commands (the worker marks the
  // frame it assembles), and whether PrepareTemporalFrame set any command's
  // temporal fields: without either it need not walk the frame's commands.
  bool assembly_has_temporal_updates_ = false;
  bool frame_has_temporal_updates_ = true;
  bool temporal_fields_set_ = true;
  void InitializeWorkerConstantRing();
  void DestroyWorkerConstantRing();
  void AdvanceWorkerConstantRing();
  void WriteWorkerRingConstants(NativeCommand& command, const NativeShader* vertex_shader,
                                const NativeShader* pixel_shader,
                                std::span<const uint8_t> vertex_canonical = {},
                                std::span<const uint8_t> pixel_canonical = {});
  VkPipelineLayout ubo_bound_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet ubo_bound_set_ = VK_NULL_HANDLE;
  VkCommandBuffer ubo_bound_command_buffer_ = VK_NULL_HANDLE;
  std::array<uint32_t, 3> ubo_bound_offsets_{};
  uint32_t ubo_rewritten_shaders_ = 0;
  uint64_t ubo_rewritten_loads_ = 0;
  bool BindUboConstantBanks(VkCommandBuffer command_buffer, VkPipelineLayout layout,
                            const NativeUploadAllocation& vertex,
                            const NativeUploadAllocation& pixel,
                            const NativeUploadAllocation& shared);
  VkPipelineCache native_pipeline_cache_ = VK_NULL_HANDLE;
  std::filesystem::path native_pipeline_cache_root_;
  std::filesystem::path native_pipeline_cache_path_;
  uint32_t native_pipeline_cache_title_id_ = 0;
  std::atomic<uint64_t> native_pipeline_cache_generation_{0};
  uint64_t native_pipeline_cache_saved_generation_ = 0;
  std::unique_ptr<NativePipelineCompilerState> native_pipeline_compiler_;
  uint64_t native_pipeline_unreported_compile_ticks_ = 0;
  uint64_t native_pipeline_unreported_wait_ticks_ = 0;
  uint64_t native_pipeline_unreported_creates_ = 0;
  uint32_t diagnostic_submitted_frame_ = 0;
  uint64_t diagnostic_draw_id_ = 0;
  size_t diagnostic_command_index_ = SIZE_MAX;
  RenderPhase diagnostic_render_phase_ = RenderPhase::kUnknown;
  uint32_t diagnostic_render_phase_object_ = 0;
  uint32_t diagnostic_light_trace_id_ = 0;
  uint64_t deterministic_trace_event_ = 0;
  bool deterministic_trace_active_ = false;
  std::vector<std::string> deferred_native_trace_lines_;
  static constexpr size_t kDeferredDiagnosticTaskCapacity = 8;
  std::mutex deferred_diagnostic_mutex_;
  std::condition_variable deferred_diagnostic_condition_;
  std::deque<std::function<void()>> deferred_diagnostic_tasks_;
  std::thread deferred_diagnostic_worker_;
  bool deferred_diagnostic_shutdown_ = false;
  bool deferred_diagnostic_task_running_ = false;
  std::atomic<uint64_t> deferred_diagnostic_drop_count_{0};
  VkDescriptorSetLayout resolve_conversion_descriptor_set_layout_ = VK_NULL_HANDLE;
  VkSampler packed_depth_stencil_sampler_ = VK_NULL_HANDLE;
  VkPipelineLayout resolve_conversion_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout texture_detile_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline texture_detile_pipeline_ = VK_NULL_HANDLE;
  // Set once the detile pipeline exists; read by texture capture threads.
  std::atomic<bool> texture_detile_available_{false};
  struct PendingDetileValidation {
    VkBuffer buffer = VK_NULL_HANDLE;
    const uint8_t* mapping = nullptr;
    std::shared_ptr<const NativeTextureResource> texture;
  };
  std::vector<PendingDetileValidation> pending_detile_validations_;
  // GPU breadcrumbs (gta4_native_gpu_breadcrumbs): the GPU writes the index of
  // each completed rendering scope into breadcrumb_buffer_ (one word per frame
  // slot); a frame the driver cut short (hang) leaves an index below its last.
  NativeUploadBuffer breadcrumb_buffer_;
  struct BreadcrumbFrame {
    uint32_t frame = 0;
    uint32_t final_value = 0;
    std::vector<std::string> labels;  // label i: the work after breadcrumb i
  };
  std::array<BreadcrumbFrame, 2> breadcrumb_frames_;
  void WriteBreadcrumb(VkCommandBuffer command_buffer, std::string label);
  void CheckBreadcrumbs();
  VkPipelineLayout cached_pipeline_layout_ = VK_NULL_HANDLE;
  std::unordered_map<NativePipelineKey, NativePipeline, NativePipelineKeyHash> native_pipelines_;
  NativePipelineLookupLifetime native_pipeline_lookup_lifetime_;
  // Recorder-owned memo across pipeline-state snapshots. A snapshot forks on
  // every texture or stream change, so its inline memo rarely sees a second
  // draw. This one is keyed by what GetOrCreatePipeline actually reads; shader
  // and declaration hashes guard against a freed object's address reuse.
  struct DrawPipelineCacheEntry {
    const NativeShader* vertex_shader = nullptr;
    const NativeShader* pixel_shader = nullptr;
    const NativeVertexDeclaration* vertex_declaration = nullptr;
    uint64_t vertex_shader_hash = 0;
    uint64_t pixel_shader_hash = 0;
    uint64_t vertex_declaration_hash = 0;
    std::array<uint32_t, kVertexStreamCount> strides{};
    NativeFixedFunctionState fixed{};
    NativePipelineLookupContext<kRenderTargetCount> context{};
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  std::vector<DrawPipelineCacheEntry> draw_pipeline_cache_;
  std::vector<NativeResolveConversionPipeline> resolve_conversion_pipelines_;
  VkPipeline hdr_present_pipeline_ = VK_NULL_HANDLE;
  VkPipeline sgsr_present_pipeline_ = VK_NULL_HANDLE; // SGSR: кэш пайплайна
  uint64_t legacy_mul_rewrites_ = 0;  // Shader registration thread only.
  std::array<NativeTextureImage, NativeFrameContextRing::kSlotCount> hdr_present_mirrors_{};
  std::vector<NativeSampler> native_samplers_;
  std::unordered_map<VkFormat, VkFormatProperties> native_format_properties_;
  FrameGenerationMap<uint64_t, NativeTextureImage*> prepared_texture_images_;
  std::unordered_map<NativeSamplerCacheKey, size_t, NativeSamplerCacheKeyHash>
      native_sampler_indices_;
  std::string active_texture_filtering_;
  bool texture_filtering_trace_pending_ = true;
  std::string active_anisotropic_filtering_;
  bool anisotropic_filtering_trace_pending_ = true;
  std::unordered_map<uint64_t, std::unique_ptr<NativeTextureImage>> native_texture_images_;
  std::map<uint64_t, std::vector<std::unique_ptr<NativeTextureImage>>> retired_texture_images_;
  size_t retired_texture_image_count_ = 0;
  std::unordered_set<uint64_t> protected_texture_generations_;
  std::unordered_set<uint64_t> packed_alias_generations_; // Live aliases, not scene history.
  std::map<NativeFlightResourceKey, NativeFlightResourceReference> staged_native_flight_resources_;
  std::array<std::optional<NativeFlightSubmission>, NativeFrameContextRing::kSlotCount>
      submitted_native_flight_resources_{};
  uint32_t active_texture_frame_ = 0;
  uint64_t next_texture_use_serial_ = 1;
  // Last realized draw target, reused by the next adjacent draw with identical
  // target descriptors (ResolveRenderingTargetMemo).
  struct NativeTargetMemo {
    bool valid = false;
    uint32_t frame = 0;
    uint32_t texture_frame = 0;
    uint64_t submission = 0;
    VkImageView presenter_view = VK_NULL_HANDLE;
    uint32_t presenter_width = 0;
    uint32_t presenter_height = 0;
    uint32_t color_attachment_mask = 0;
    VkImageAspectFlags depth_stencil_aspects = 0;
    std::array<SurfaceDescriptor, kRenderTargetCount> render_targets{};
    SurfaceDescriptor depth_stencil{};
    NativeRenderingTarget target{};
  };
  NativeTargetMemo native_target_memo_;
  std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> accounted_texture_bytes_{};
  NativeTextureEvictionIndex texture_eviction_scan_;
  size_t texture_eviction_remaining_ = 0;
  NativeTextureHeapBudgets texture_budget_snapshot_;
  uint64_t texture_heap_usage_ = 0;
  uint64_t texture_heap_budget_ = 0;
  NativePeriodicWorkSchedule texture_budget_poll_schedule_{kNativeTextureBudgetPollPhaseFrames};
  bool texture_budget_pressure_active_ = false;
  // Written by the title/producer thread while replacing the handle cache,
  // then drained by the render worker before image retirement. Keeping this
  // separate from pending_texture_release_generations_ avoids cross-thread
  // mutation of render-worker lifecycle state.
  std::unordered_set<uint64_t> superseded_texture_release_generations_;
  std::unordered_set<uint64_t> pending_texture_release_generations_;
  std::atomic<uint64_t> buffer_capture_reuse_count_{0};
  std::atomic<uint64_t> buffer_shadow_validation_count_{0};
  std::atomic<uint64_t> buffer_shadow_mismatch_count_{0};
  std::atomic<uint64_t> buffer_fast_path_disable_count_{0};
  std::atomic<uint64_t> buffer_fast_path_request_count_{0};
  std::atomic<bool> buffer_fast_path_disabled_{false};
  NativePeriodicWorkSchedule buffer_cache_poll_schedule_{kNativeBufferCachePollPhaseFrames};
  bool buffer_cache_reclamation_pending_ = false;
  uint64_t texture_image_eviction_count_ = 0;
  uint64_t texture_image_evicted_bytes_ = 0;
  uint64_t texture_allocation_retry_count_ = 0;
  uint64_t texture_allocation_failure_count_ = 0;
  std::vector<std::unique_ptr<NativeSurfaceImage>> native_surface_images_;
  std::unordered_map<uint32_t, std::vector<NativeSurfaceImage*>> surface_images_by_handle_;
  struct NativeTextureReadback {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t memory_type = UINT32_MAX;
    VkDeviceSize memory_size = 0;
    VkDeviceSize capacity = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
  } texture_readback_;
  std::unordered_set<uint64_t> pending_surface_release_ids_;
  uint64_t next_surface_lifetime_id_ = 1;
  std::unordered_map<GuestPlacementKey, NativePlacementOwner, GuestPlacementKeyHash>
      native_placement_owners_;
  uint64_t next_surface_write_serial_ = 1;
  uint64_t next_reflection_capture_epoch_ = 1;
  NativeReflectionRegistry reflection_resources_;
  uint32_t native_descriptor_capacity_ = 0;
  uint32_t native_sampler_descriptor_capacity_ = 0;
  bool null_images_initialized_ = false;
};

}  // namespace rex::graphics::gta4_native
