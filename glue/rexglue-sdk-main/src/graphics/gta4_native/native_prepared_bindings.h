#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rex::graphics::gta4_native {
// This is used only inside one indexed preparation batch. Original commands
// retain their resources; pages are not repurposed until their slot fence.
// Compare guest-null meaning as well as resource identity and all fetch words.
template <typename Command>
bool NativePreparedTextureInputsEqual(const Command& previous, const Command& next) {
  if (!previous.bindings_prepared || previous.failed_texture_mask || !previous.pipeline_state ||
      !next.pipeline_state || previous.used_texture_mask != next.used_texture_mask) return false;
  for (uint32_t mask = next.used_texture_mask; mask; mask &= mask - 1) {
    const uint32_t stage = uint32_t(std::countr_zero(mask));
    if (stage >= previous.textures.size()) break;
    if (previous.pipeline_state->textures[stage] != next.pipeline_state->textures[stage] ||
        previous.textures[stage] != next.textures[stage] ||
        std::memcmp(&previous.texture_fetches[stage], &next.texture_fetches[stage],
                    sizeof(previous.texture_fetches[stage])) != 0) return false;
  }
  return true;
}
template <typename Command>
void CopyNativePreparedTextureBindings(const Command& previous, Command& next) {
  next.descriptor_page = previous.descriptor_page;
  next.descriptor_copy = previous.descriptor_copy;
  next.image_descriptor_epoch = previous.image_descriptor_epoch;
  next.sampler_descriptor_epoch = previous.sampler_descriptor_epoch;
  next.cached_descriptor_epoch = previous.cached_descriptor_epoch;
  if (previous.descriptor_page != UINT32_MAX && !next.bindings_prepared &&
      !next.failed_texture_mask) [[likely]] {
    for (uint32_t mask = previous.used_texture_mask; mask; mask &= mask - 1) {
      const uint32_t stage = uint32_t(std::countr_zero(mask));
      if (stage >= next.texture_descriptor_indices.size()) break;
      next.texture_descriptor_indices[stage] = previous.texture_descriptor_indices[stage];
      next.sampler_descriptor_indices[stage] = previous.sampler_descriptor_indices[stage];
      next.binding_realization[stage] = previous.binding_realization[stage];
    }
  } else {
    next.texture_descriptor_indices = previous.texture_descriptor_indices;
    next.sampler_descriptor_indices = previous.sampler_descriptor_indices;
    next.binding_realization = previous.binding_realization;
    next.draw_descriptor_sets = previous.draw_descriptor_sets;
  }
  next.realized_image_mask = previous.realized_image_mask;
  next.realized_sampler_mask = previous.realized_sampler_mask;
  next.guest_null_texture_mask = previous.guest_null_texture_mask;
  next.failed_texture_mask = 0;
  next.bindings_prepared = true;
  // Never copy diagnostic-owned objects from another draw.
  if (next.extras && next.extras->room_light_input_bindings) [[unlikely]] {
    next.extras->room_light_input_bindings.reset();
  }
}
}  // namespace rex::graphics::gta4_native
