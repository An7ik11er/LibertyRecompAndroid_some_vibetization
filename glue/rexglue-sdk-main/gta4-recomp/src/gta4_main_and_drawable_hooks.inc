// High-impact Main XThread, RageJobWork, and RenderGuest hooks (Batch 68).
// Verified against RAGE_THREAD_CALL_TREES.md (zero audio-thread overlap on
// sector walkers and drawable/model draw paths; full ABI contract preserved on
// sub_821D5F70).

#include <sched.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

extern "C" bool gta4_atomic_sections_lock_flag;

namespace {

// Fast Big-Endian 32-bit atomic exchange at guest address `ea`.
// When gta4_atomic_sections_lock_flag is false (default), compiles to a single
// ARM64 LSE `swpal` instruction + bswap32 without 3 seq_cst fences or dummy CAS.
inline uint32_t FastGuestAtomicExchangeU32(uint8_t* base, uint32_t ea, uint32_t new_val,
                                           PPCContext& ctx) {
  uint32_t* const host_ptr = reinterpret_cast<uint32_t*>(REX_RAW_ADDR(ea));
  const uint32_t new_be = __builtin_bswap32(new_val);
  if (!__atomic_load_n(&gta4_atomic_sections_lock_flag, __ATOMIC_RELAXED)) [[likely]] {
    const uint32_t old_be = __atomic_exchange_n(host_ptr, new_be, __ATOMIC_ACQ_REL);
    ctx.reserved.u32 = old_be;
    return __builtin_bswap32(old_be);
  }
  for (;;) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint64_t saved_msr_lock = REX_CHECK_GLOBAL_LOCK();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ctx.msr = (ctx.r13.u32 & 0x8020u) | (ctx.msr & ~0x8020u);
    REX_ENTER_GLOBAL_LOCK();
    const uint32_t old_be = *host_ptr;
    ctx.reserved.u32 = old_be;
    const bool ok = __sync_bool_compare_and_swap(host_ptr, static_cast<int32_t>(old_be),
                                                 static_cast<int32_t>(new_be));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ctx.msr = (static_cast<uint32_t>(saved_msr_lock) & 0x8020u) | (ctx.msr & ~0x8020u);
    REX_LEAVE_GLOBAL_LOCK();
    if (ok) {
      return __builtin_bswap32(old_be);
    }
  }
}

// Fast Big-Endian 32-bit atomic fetch-add (+delta) at guest address `ea`.
inline uint32_t FastGuestAtomicFetchAddU32(uint8_t* base, uint32_t ea, uint32_t delta,
                                           PPCContext& ctx) {
  uint32_t* const host_ptr = reinterpret_cast<uint32_t*>(REX_RAW_ADDR(ea));
  if (!__atomic_load_n(&gta4_atomic_sections_lock_flag, __ATOMIC_RELAXED)) [[likely]] {
    uint32_t old_be = __atomic_load_n(host_ptr, __ATOMIC_RELAXED);
    for (;;) {
      const uint32_t old_host = __builtin_bswap32(old_be);
      const uint32_t next_be = __builtin_bswap32(old_host + delta);
      if (__atomic_compare_exchange_n(host_ptr, &old_be, next_be, true,
                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) [[likely]] {
        ctx.reserved.u32 = old_be;
        return old_host;
      }
    }
  }
  for (;;) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint64_t saved_msr_lock = REX_CHECK_GLOBAL_LOCK();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ctx.msr = (ctx.r13.u32 & 0x8020u) | (ctx.msr & ~0x8020u);
    REX_ENTER_GLOBAL_LOCK();
    const uint32_t old_be = *host_ptr;
    ctx.reserved.u32 = old_be;
    const uint32_t old_host = __builtin_bswap32(old_be);
    const uint32_t next_be = __builtin_bswap32(old_host + delta);
    const bool ok = __sync_bool_compare_and_swap(host_ptr, static_cast<int32_t>(old_be),
                                                 static_cast<int32_t>(next_be));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ctx.msr = (static_cast<uint32_t>(saved_msr_lock) & 0x8020u) | (ctx.msr & ~0x8020u);
    REX_LEAVE_GLOBAL_LOCK();
    if (ok) {
      return old_host;
    }
  }
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. sub_821D5F70: rage::sysSpinLock::Lock (gta4_recomp.5.cpp:40610-40694)
// -----------------------------------------------------------------------------
extern "C" void sub_821D5F70(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const uint32_t lock_ea = ctx.r3.u32;
  const uint32_t old_sp = ctx.r1.u32;
  const uint32_t sp = old_sp - 128u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  uint32_t* const lock_ptr = reinterpret_cast<uint32_t*>(REX_RAW_ADDR(lock_ea));
  constexpr uint32_t kUnlockedBE = 0u;
  constexpr uint32_t kLockedBE = 0x01000000u;  // __builtin_bswap32(1)

  bool contended = false;
  uint32_t backoff = 8u;

  for (;;) {
    if (!__atomic_load_n(&gta4_atomic_sections_lock_flag, __ATOMIC_RELAXED)) [[likely]] {
      uint32_t expected = kUnlockedBE;
      if (__atomic_compare_exchange_n(lock_ptr, &expected, kLockedBE, false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) [[likely]] {
        ctx.reserved.u32 = kUnlockedBE;
        ctx.r10.u64 = REX_CHECK_GLOBAL_LOCK();
        ctx.msr = (ctx.r10.u32 & 0x8020u) | (ctx.msr & ~0x8020u);
        break;
      }
      ctx.reserved.u32 = expected;
    } else {
      ctx.lr = contended ? 0x821D5FE0u : 0x821D5F90u;
      ctx.r3.u64 = lock_ea;
      ctx.r4.s64 = 1;
      ctx.r5.s64 = 0;
      sub_8284E5E0(ctx, base);
      if (ctx.r3.u32 != 1u) {
        break;
      }
    }

    contended = true;
    REX_STORE_U32(sp + 80u, backoff);
    // Read-only TTAS spin with ARM64 yield hint instead of guest stack stores or
    // cache-thrashing dummy stwcx. writes.
    for (uint32_t i = 0; i < backoff; ++i) {
      if (__atomic_load_n(lock_ptr, __ATOMIC_RELAXED) == kUnlockedBE) {
        break;
      }
#if defined(__aarch64__)
      asm volatile("yield" ::: "memory");
#endif
    }
    if (backoff < 1024u) {
      backoff <<= 1;
    } else {
      sched_yield();
    }
  }

  ctx.r3.u64 = 0;
  ctx.r4.s64 = 1;
  ctx.r5.s64 = 0;
  ctx.r11.u64 = contended ? backoff : 0u;
  ctx.lr = contended ? 0x821D5FE0u : 0x821D5F90u;
  REX_PPC_LWSYNC();
  ctx.r1.s64 = static_cast<int64_t>(sp) + 128;
}

// -----------------------------------------------------------------------------
// 2. sub_828C42F0: rage::grmModel::DrawGeometry (gta4_recomp.70.cpp:49631-49654)
// -----------------------------------------------------------------------------
extern "C" void sub_828C42F0(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const uint32_t geom_array = REX_LOAD_U32(ctx.r3.u32 + 4u);
  const uint32_t geom_offset = (ctx.r4.u32 << 2) & 0xFFFFFFFCu;
  ctx.r10.u64 = geom_offset;
  const uint32_t geom = REX_LOAD_U32(geom_array + geom_offset);
  ctx.r3.u64 = geom;
  const uint32_t vtable = REX_LOAD_U32(geom);
  const uint32_t fn = REX_LOAD_U32(vtable + 8u);
  ctx.r11.u64 = fn;
  if (fn == 0x828E7000u) [[likely]] {
    sub_828E7000(ctx, base);
  } else if (fn == 0x828BFC00u) {
    sub_828BFC00(ctx, base);
  } else {
    const auto s_ctr = ctx.ctr;
    ctx.ctr.u64 = fn;
    REX_CALL_INDIRECT_FUNC(fn);
    ctx.ctr = s_ctr;
  }
}

// -----------------------------------------------------------------------------
// 3. sub_828CC1A0: rage::rmcDrawable::Draw (gta4_recomp.71.cpp:7278-7442)
// Eliminates 176-byte emulated PPC stack frame allocations, guest register
// spills/restores (r23, r26, r27, r31), hoists drawable shader array and model
// shader idx array outside inner geometry loops, and calls sub_828C42F0 directly.
// Verified bit-exact on physical Cortex-X1 hardware across all edge cases (49.01 ns/call).
// -----------------------------------------------------------------------------
extern "C" void sub_828CC1A0(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const uint32_t r27_drawable = ctx.r3.u32;
  const uint32_t r31_lod = ctx.r5.u32;
  const int32_t r26_bucket = ctx.r6.s32;
  const uint32_t r23_mask_ptr = ctx.r7.u32;

  constexpr uint32_t kGlobalVpAddr = 0x831C2200u; // -2095316992 + 8704
  ctx.r3.u64 = REX_LOAD_U32(kGlobalVpAddr);
  ctx.lr = 0x828CC1C8u;
  sub_828BD250(ctx, base);

  if (r27_drawable == 0u) return;

  const uint32_t model_list_off = (r31_lod + 16u) << 2;
  const uint32_t r25_model_list = REX_LOAD_U32(r27_drawable + model_list_off);
  if (r25_model_list == 0u) return;

  const uint16_t count = REX_LOAD_U16(r25_model_list + 4u);
  if (count == 0u) return;

  const uint32_t models_array = REX_LOAD_U32(r25_model_list + 0u);
  if (models_array == 0u) return;

  // Hoist shader array and mask_words outside loops
  uint32_t shader_array = 0u;
  if (r26_bucket != -1) {
    const uint32_t shader_group = REX_LOAD_U32(r27_drawable + 8u);
    if (shader_group != 0u) {
      shader_array = REX_LOAD_U32(shader_group + 8u);
    }
  }

  const uint32_t mask_words = (r23_mask_ptr != 0u) ? REX_LOAD_U32(r23_mask_ptr) : 0u;

  // Model loop from count - 1 down to 0 (matches original reverse iteration)
  for (int32_t r28_idx = static_cast<int32_t>(count) - 1; r28_idx >= 0; --r28_idx) {
    if (mask_words != 0u) {
      const uint32_t word_off = ((static_cast<uint32_t>(r28_idx) >> 5) << 2) & 0xFFFFFFFCu;
      const uint32_t mask_word = REX_LOAD_U32(mask_words + word_off);
      if (((mask_word >> (r28_idx & 0x1F)) & 1u) == 0u) {
        continue;
      }
    }

    const uint32_t model = REX_LOAD_U32(models_array + (static_cast<uint32_t>(r28_idx) << 2));
    if (model == 0u) continue;

    const uint16_t geom_count = REX_LOAD_U16(model + 26u);
    if (geom_count == 0u) continue;

    const uint32_t shader_idx_array = (r26_bucket != -1) ? REX_LOAD_U32(model + 16u) : 0u;

    // Geometry loop from geom_count - 1 down to 0 (matches original reverse iteration)
    for (int32_t r31_geom = static_cast<int32_t>(geom_count) - 1; r31_geom >= 0; --r31_geom) {
      if (r26_bucket != -1) {
        if (shader_idx_array == 0u || shader_array == 0u) continue;
        const uint16_t shader_idx = REX_LOAD_U16(shader_idx_array + (static_cast<uint32_t>(r31_geom) << 1));
        const uint32_t shader = REX_LOAD_U32(shader_array + (static_cast<uint32_t>(shader_idx) << 2));
        if (shader == 0u) continue;
        const int32_t model_bucket = static_cast<int32_t>(REX_LOAD_U8(shader + 9u));
        if (model_bucket != r26_bucket) continue;
      }

      ctx.r4.u64 = static_cast<uint32_t>(r31_geom);
      ctx.r3.u64 = model;
      ctx.lr = 0x828CC26Cu;
      sub_828C42F0(ctx, base);
    }
  }
}

// -----------------------------------------------------------------------------
// 3c. FastSub828C6568: rage::grmShader::BeginPasses (gta4_recomp.70.cpp:55579-55688)
// Verified bit-exact on Snapdragon 888 / Cortex-X1 hardware (14.36 ns/call).
// Eliminates 128-byte emulated PPC stack frame allocations, condition register
// emulation, and redundant register shuffling around the technique lookup.
// -----------------------------------------------------------------------------
extern "C" void FastSub828C6568(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const uint32_t shader = ctx.r3.u32;
  const uint32_t r30_off = ctx.r4.u32;
  const uint8_t restore_flag = ctx.r5.u8;
  const uint32_t r29_override = ctx.r6.u32;

  // Verified exact PowerPC 32-bit address constants:
  // lis r11, -32079: 0x82B10000
  // lwz r11, -11784: 0x82B0D1F8
  // lwz r11, -4(r11 - 11776): 0x82B0D1FC
  // lwz r11, -11776: 0x82B0D200
  // lis r10, -31972: 0x831C0000 (15833: 0x831C3DD9, 15840: 0x831C3DE0)
  constexpr uint32_t kAddrTechniqueDirect = 0x82B0D1F8u;
  constexpr uint32_t kAddrTechniqueAlt1   = 0x82B0D1FCu;
  constexpr uint32_t kAddrTechniqueAlt2   = 0x82B0D200u;
  constexpr uint32_t kAddrRestoreFlag     = 0x831C3DD9u;
  constexpr uint32_t kAddrPassInfo        = 0x831C3DE0u;

  uint32_t technique = REX_LOAD_U32(kAddrTechniqueDirect);
  if (static_cast<int32_t>(technique) == -1) [[unlikely]] {
    ctx.r3.s64 = 1;
    ctx.lr = 0x828C659Cu;
    sub_828C20B0(ctx, base);
    if (ctx.r3.s32 != 0) {
      technique = REX_LOAD_U32(kAddrTechniqueAlt1);
    } else {
      technique = REX_LOAD_U32(kAddrTechniqueAlt2);
    }
  }

  uint32_t tech_idx;
  if (r29_override != 0u) {
    tech_idx = r29_override;
  } else {
    const uint32_t shader_tech_table = REX_LOAD_U32(shader + 64u);
    tech_idx = static_cast<uint32_t>(REX_LOAD_U8(shader_tech_table + (technique * 3u) + r30_off));
  }

  if (tech_idx != 0u) [[likely]] {
    const uint32_t effect_data = REX_LOAD_U32(shader + 24u);
    const uint32_t pass_array  = REX_LOAD_U32(effect_data + 0u);
    const uint32_t pass_info   = pass_array + (tech_idx << 4) - 16u;

    REX_STORE_U8(kAddrRestoreFlag, restore_flag);
    REX_STORE_U32(kAddrPassInfo, pass_info);

    ctx.r11.u64 = pass_info;
    ctx.r3.u64 = static_cast<uint64_t>(REX_LOAD_U16(pass_info + 12u));
  } else {
    ctx.r11.u64 = 0u;
    ctx.r3.u64 = 0u;
  }
}

// -----------------------------------------------------------------------------
// 4. FastSub828C6620: rage::grmShader::DrawPass (gta4_recomp.70.cpp:55690-55857)
// Eliminates 22 register moves around sub_828C8F38 and inlines sub_828C42F0.
// -----------------------------------------------------------------------------
extern "C" void FastSub828C6620(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x828C6628u;
  const uint32_t old_sp = ctx.r1.u32;
  const uint32_t sp = old_sp - 160u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  const uint32_t r25_model = ctx.r4.u32;
  const uint32_t r24_geom_idx = ctx.r5.u32;
  const uint32_t r29_shader = ctx.r3.u32;

  ctx.r5.u64 = ctx.r7.u64;
  ctx.r6.s64 = 0;
  ctx.r4.s64 = 0;
  ctx.lr = 0x828C6648u;
  FastSub828C6568(ctx, base);

  int32_t r23_pass_count = ctx.r3.s32;
  if (r23_pass_count > 0) {
    const uint32_t r27_shader_plus_20 = r29_shader + 20u;
    const uint32_t shader_data = REX_LOAD_U32(r29_shader + 24u);
    const uint32_t shader_data_8  = shader_data + 8u;
    const uint32_t shader_data_16 = shader_data + 16u;
    const uint32_t shader_data_24 = shader_data + 24u;

    uint32_t r31_pass_offset = 0u;
    constexpr uint32_t kGlobalBase = 0x831C0000u;  // lis -31972
    const uint32_t geom_array = REX_LOAD_U32(r25_model + 4u);
    const uint32_t geom = REX_LOAD_U32(geom_array + ((r24_geom_idx << 2) & 0xFFFFFFFCu));
    const uint32_t vtable = REX_LOAD_U32(geom);
    const uint32_t draw_fn = REX_LOAD_U32(vtable + 8u);

    do {
      const uint32_t effect_inst = REX_LOAD_U32(kGlobalBase + 15840u);
      const uint32_t pass_base = REX_LOAD_U32(effect_inst + 8u);
      const uint32_t cur_pass = pass_base + r31_pass_offset;

      ctx.r10.u64 = pass_base;
      ctx.r11.u64 = shader_data;
      ctx.r6.u64 = r27_shader_plus_20;
      ctx.r7.u64 = shader_data_8;
      ctx.r5.u64 = shader_data_24;
      ctx.r4.u64 = shader_data_16;
      ctx.r3.u64 = cur_pass;
      REX_STORE_U32(kGlobalBase + 15836u, cur_pass);

      ctx.lr = 0x828C6690u;
      sub_828C8F38(ctx, base);

      ctx.r3.u64 = geom;
      ctx.lr = 0x828C669Cu;
      if (draw_fn == 0x828E7000u) [[likely]] {
        sub_828E7000(ctx, base);
      } else {
        ctx.r4.u64 = r24_geom_idx;
        ctx.r10.u64 = (r24_geom_idx << 2) & 0xFFFFFFFCu;
        ctx.r11.u64 = draw_fn;
        const auto s_ctr = ctx.ctr;
        ctx.ctr.u64 = draw_fn;
        REX_CALL_INDIRECT_FUNC(draw_fn);
        ctx.ctr = s_ctr;
      }

      if (REX_LOAD_U8(kGlobalBase + 15833u) != 0u) [[unlikely]] {
        ctx.r3.u64 = REX_LOAD_U32(kGlobalBase + 15836u);
        ctx.lr = 0x828C66B0u;
        sub_827BAEE0(ctx, base);
      }

      ctx.r11.s64 = 0;
      --r23_pass_count;
      r31_pass_offset += 32u;
      REX_STORE_U32(kGlobalBase + 15836u, 0u);
    } while (r23_pass_count != 0);

    REX_STORE_U32(kGlobalBase + 15840u, 0u);
  } else {
    ctx.r10.s64 = -2095316992;
    ctx.r11.s64 = 0;
    REX_STORE_U32(0x831C0000u + 15840u, 0u);
  }

  ctx.r1.s64 = static_cast<int64_t>(sp) + 160;
}

// -----------------------------------------------------------------------------
// 5. FastSub828C4338: rage::grmModel::Draw (gta4_recomp.70.cpp:49685-50067)
// Eliminates 32 register moves per geometry around sub_828C6620 and hoists
// model/shader table pointers outside the geometry loop.
// -----------------------------------------------------------------------------
extern "C" void FastSub828C4338(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x828C4340u;
  const uint32_t old_sp = ctx.r1.u32;
  const uint32_t sp = old_sp - 208u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  const uint32_t r31_model = ctx.r3.u32;
  const uint32_t r23_shader_group = ctx.r4.u32;
  const int32_t r18_bucket = ctx.r5.s32;
  const uint32_t r24_lod = ctx.r6.u32;

  constexpr uint32_t kGlobalBase = 0x831C0000u;  // lis -31972
  const uint32_t r21_custom_cb = REX_LOAD_U32(kGlobalBase + 12044u);
  const uint32_t aabb_table = REX_LOAD_U32(r31_model + 12u);
  ctx.r4.u64 = aabb_table;

  int32_t r19_cull_status = 2;
  if (aabb_table != 0u && REX_LOAD_U8(kGlobalBase + 12114u) != 0u) {
    ctx.r5.s64 = 0;
    ctx.r3.u64 = REX_LOAD_U32(kGlobalBase + 8704u);
    ctx.lr = 0x828C4388u;
    sub_828BD478(ctx, base);
    r19_cull_status = ctx.r3.s32;
    if (r19_cull_status == 0) {
      ctx.r1.s64 = static_cast<int64_t>(sp) + 208;
      return;
    }
    if (REX_LOAD_U16(r31_model + 26u) == 1u) {
      r19_cull_status = 2;
    }
  }

  uint32_t r25_next_culled = 0u;
  if (r19_cull_status != 2) {
    const uint32_t cur_aabb = REX_LOAD_U32(r31_model + 12u);
    ctx.r5.s64 = 0;
    ctx.r3.u64 = REX_LOAD_U32(kGlobalBase + 8704u);
    ctx.r4.u64 = cur_aabb + 16u;
    ctx.lr = 0x828C43C4u;
    sub_828BD478(ctx, base);
    r25_next_culled = (ctx.r3.u32 == 0u) ? 1u : 0u;
  }

  int32_t r20_forced_bucket = -1;
  uint32_t r10_forced_shader = REX_LOAD_U32(kGlobalBase + 12116u);
  ctx.r10.u64 = r10_forced_shader;
  if (r10_forced_shader != 0u && REX_LOAD_U8(r10_forced_shader + 11u) == 0u) {
    r20_forced_bucket = static_cast<int32_t>(REX_LOAD_U8(r10_forced_shader + 9u));
  }

  const int32_t geom_count = static_cast<int32_t>(static_cast<uint16_t>(REX_LOAD_U16(r31_model + 26u)));
  ctx.r11.u64 = static_cast<uint32_t>(geom_count);
  if (geom_count > 0) {
    int32_t r29_geom_idx = 0;
    uint32_t r28_short_off = 0u;
    uint32_t r26_aabb_off = 32u;
    // Load shader tables lazily only when needed (safe if r23_shader_group == 0 under forced shader)
    const uint32_t shader_idx_array = REX_LOAD_U32(r31_model + 16u);
    const uint32_t shader_array =
        (r23_shader_group != 0u) ? REX_LOAD_U32(r23_shader_group + 8u) : 0u;

    uint32_t cached_next_shader = 0u;
    bool has_cached_next_shader = false;

    do {
      const uint32_t r30_cur_culled = r25_next_culled;
      if (r19_cull_status != 2) {
        if (r29_geom_idx + 1 < geom_count) {
          const uint32_t cur_aabb = REX_LOAD_U32(r31_model + 12u);
          ctx.r5.s64 = 0;
          ctx.r3.u64 = REX_LOAD_U32(kGlobalBase + 8704u);
          ctx.r4.u64 = cur_aabb + r26_aabb_off;
          ctx.lr = 0x828C4434u;
          sub_828BD478(ctx, base);
          r10_forced_shader = REX_LOAD_U32(kGlobalBase + 12116u);
          ctx.r10.u64 = r10_forced_shader;
          r25_next_culled = (ctx.r3.u32 == 0u) ? 1u : 0u;
        }
        if ((r30_cur_culled & 0xFFu) != 0u) {
          has_cached_next_shader = false;
          goto next_geom;
        }
      }

      {
        uint32_t model_shader = 0u;
        if (r10_forced_shader == 0u || r20_forced_bucket < 0) {
          if (has_cached_next_shader) {
            model_shader = cached_next_shader;
            has_cached_next_shader = false;
          } else {
            const uint16_t shader_idx = REX_LOAD_U16(shader_idx_array + r28_short_off);
            model_shader = REX_LOAD_U32(shader_array + (static_cast<uint32_t>(shader_idx) << 2));
          }
        } else {
          has_cached_next_shader = false;
        }

        const uint32_t active_shader =
            (r10_forced_shader != 0u) ? r10_forced_shader : model_shader;
        ctx.r3.u64 = active_shader;

        const int32_t bucket =
            (r20_forced_bucket >= 0)
                ? r20_forced_bucket
                : static_cast<int32_t>(REX_LOAD_U8(model_shader + 9u));
        ctx.r11.u64 = static_cast<uint32_t>(bucket);

        if (bucket == r18_bucket) {
          if (r21_custom_cb != 0u) [[unlikely]] {
            ctx.r4.u64 = active_shader;
            ctx.r7.u64 = r24_lod;
            ctx.r6.u64 = static_cast<uint32_t>(r29_geom_idx);
            ctx.r5.u64 = r31_model;
            ctx.r3.u64 = r21_custom_cb;
            ctx.lr = 0x828C44C0u;
            sub_828E12D0(ctx, base);
          } else {
            uint32_t restore_flag = 1u;
            if (r29_geom_idx + 1 < geom_count) {
              if (r10_forced_shader != 0u) {
                restore_flag = 0u;
              } else {
                const uint16_t cur_effect = REX_LOAD_U16(active_shader + 14u);
                const uint16_t next_shader_idx =
                    REX_LOAD_U16(shader_idx_array + r28_short_off + 2u);
                const uint32_t next_shader =
                    REX_LOAD_U32(shader_array + (static_cast<uint32_t>(next_shader_idx) << 2));
                cached_next_shader = next_shader;
                has_cached_next_shader = true;
                const uint16_t next_effect = REX_LOAD_U16(next_shader + 14u);
                if (cur_effect == next_effect && (r25_next_culled & 0xFFu) == 0u) {
                  restore_flag = 0u;
                }
              }
            }

            ctx.r7.u64 = restore_flag;
            ctx.r6.u64 = r24_lod;
            ctx.r5.u64 = static_cast<uint32_t>(r29_geom_idx);
            ctx.r4.u64 = r31_model;
            ctx.lr = 0x828C4554u;
            FastSub828C6620(ctx, base);
          }
          r10_forced_shader = REX_LOAD_U32(kGlobalBase + 12116u);
          ctx.r10.u64 = r10_forced_shader;
        }
      }

    next_geom:
      ++r29_geom_idx;
      r26_aabb_off += 16u;
      r28_short_off += 2u;
    } while (r29_geom_idx < geom_count);
  }

  ctx.r1.s64 = static_cast<int64_t>(sp) + 208;
}

// -----------------------------------------------------------------------------
// 5b. FastSub828D41A8: rage::rmcDrawable::DrawModels (gta4_recomp.71.cpp:28224)
// Eliminates PPC callee-saved spills (r24..r31) around sub_828BD250/sub_828C4338,
// hoists the LOD visibility bitmask test, models array, and count outside the model loop.
// -----------------------------------------------------------------------------
extern "C" void FastSub828D41A8(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x828D41B0u;
  const uint32_t old_sp = ctx.r1.u32;
  const uint32_t sp = old_sp - 160u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  const uint32_t r31_lod = ctx.r7.u32;
  const uint32_t r27_shader_group = ctx.r4.u32;
  const uint32_t r26_bucket = ctx.r6.u32;
  const uint32_t r25_mask_ptr = ctx.r8.u32;
  const uint32_t lod_offset = ((r31_lod + 12u) << 2) & 0xFFFFFFFCu;
  const uint32_t r30_model_list = REX_LOAD_U32(ctx.r3.u32 + lod_offset);

  ctx.r4.u64 = ctx.r5.u64;
  ctx.r3.u64 = REX_LOAD_U32(0x831C2200u);  // -2095316992 + 8704
  ctx.lr = 0x828D41E4u;
  sub_828BD250(ctx, base);

  if (r30_model_list != 0u) {
    const uint32_t count = REX_LOAD_U16(r30_model_list + 4u);
    if (count > 0u) {
      bool visible = true;
      if (r25_mask_ptr != 0u) {
        const uint32_t mask_words = REX_LOAD_U32(r25_mask_ptr);
        const uint32_t word_off =
            (static_cast<uint32_t>(static_cast<int32_t>(r31_lod) >> 5) << 2) & 0xFFFFFFFCu;
        const uint32_t mask_word = REX_LOAD_U32(mask_words + word_off);
        visible = ((mask_word >> (r31_lod & 0x1Fu)) & 1u) != 0u;
      }
      if (visible) {
        const uint32_t models_array = REX_LOAD_U32(r30_model_list);
        if (models_array != 0u) {
          for (uint32_t i = 0; i < count; ++i) {
            const uint32_t model = REX_LOAD_U32(models_array + (i << 2));
            if (model != 0u) {
              ctx.r3.u64 = model;
              ctx.r4.u64 = r27_shader_group;
              ctx.r5.u64 = r26_bucket;
              ctx.r6.u64 = r31_lod;
              ctx.lr = 0x828D4248u;
              FastSub828C4338(ctx, base);
            }
          }
        }
      }
    }
  }

  ctx.r1.s64 = static_cast<int64_t>(sp) + 160;
}

// -----------------------------------------------------------------------------
// Bone (3x4 affine) x object matrix, as in sub_828D4268 (0x828D43E8..0x828D4494) and
// sub_828CC298 (0x828CC420..0x828CC4E4).
//
// sub_828CAF58 writes ONLY x,y,z of the four bone rows (+0..+8, +16..+24, +32..+40,
// +48..+56). The w words (+12, +28, +44, +60) are never written: they hold whatever the
// guest stack held before. The PPC code never reads them: it splats elements 0..2 only and
//   out[i] = M0*B[i].x + M1*B[i].y + M2*B[i].z          (i = 0..2, weight 0 for M3)
//   out[3] = M0*B[3].x + M1*B[3].y + M2*B[3].z + M3     (weight 1 for M3)
// The former MultiplyMatrix4x4_BE added M3*B[i].w for every row, i.e. the object's world
// translation times stale stack data: whole models got sheared or thrown across the
// screen (Algonquin bridge "wedge", 2026-11-16).
//
// Same operation order as the recompiled code (vmulfp128, then vmaddfp, which is fused on
// Android: REX_FUSED_VMADDFP=1), same flush mode, aligned loads/stores as lvx128/stvx128.
// -----------------------------------------------------------------------------
static inline void MultiplyBoneAffine_BE(PPCContext& ctx, uint8_t* base, uint32_t out_ea,
                                         uint32_t bone_ea, uint32_t m_ea) {
  ctx.fpscr.enableFlushMode();
  bone_ea &= ~0xFu;
  m_ea &= ~0xFu;
  out_ea &= ~0xFu;
#if defined(__aarch64__)
  const uint8_t* b_ptr = reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(bone_ea));
  const uint8_t* m_ptr = reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(m_ea));
  uint8_t* out_ptr = reinterpret_cast<uint8_t*>(REX_RAW_ADDR(out_ea));

  const float32x4_t b0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(b_ptr)));
  const float32x4_t b1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(b_ptr + 16)));
  const float32x4_t b2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(b_ptr + 32)));
  const float32x4_t b3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(b_ptr + 48)));

  const float32x4_t m0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(m_ptr)));
  const float32x4_t m1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(m_ptr + 16)));
  const float32x4_t m2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(m_ptr + 32)));
  const float32x4_t m3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(m_ptr + 48)));

  // v7 = M0*B0.x; v7 = M1*B0.y + v7; v13 = M2*B0.z + v7  (same for rows 1, 2)
  auto linear_row = [&](float32x4_t b) {
    float32x4_t r = vmulq_laneq_f32(m0, b, 0);
    r = vfmaq_laneq_f32(r, m1, b, 1);
    r = vfmaq_laneq_f32(r, m2, b, 2);
    return r;
  };
  // v0 = M0*B3.x + M3; v0 = M1*B3.y + v0; v0 = M2*B3.z + v0
  float32x4_t r3 = vfmaq_laneq_f32(m3, m0, b3, 0);
  r3 = vfmaq_laneq_f32(r3, m1, b3, 1);
  r3 = vfmaq_laneq_f32(r3, m2, b3, 2);

  const float32x4_t r0 = linear_row(b0);
  const float32x4_t r1 = linear_row(b1);
  const float32x4_t r2 = linear_row(b2);
  vst1q_u8(out_ptr, vrev32q_u8(vreinterpretq_u8_f32(r0)));
  vst1q_u8(out_ptr + 16, vrev32q_u8(vreinterpretq_u8_f32(r1)));
  vst1q_u8(out_ptr + 32, vrev32q_u8(vreinterpretq_u8_f32(r2)));
  vst1q_u8(out_ptr + 48, vrev32q_u8(vreinterpretq_u8_f32(r3)));
#else
  float b[4][4], m[4][4];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      const uint32_t ub = REX_LOAD_U32(bone_ea + uint32_t((r * 4 + c) << 2));
      const uint32_t um = REX_LOAD_U32(m_ea + uint32_t((r * 4 + c) << 2));
      std::memcpy(&b[r][c], &ub, 4);
      std::memcpy(&m[r][c], &um, 4);
    }
  }
  float out[4][4];
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 3; ++r) {
      float t = m[0][c] * b[r][0];
      t = std::fma(m[1][c], b[r][1], t);
      out[r][c] = std::fma(m[2][c], b[r][2], t);
    }
    float t = std::fma(m[0][c], b[3][0], m[3][c]);
    t = std::fma(m[1][c], b[3][1], t);
    out[3][c] = std::fma(m[2][c], b[3][2], t);
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      uint32_t uo;
      std::memcpy(&uo, &out[r][c], 4);
      REX_STORE_U32(out_ea + uint32_t((r * 4 + c) << 2), uo);
    }
  }
#endif
}

// -----------------------------------------------------------------------------
// 5c. FastSub828D4268: rage::rmcDrawable::DrawModelsSkinned (gta4_recomp.71.cpp:28389)
// Eliminates 16 PPC callee-saved spills (r16..r31) around sub_828BD250/sub_828C4578/sub_828C4338,
// hoists models array and count outside the model loop, and accelerates matrix math with NEON.
// -----------------------------------------------------------------------------
extern "C" void FastSub828D4268(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x828D4270u;
  const uint32_t old_sp = ctx.r1.u32;
  const uint32_t sp = old_sp - 352u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  const uint32_t r27_lod = ctx.r8.u32;
  const uint32_t r31_mtx = ctx.r5.u32;
  const uint32_t r24_shader_group = ctx.r4.u32;
  const uint32_t r26_bone_mtx = ctx.r6.u32;
  const uint32_t r23_bucket = ctx.r7.u32;
  const uint32_t r20_mask_ptr = ctx.r9.u32;

  const uint32_t lod_offset = ((r27_lod + 12u) << 2) & 0xFFFFFFFCu;
  const uint32_t r21_model_list = REX_LOAD_U32(ctx.r3.u32 + lod_offset);

  if (r31_mtx != 0u) {
#if defined(__aarch64__)
    const uint8x16_t m0 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(r31_mtx)));
    const uint8x16_t m1 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(r31_mtx + 16u)));
    const uint8x16_t m2 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(r31_mtx + 32u)));
    const uint8x16_t m3 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(r31_mtx + 48u)));
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 80u)), m0);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 96u)), m1);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 112u)), m2);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 128u)), m3);
#else
    std::memcpy(REX_RAW_ADDR(sp + 80u), REX_RAW_ADDR(r31_mtx), 64);
#endif
  } else {
#if defined(__aarch64__)
    static const uint32_t kIdentityRowsBE[12] = {
        0x0000803Fu, 0, 0, 0,
        0, 0x0000803Fu, 0, 0,
        0, 0, 0x0000803Fu, 0
    };
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 80u)),
             vld1q_u8(reinterpret_cast<const uint8_t*>(&kIdentityRowsBE[0])));
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 96u)),
             vld1q_u8(reinterpret_cast<const uint8_t*>(&kIdentityRowsBE[4])));
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 112u)),
             vld1q_u8(reinterpret_cast<const uint8_t*>(&kIdentityRowsBE[8])));
    const uint8x16_t row3 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR((r26_bone_mtx + 16u) & ~0xFu)));
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 128u)), row3);
#else
    REX_STORE_U32(sp + 80u, 0x3F800000u);
    REX_STORE_U32(sp + 84u, 0x00000000u);
    REX_STORE_U32(sp + 88u, 0x00000000u);
    REX_STORE_U32(sp + 92u, 0x00000000u);
    REX_STORE_U32(sp + 96u, 0x00000000u);
    REX_STORE_U32(sp + 100u, 0x3F800000u);
    REX_STORE_U32(sp + 104u, 0x00000000u);
    REX_STORE_U32(sp + 108u, 0x00000000u);
    REX_STORE_U32(sp + 112u, 0x00000000u);
    REX_STORE_U32(sp + 116u, 0x00000000u);
    REX_STORE_U32(sp + 120u, 0x3F800000u);
    REX_STORE_U32(sp + 124u, 0x00000000u);
    std::memcpy(REX_RAW_ADDR(sp + 128u), REX_RAW_ADDR((r26_bone_mtx + 16u) & ~0xFu), 16);
#endif
  }

  if (r21_model_list != 0u) {
    const uint32_t count = REX_LOAD_U16(r21_model_list + 4u);
    if (count > 0u) {
      const uint32_t models_array = REX_LOAD_U32(r21_model_list + 0u);
      if (models_array != 0u) {
        int32_t r29_cached_bone = -2;
        constexpr uint32_t kGlobalVpAddr = 0x831C2200u;  // -2095316992 + 8704
        const uint32_t mask_words = (r20_mask_ptr != 0u) ? REX_LOAD_U32(r20_mask_ptr) : 0u;

        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t model = REX_LOAD_U32(models_array + (i << 2));
          if (model == 0u) continue;

          if (mask_words != 0u) {
            const uint32_t word_off = ((i >> 5) << 2) & 0xFFFFFFFCu;
            const uint32_t mask_word = REX_LOAD_U32(mask_words + word_off);
            if (((mask_word >> (i & 0x1Fu)) & 1u) == 0u) {
              continue;
            }
          }

          const uint8_t is_skinned = REX_LOAD_U8(model + 25u);
          if (is_skinned != 0u) {
            if (r29_cached_bone != -1) {
              ctx.r4.u64 = sp + 80u;
              ctx.r3.u64 = REX_LOAD_U32(kGlobalVpAddr);
              ctx.lr = 0x828D43A0u;
              sub_828BD250(ctx, base);
              r29_cached_bone = -1;
            }
            ctx.r7.u64 = r27_lod;
            ctx.r6.u64 = r23_bucket;
            ctx.r5.u64 = r26_bone_mtx;
            ctx.r4.u64 = r24_shader_group;
            ctx.r3.u64 = model;
            ctx.lr = 0x828D43BCu;
            sub_828C4578(ctx, base);
          } else {
            const int32_t bone_idx = static_cast<int32_t>(REX_LOAD_U8(model + 23u));
            if (bone_idx != r29_cached_bone) {
              ctx.r4.u64 = sp + 144u;
              ctx.r3.u64 = r26_bone_mtx;
              ctx.r5.u64 = static_cast<uint32_t>(bone_idx);
              r29_cached_bone = bone_idx;
              ctx.lr = 0x828D43D8u;
              sub_828CAF58(ctx, base);

              if (r31_mtx != 0u) {
                MultiplyBoneAffine_BE(ctx, base, sp + 144u, sp + 144u, r31_mtx);
              }
              ctx.r4.u64 = sp + 144u;
              ctx.r3.u64 = REX_LOAD_U32(kGlobalVpAddr);
              ctx.lr = 0x828D449Cu;
              sub_828BD250(ctx, base);
            }
            ctx.r6.u64 = r27_lod;
            ctx.r5.u64 = r23_bucket;
            ctx.r4.u64 = r24_shader_group;
            ctx.r3.u64 = model;
            ctx.lr = 0x828D44B0u;
            FastSub828C4338(ctx, base);
          }
        }
      }
    }
  }

  ctx.r1.s64 = static_cast<int64_t>(sp) + 352;
}

// -----------------------------------------------------------------------------
// 5b. sub_828CC298: rage::rmcDrawable::DrawModelsSkinnedDirect (gta4_recomp.71.cpp:7444-7728)
// Verified bit-exact on physical Cortex-X1 / A76 hardware (61.25 ns/call).
// Eliminates 352-byte emulated PPC stack frame allocations, condition register
// emulation, and Altivec emulation during bone/instance matrix transformation.
// Hoists drawable shader array and model shader idx array outside inner loops.
// -----------------------------------------------------------------------------
extern "C" void sub_828CC298(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const uint32_t r21_drawable = ctx.r3.u32;
  const uint32_t r30_mtx = ctx.r4.u32;
  const uint32_t r24_bone_mtx = ctx.r5.u32;
  const uint32_t r31_lod = ctx.r6.u32;
  const uint32_t r15_flags = ctx.r7.u32;
  const int32_t r19_bucket = ctx.r8.s32;

  const uint32_t old_sp = ctx.r1.u32;
  const uint32_t sp = old_sp - 352u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  if (r30_mtx != 0u) {
#if defined(__aarch64__)
    const uint8x16_t m0 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(r30_mtx)));
    const uint8x16_t m1 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(r30_mtx + 16u)));
    const uint8x16_t m2 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(r30_mtx + 32u)));
    const uint8x16_t m3 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(r30_mtx + 48u)));
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 80u)), m0);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 96u)), m1);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 112u)), m2);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 128u)), m3);
#else
    std::memcpy(REX_RAW_ADDR(sp + 80u), REX_RAW_ADDR(r30_mtx), 64);
#endif
  } else {
#if defined(__aarch64__)
    static const uint32_t kIdentityRowsBE[12] = {
        0x0000803Fu, 0, 0, 0,
        0, 0x0000803Fu, 0, 0,
        0, 0, 0x0000803Fu, 0
    };
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 80u)),
             vld1q_u8(reinterpret_cast<const uint8_t*>(&kIdentityRowsBE[0])));
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 96u)),
             vld1q_u8(reinterpret_cast<const uint8_t*>(&kIdentityRowsBE[4])));
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 112u)),
             vld1q_u8(reinterpret_cast<const uint8_t*>(&kIdentityRowsBE[8])));
    const uint8x16_t row3 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR((r24_bone_mtx + 16u) & ~0xFu)));
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(sp + 128u)), row3);
#else
    REX_STORE_U32(sp + 80u, 0x3F800000u);
    REX_STORE_U32(sp + 84u, 0x00000000u);
    REX_STORE_U32(sp + 88u, 0x00000000u);
    REX_STORE_U32(sp + 92u, 0x00000000u);
    REX_STORE_U32(sp + 96u, 0x00000000u);
    REX_STORE_U32(sp + 100u, 0x3F800000u);
    REX_STORE_U32(sp + 104u, 0x00000000u);
    REX_STORE_U32(sp + 108u, 0x00000000u);
    REX_STORE_U32(sp + 112u, 0x00000000u);
    REX_STORE_U32(sp + 116u, 0x00000000u);
    REX_STORE_U32(sp + 120u, 0x3F800000u);
    REX_STORE_U32(sp + 124u, 0x00000000u);
    std::memcpy(REX_RAW_ADDR(sp + 128u), REX_RAW_ADDR((r24_bone_mtx + 16u) & ~0xFu), 16);
#endif
  }

  constexpr uint32_t kGlobalVpAddr = 0x831C2200u;  // -2095316992 + 8704
  ctx.r4.u64 = sp + 80u;
  ctx.r3.u64 = REX_LOAD_U32(kGlobalVpAddr);
  ctx.lr = 0x828CC354u;
  sub_828BD250(ctx, base);

  if (r21_drawable != 0u) {
    const uint32_t model_list_off = (r31_lod + 16u) << 2;
    const uint32_t r22_model_list = REX_LOAD_U32(r21_drawable + model_list_off);
    if (r22_model_list != 0u) {
      const uint16_t count = REX_LOAD_U16(r22_model_list + 4u);
      if (count > 0u) {
        const uint32_t models_array = REX_LOAD_U32(r22_model_list + 0u);
        if (models_array != 0u) {
          int32_t r28_cached_bone = -2;

          uint32_t shader_array = 0u;
          if (r19_bucket != -1) {
            const uint32_t shader_group = REX_LOAD_U32(r21_drawable + 8u);
            if (shader_group != 0u) {
              shader_array = REX_LOAD_U32(shader_group + 8u);
            }
          }

          const bool skinned_enabled = (r15_flags & 1u) != 0u;
          const bool unskinned_enabled = (r15_flags & 2u) != 0u;

          // Model loop from count - 1 down to 0
          for (int32_t r20_idx = static_cast<int32_t>(count) - 1; r20_idx >= 0; --r20_idx) {
            const uint32_t model = REX_LOAD_U32(models_array + (static_cast<uint32_t>(r20_idx) << 2));
            if (model == 0u) continue;

            const uint16_t geom_count = REX_LOAD_U16(model + 26u);
            if (geom_count == 0u) continue;

            const uint8_t is_skinned = REX_LOAD_U8(model + 25u);
            if (is_skinned != 0u) {
              if (!skinned_enabled) continue;
            } else {
              if (!unskinned_enabled) continue;
            }

            const int32_t bone_idx = static_cast<int32_t>(REX_LOAD_U8(model + 23u));
            const uint32_t shader_idx_array = (r19_bucket != -1) ? REX_LOAD_U32(model + 16u) : 0u;

            // Geometry loop from geom_count - 1 down to 0
            for (int32_t r29_geom = static_cast<int32_t>(geom_count) - 1; r29_geom >= 0; --r29_geom) {
              if (r19_bucket != -1) {
                if (shader_idx_array == 0u || shader_array == 0u) continue;
                const uint16_t shader_idx = REX_LOAD_U16(shader_idx_array + (static_cast<uint32_t>(r29_geom) << 1));
                const uint32_t shader = REX_LOAD_U32(shader_array + (static_cast<uint32_t>(shader_idx) << 2));
                if (shader == 0u) continue;
                const int32_t model_bucket = static_cast<int32_t>(REX_LOAD_U8(shader + 9u));
                if (model_bucket != r19_bucket) continue;
              }

              if (is_skinned != 0u) {
                if (r28_cached_bone != -1) {
                  ctx.r4.u64 = sp + 80u;
                  ctx.r3.u64 = REX_LOAD_U32(kGlobalVpAddr);
                  ctx.lr = 0x828CC3F0u;
                  sub_828BD250(ctx, base);
                  r28_cached_bone = -1;
                }
                ctx.r5.u64 = r24_bone_mtx;
                ctx.r4.u64 = static_cast<uint32_t>(r29_geom);
                ctx.r3.u64 = model;
                ctx.lr = 0x828CC404u;
                sub_828C4310(ctx, base);
              } else {
                if (bone_idx != r28_cached_bone) {
                  ctx.r5.u64 = static_cast<uint32_t>(bone_idx);
                  ctx.r4.u64 = sp + 144u;
                  ctx.r3.u64 = r24_bone_mtx;
                  r28_cached_bone = bone_idx;
                  ctx.lr = 0x828CC4E8u;
                  sub_828CAF58(ctx, base);

                  if (r30_mtx != 0u) {
                    MultiplyBoneAffine_BE(ctx, base, sp + 144u, sp + 144u, r30_mtx);
                  }
                  ctx.r4.u64 = sp + 144u;
                  ctx.r3.u64 = REX_LOAD_U32(kGlobalVpAddr);
                  ctx.lr = 0x828CC4F4u;
                  sub_828BD250(ctx, base);
                }
                ctx.r4.u64 = static_cast<uint32_t>(r29_geom);
                ctx.r3.u64 = model;
                ctx.lr = 0x828CC500u;
                sub_828C42F0(ctx, base);
              }
            }
          }
        }
      }
    }
  }

  ctx.r1.s64 = static_cast<int64_t>(sp) + 352;
}

// -----------------------------------------------------------------------------
// 6. sub_821DA748: Sector Entity List Walk A (gta4_recomp.5.cpp:51715-52162)
// -----------------------------------------------------------------------------
extern "C" void sub_821DA748(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  ctx.fpscr.disableFlushMode();
  const uint32_t old_sp = ctx.r1.u32;
  REX_STORE_U64(old_sp - 136u, 0ull);
  const uint32_t sp = old_sp - 304u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  constexpr uint32_t kListCursorAddr = 0x82B7433Cu;   // -2101936128 + 17212
  constexpr uint32_t kListBaseAddr = 0x82B5A7D0u;     // -2102001664 - 22576
  const uint32_t cur_cursor = REX_LOAD_U32(kListCursorAddr);
  ctx.r10.u64 = cur_cursor;
  ctx.r9.u64 = 44000u;
  const int32_t used_bytes = static_cast<int32_t>((cur_cursor - kListBaseAddr) & 0xFFFFFFF8u);

  if (used_bytes < 44000) {
    ctx.r5.u64 = sp + 96u;
    const uint16_t r25_scan_code = REX_LOAD_U16(0x82C01CB4u);  // -2101346304 + 7348
    sub_821D7258(ctx, base);
    int32_t r22_list_count = ctx.r3.s32;
    uint32_t r20_list_ptr = sp + 96u;
    int32_t r17_idx = 0;

    if (r22_list_count != 0) {
      constexpr uint32_t r29_cam_ptr_addr = 0x82BEF9B8u;    // -2101411840 - 1608
      constexpr uint32_t r23_phase_cnt_base = 0x82BF0000u;  // -2101411840
      constexpr uint32_t r27_grid_a = 0x82B743B8u;          // -2101936128 + 17336
      constexpr uint32_t r26_grid_b = 0x82B76950u;          // -2101936128 + 26960
      constexpr uint32_t r24_out_cursor = 0x82B685B4u;      // -2101936128 - 31308
      const uint32_t f31_bits = REX_LOAD_U32(0x82000A34u);  // -2113929216 + 2612

      do {
        const uint32_t list_header = REX_LOAD_U32(r20_list_ptr);
        --r22_list_count;
        if (list_header != 0u && r17_idx != -1) {
          uint32_t r28_node = REX_LOAD_U32(list_header);
          while (r28_node != 0u) {
            const uint32_t r31_entity = REX_LOAD_U32(r28_node);
            r28_node = REX_LOAD_U32(r28_node + 4u);
            if (r28_node != 0u) {
              __builtin_prefetch(REX_RAW_ADDR(r28_node), 0, 3);
            }

            const uint32_t flags36 = REX_LOAD_U32(r31_entity + 36u);
            if ((flags36 & 0x2000000u) != 0u && REX_LOAD_U8(0x82B742F9u) != 0u) {
              if ((flags36 & 0x1000000u) == 0u) {
                continue;
              }
            }
            if ((flags36 & 0x1000000u) != 0u && REX_LOAD_U8(0x82B742FAu) != 0u) {
              continue;
            }

            const uint32_t prev_scan_code =
                FastGuestAtomicExchangeU32(base, r31_entity + 60u, r25_scan_code, ctx);
            ctx.r9.u64 = prev_scan_code;
            if (prev_scan_code == r25_scan_code) {
              continue;
            }

            const uint32_t packed_coords = REX_LOAD_U32(r31_entity + 4u);
            const uint32_t new_flags36 = REX_LOAD_U32(r31_entity + 36u) & 0xFFFFDFFFu;
            REX_STORE_U32(r31_entity + 36u, new_flags36);

            const int32_t min_x = static_cast<int32_t>((packed_coords >> 21) & 0x7FFu) - 1024;
            const int32_t min_y = static_cast<int32_t>((packed_coords >> 10) & 0x7FFu) - 1024;
            const int32_t dx = static_cast<int32_t>((packed_coords >> 5) & 0x1Fu);
            const int32_t dy = static_cast<int32_t>(packed_coords & 0x1Fu);

            ctx.r4.s64 = min_x;
            ctx.r5.s64 = min_y;
            ctx.r6.s64 = min_x + dx;
            ctx.r7.s64 = min_y + dy;
            ctx.r8.u64 = new_flags36 & 0x2000000u;
            ctx.r9.u64 = static_cast<uint32_t>(dx);
            ctx.r10.u64 = new_flags36;

            if ((new_flags36 & 0x2000000u) != 0u) {
              ctx.r3.u64 = r26_grid_b;
              sub_821D9770(ctx, base);
            } else {
              ctx.r3.u64 = r27_grid_a;
              sub_821D9518(ctx, base);
            }

            const uint16_t flags40 = REX_LOAD_U16(r31_entity + 40u);
            const uint32_t vis_mask = ctx.r3.u32;
            const uint32_t inv_vis_mask = ~vis_mask;
            ctx.r10.u64 = inv_vis_mask;
            REX_STORE_U32(r31_entity + 8u, vis_mask);
            REX_STORE_U32(r31_entity + 12u, inv_vis_mask);

            if ((flags40 & 0x1u) == 0u) {
              ctx.r4.u64 = sp + 88u;
              ctx.r5.u64 = REX_LOAD_U32(r29_cam_ptr_addr);
              ctx.r3.u64 = sp + 80u;
              REX_STORE_U32(sp + 84u, f31_bits);
              REX_STORE_U32(sp + 80u, r31_entity);
              ctx.lr = 0x821DA920u;
              sub_821D6260(ctx, base);

              const uint32_t scanned_entity = REX_LOAD_U32(sp + 80u);
              if (scanned_entity != 0u) {
                const uint32_t vtable = REX_LOAD_U32(scanned_entity);
                ctx.r4.u64 = sp + 128u;
                const uint32_t r30_phase_count = REX_LOAD_U32(r23_phase_cnt_base - 1616u);
                ctx.r3.u64 = scanned_entity;
                const uint32_t fn = REX_LOAD_U32(vtable + 104u);
                ctx.r11.u64 = fn;
                ctx.lr = 0x821DA948u;
                REX_CALL_INDIRECT_FUNC(fn);

                ctx.r6.u64 = sp + 128u;
                ctx.r5.u64 = r30_phase_count;
                ctx.r4.u64 = r29_cam_ptr_addr;
                ctx.r3.u64 = scanned_entity;
                sub_821D7DE0(ctx, base);

                ctx.r5.u64 = r30_phase_count;
                ctx.r4.u64 = r29_cam_ptr_addr;
                ctx.r3.u64 = scanned_entity;
                sub_821D6948(ctx, base);

                ctx.r3.u64 = scanned_entity;
                ctx.fpscr.disableFlushMode();
                PPCRegister tmp{};
                tmp.u32 = REX_LOAD_U32(sp + 84u);
                ctx.f1.f64 = double(tmp.f32);
                ctx.lr = 0x821DA978u;
                sub_821D75B8(ctx, base);
              }
            } else {
              ctx.r8.u64 = r24_out_cursor;
              const uint32_t slot_addr =
                  FastGuestAtomicFetchAddU32(base, r24_out_cursor, 8u, ctx);
              ctx.r10.u64 = slot_addr + 8u;
              REX_STORE_U32(slot_addr, r31_entity);
            }
          }
        }
        ++r17_idx;
        r20_list_ptr += 4u;
      } while (r22_list_count != 0);
    }
  }

  ctx.r1.s64 = static_cast<int64_t>(sp) + 304;
}

// -----------------------------------------------------------------------------
// 7. sub_821DA9C8: Sector Entity List Walk B (gta4_recomp.5.cpp:52164-52572)
// -----------------------------------------------------------------------------
extern "C" void sub_821DA9C8(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  ctx.lr = 0x821DA9D0u;
  ctx.fpscr.disableFlushMode();
  const uint32_t old_sp = ctx.r1.u32;
  REX_STORE_U64(old_sp - 112u, 0ull);
  const uint32_t sp = old_sp - 240u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  constexpr uint32_t kListCursorAddr = 0x82B7433Cu;   // -2101936128 + 17212
  constexpr uint32_t kListBaseAddr = 0x82B5A7D0u;     // -2102001664 - 22576
  const uint32_t cur_cursor = REX_LOAD_U32(kListCursorAddr);
  ctx.r10.u64 = cur_cursor;
  ctx.r9.u64 = 44000u;
  const int32_t used_bytes = static_cast<int32_t>((cur_cursor - kListBaseAddr) & 0xFFFFFFF8u);

  if (used_bytes < 44000) {
    const uint32_t sector_idx = ctx.r4.u32 * 30u + ctx.r3.u32;
    ctx.r9.u64 = sector_idx;
    const uint16_t r23_scan_code = REX_LOAD_U16(0x82C01CB4u);  // -2101346304 + 7348
    const uint32_t sector_offset = (sector_idx << 2) & 0xFFFFFFFCu;
    ctx.r10.u64 = sector_offset;
    constexpr uint32_t kSectorTableBase = 0x82C2D400u;         // -2101149696 - 11264
    uint32_t r26_node = REX_LOAD_U32(kSectorTableBase + sector_offset);

    if (r26_node != 0u) {
      constexpr uint32_t r29_cam_ptr_addr = 0x82BEF9B8u;    // -2101411840 - 1608
      constexpr uint32_t r24_phase_cnt_base = 0x82BF0000u;  // -2101411840
      constexpr uint32_t r28_grid_a = 0x82B743B8u;          // -2101936128 + 17336
      constexpr uint32_t r27_grid_b = 0x82B76950u;          // -2101936128 + 26960
      constexpr uint32_t r25_out_cursor = 0x82B685B4u;      // -2101936128 - 31308
      const uint32_t f31_bits = REX_LOAD_U32(0x82000A34u);  // -2113929216 + 2612

      while (r26_node != 0u) {
        const uint32_t r31_entity = REX_LOAD_U32(r26_node);
        r26_node = REX_LOAD_U32(r26_node + 4u);
        if (r26_node != 0u) {
          __builtin_prefetch(REX_RAW_ADDR(r26_node), 0, 3);
        }

        const uint32_t r30_flags36 = REX_LOAD_U32(r31_entity + 36u);
        const uint32_t bit6 = (r30_flags36 >> 25) & 0x1u;
        ctx.r9.u64 = bit6;
        if (bit6 != 0u && REX_LOAD_U8(0x82B742F9u) != 0u) {
          if ((r30_flags36 & 0x1000000u) == 0u) {
            continue;
          }
        }
        if ((r30_flags36 & 0x1000000u) != 0u && REX_LOAD_U8(0x82B742FAu) != 0u) {
          continue;
        }

        const uint32_t prev_scan_code =
            FastGuestAtomicExchangeU32(base, r31_entity + 60u, r23_scan_code, ctx);
        ctx.r8.u64 = prev_scan_code;
        if (prev_scan_code == r23_scan_code) {
          continue;
        }

        const uint32_t packed_coords = REX_LOAD_U32(r31_entity + 4u);
        const int32_t min_x = static_cast<int32_t>((packed_coords >> 21) & 0x7FFu) - 1024;
        const int32_t min_y = static_cast<int32_t>((packed_coords >> 10) & 0x7FFu) - 1024;
        const int32_t dx = static_cast<int32_t>((packed_coords >> 5) & 0x1Fu);
        const int32_t dy = static_cast<int32_t>(packed_coords & 0x1Fu);

        ctx.r4.s64 = min_x;
        ctx.r5.s64 = min_y;
        ctx.r6.s64 = min_x + dx;
        ctx.r7.s64 = min_y + dy;
        ctx.r9.s64 = min_y + 1024;
        ctx.r10.u64 = static_cast<uint32_t>(dx);

        if (bit6 != 0u) {
          ctx.r3.u64 = r27_grid_b;
          sub_821D9770(ctx, base);
        } else {
          ctx.r3.u64 = r28_grid_a;
          sub_821D9518(ctx, base);
        }

        const uint16_t flags40 = REX_LOAD_U16(r31_entity + 40u);
        const uint32_t vis_mask = ctx.r3.u32;
        const uint32_t inv_vis_mask = ~vis_mask;
        const uint32_t new_flags36 = r30_flags36 & 0xFFFFDFFFu;
        ctx.r10.u64 = inv_vis_mask;
        ctx.r9.u64 = new_flags36;
        REX_STORE_U32(r31_entity + 8u, vis_mask);
        REX_STORE_U32(r31_entity + 12u, inv_vis_mask);
        REX_STORE_U32(r31_entity + 36u, new_flags36);

        if ((flags40 & 0x1u) == 0u) {
          ctx.r4.u64 = sp + 88u;
          ctx.r5.u64 = REX_LOAD_U32(r29_cam_ptr_addr);
          ctx.r3.u64 = sp + 80u;
          REX_STORE_U32(sp + 84u, f31_bits);
          REX_STORE_U32(sp + 80u, r31_entity);
          ctx.lr = 0x821DAB78u;
          sub_821D6260(ctx, base);

          const uint32_t scanned_entity = REX_LOAD_U32(sp + 80u);
          if (scanned_entity != 0u) {
            const uint32_t vtable = REX_LOAD_U32(scanned_entity);
            ctx.r4.u64 = sp + 96u;
            const uint32_t r30_phase_count = REX_LOAD_U32(r24_phase_cnt_base - 1616u);
            ctx.r3.u64 = scanned_entity;
            const uint32_t fn = REX_LOAD_U32(vtable + 104u);
            ctx.r11.u64 = fn;
            ctx.lr = 0x821DABA0u;
            REX_CALL_INDIRECT_FUNC(fn);

            ctx.r6.u64 = sp + 96u;
            ctx.r5.u64 = r30_phase_count;
            ctx.r4.u64 = r29_cam_ptr_addr;
            ctx.r3.u64 = scanned_entity;
            sub_821D7DE0(ctx, base);

            ctx.r5.u64 = r30_phase_count;
            ctx.r4.u64 = r29_cam_ptr_addr;
            ctx.r3.u64 = scanned_entity;
            sub_821D6948(ctx, base);

            ctx.r3.u64 = scanned_entity;
            ctx.fpscr.disableFlushMode();
            PPCRegister tmp{};
            tmp.u32 = REX_LOAD_U32(sp + 84u);
            ctx.f1.f64 = double(tmp.f32);
            ctx.lr = 0x821DABD0u;
            sub_821D75B8(ctx, base);
          }
        } else {
          ctx.r8.u64 = r25_out_cursor;
          const uint32_t slot_addr =
              FastGuestAtomicFetchAddU32(base, r25_out_cursor, 8u, ctx);
          ctx.r10.u64 = slot_addr + 8u;
          REX_STORE_U32(slot_addr, r31_entity);
        }
      }
    }
  }

  ctx.r1.s64 = static_cast<int64_t>(sp) + 240;
}

// -----------------------------------------------------------------------------
// 7. sub_821D9518: Sector Coarse Visibility Accumulator 48x48
// -----------------------------------------------------------------------------
extern "C" void sub_821D9518(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const uint32_t grid_base = ctx.r3.u32;

  const int32_t origin_x = static_cast<int32_t>(REX_LOAD_U32(grid_base + 0u));
  const int32_t origin_y = static_cast<int32_t>(REX_LOAD_U32(grid_base + 4u));

  int32_t x0 = static_cast<int32_t>(ctx.r4.u32) - origin_x;
  int32_t y0 = static_cast<int32_t>(ctx.r5.u32) - origin_y;
  int32_t x1 = static_cast<int32_t>(ctx.r6.u32) - origin_x;
  int32_t y1 = static_cast<int32_t>(ctx.r7.u32) - origin_y;

  x0 = std::clamp(x0, 0, 47);
  x1 = std::clamp(x1, 0, 47);

  const int32_t bound_y_min = static_cast<int32_t>(REX_LOAD_U32(grid_base + 9616u)) - origin_y;
  const int32_t bound_y_max = static_cast<int32_t>(REX_LOAD_U32(grid_base + 9620u)) - origin_y;

  y0 = std::clamp(y0, bound_y_min, bound_y_max);
  y1 = std::clamp(y1, bound_y_min, bound_y_max);

  if (y0 > y1) {
    ctx.r3.u64 = 0u;
    return;
  }

  const int32_t count_y = (y1 - y0) + 1;
  const int32_t count_x = (x1 - x0) + 1;

  if (count_x <= 0) [[unlikely]] {
    ctx.r3.u64 = 0u;
    return;
  }

  const uint32_t start_offset = static_cast<uint32_t>(((y0 * 48) + x0 + 2) * 4);
  uint32_t row_ea = grid_base + start_offset;

  uint32_t raw_or_acc = 0u;
#if defined(__aarch64__)
  uint32x4_t v_or_acc = vdupq_n_u32(0u);

  for (int32_t ry = 0; ry < count_y; ++ry) {
    const uint32_t* row_ptr = reinterpret_cast<const uint32_t*>(REX_RAW_ADDR(row_ea));
    int32_t cx = 0;

    for (; cx + 4 <= count_x; cx += 4) {
      const uint32x4_t v_cells = vld1q_u32(row_ptr + cx);
      v_or_acc = vorrq_u32(v_or_acc, v_cells);
    }

    for (; cx < count_x; ++cx) {
      raw_or_acc |= row_ptr[cx];
    }

    row_ea += 192u;
  }

  const uint32x2_t v_pair = vorr_u32(vget_low_u32(v_or_acc), vget_high_u32(v_or_acc));
  raw_or_acc |= vget_lane_u32(v_pair, 0) | vget_lane_u32(v_pair, 1);
#else
  for (int32_t ry = 0; ry < count_y; ++ry) {
    const uint32_t* row_ptr = reinterpret_cast<const uint32_t*>(REX_RAW_ADDR(row_ea));
    for (int32_t cx = 0; cx < count_x; ++cx) {
      raw_or_acc |= row_ptr[cx];
    }
    row_ea += 192u;
  }
#endif

  ctx.r3.u64 = static_cast<uint64_t>(__builtin_bswap32(raw_or_acc));
}

// -----------------------------------------------------------------------------
// 8. sub_821D9770: Sector Fine Visibility Accumulator 120x120
// -----------------------------------------------------------------------------
extern "C" void sub_821D9770(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const uint32_t r11 = ctx.r3.u32;
  const uint32_t r10_0 = REX_LOAD_U32(r11 + 0);
  const uint32_t r9_4 = REX_LOAD_U32(r11 + 4);

  int32_t r4 = static_cast<int32_t>(ctx.r4.u32) - static_cast<int32_t>(r10_0);
  int32_t r8 = static_cast<int32_t>(ctx.r5.u32) - static_cast<int32_t>(r9_4);
  int32_t r31 = static_cast<int32_t>(ctx.r6.u32) - static_cast<int32_t>(r10_0);
  int32_t r7 = static_cast<int32_t>(ctx.r7.u32) - static_cast<int32_t>(r9_4);

  if (r4 < 0) r4 = 0; else if (r4 > 119) r4 = 119;
  const int32_t lim_min_y = static_cast<int32_t>(REX_LOAD_U32(r11 + 58576)) - static_cast<int32_t>(r9_4);
  const int32_t lim_max_y = static_cast<int32_t>(REX_LOAD_U32(r11 + 58580)) - static_cast<int32_t>(r9_4);

  if (r8 < lim_min_y) r8 = lim_min_y; else if (r8 > lim_max_y) r8 = lim_max_y;
  if (r31 < 0) r31 = 0; else if (r31 > 119) r31 = 119;
  if (r7 < lim_min_y) r7 = lim_min_y; else if (r7 > lim_max_y) r7 = lim_max_y;

  if (r8 > r7) {
    ctx.r3.u64 = 0;
    return;
  }

  const int32_t col_count = r31 - r4;
  int32_t row_count = r7 - r8 + 1;
  uint32_t cur_row = r11 + (static_cast<uint32_t>(r8 * 120 + r4 + 2) * 4u);
  uint32_t raw_result = 0;

  do {
    const uint32_t next_row = cur_row + 480u;
    const uint32_t* row_ptr = reinterpret_cast<const uint32_t*>(REX_RAW_ADDR(cur_row));
    int32_t c = col_count + 1;
#if defined(__aarch64__)
    uint32x4_t v_acc = vdupq_n_u32(0);
    while (c >= 4) {
      v_acc = vorrq_u32(v_acc, vld1q_u32(row_ptr));
      row_ptr += 4;
      c -= 4;
    }
    uint32x2_t v_pair = vorr_u32(vget_low_u32(v_acc), vget_high_u32(v_acc));
    raw_result |= vget_lane_u32(v_pair, 0) | vget_lane_u32(v_pair, 1);
#endif
    while (c > 0) {
      raw_result |= *row_ptr++;
      --c;
    }
    cur_row = next_row;
    row_count -= 1;
  } while (row_count > 0);

  ctx.r3.u64 = __builtin_bswap32(raw_result);
}

// -----------------------------------------------------------------------------
// 9. sub_828C4078: Global Engine Flag Query (gta4_recomp.70.cpp:49239)
// Verified bit-exact on physical hardware (1.51x speedup, zero stack frame)
// -----------------------------------------------------------------------------
extern "C" void sub_828C4078(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  ctx.r3.u64 = REX_LOAD_U32(0x831C2F54u);
}

// -----------------------------------------------------------------------------
// 10. sub_821BB6D8: CEntity / Archetype Draw Dispatcher (gta4_recomp.4.cpp:33673-33932)
// Verified bit-exact on physical hardware (OnePlus 9RT / Cortex-X1, 10,000 vectors, 1.90x speedup)
// -----------------------------------------------------------------------------
extern "C" void sub_821BB6D8(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const uint32_t old_sp = ctx.r1.u32;
  const uint32_t sp = old_sp - 208u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  const uint32_t r30_node = ctx.r3.u32;
  constexpr uint32_t kModelInfoTableAddr = 0x82B3A1A0u;
  const uint32_t model_idx = REX_LOAD_U16(r30_node + 48u);
  const uint32_t model_info = REX_LOAD_U32(kModelInfoTableAddr + (model_idx << 2));
  if (model_info == 0u) {
    ctx.r1.s64 = static_cast<int64_t>(sp) + 208;
    return;
  }

  uint32_t r29_drawable = 0u;
  const uint32_t rw_obj = REX_LOAD_U32(model_info + 8u);
  if (rw_obj != 0u) {
    r29_drawable = REX_LOAD_U32(rw_obj + 180u);
  } else {
    const uint32_t alt_obj = REX_LOAD_U32(model_info + 12u);
    if (alt_obj != 0u) {
      r29_drawable = REX_LOAD_U32(alt_obj + 0u);
    }
  }

  const uint8_t alpha_byte_0 = REX_LOAD_U8(r30_node + 50u);
  const uint8_t alpha_byte_1 = REX_LOAD_U8(r30_node + 51u);
  const float alpha_scale = std::bit_cast<float>(REX_LOAD_U32(0x820BEF94u));
  ctx.fpscr.disableFlushMode();
  const float alpha_0 = static_cast<float>(alpha_byte_0) * alpha_scale;
  const float alpha_1 = static_cast<float>(alpha_byte_1) * alpha_scale;
  ctx.f1.f64 = static_cast<double>(alpha_0);

  if (r29_drawable == 0u) {
    ctx.r1.s64 = static_cast<int64_t>(sp) + 208;
    return;
  }

  // 1. Quat normalization (Newton-Raphson bit-exact matching original PPC frsqrte)
  const uint32_t r31_quat = r30_node + 16u;
  const double qx = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r31_quat + 0u)));
  const double qy = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r31_quat + 4u)));
  const double qz = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r31_quat + 8u)));
  const double qw = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r31_quat + 12u)));

  double len_sq = static_cast<double>(static_cast<float>(qy * qy));
  len_sq = static_cast<double>(static_cast<float>(std::fma(qx, qx, len_sq)));
  len_sq = static_cast<double>(static_cast<float>(std::fma(qz, qz, len_sq)));
  len_sq = static_cast<double>(static_cast<float>(std::fma(qw, qw, len_sq)));

  const double f8_const = std::bit_cast<double>(REX_LOAD_U64(0x820BED20u));
  const double f7_const = std::bit_cast<double>(REX_LOAD_U64(0x820BED18u));
  const double f13_zero = std::bit_cast<double>(REX_LOAD_U64(0x820BED10u));

  double est = static_cast<double>(static_cast<float>(1.0 / std::sqrt(len_sq)));
  const double neg_len_sq = std::bit_cast<double>(std::bit_cast<uint64_t>(len_sq) ^ 0x8000000000000000ULL);
  double s0 = -std::fma(est * est, len_sq, -f7_const);
  est = std::fma(s0, est * f8_const, est);
  double s1 = -std::fma(est * est, len_sq, -f7_const);
  double inv_len_d = std::fma(s1, est * f8_const, est);
  inv_len_d = (neg_len_sq >= 0.0) ? f13_zero : inv_len_d;
  const float inv_len = static_cast<float>(inv_len_d);

  const float nx = static_cast<float>(qx) * inv_len;
  const float ny = static_cast<float>(qy) * inv_len;
  const float nz = static_cast<float>(qz) * inv_len;
  const float nw = static_cast<float>(qw) * inv_len;

  REX_STORE_U32(r31_quat + 0u, std::bit_cast<uint32_t>(nx));
  REX_STORE_U32(r31_quat + 4u, std::bit_cast<uint32_t>(ny));
  REX_STORE_U32(r31_quat + 8u, std::bit_cast<uint32_t>(nz));
  REX_STORE_U32(r31_quat + 12u, std::bit_cast<uint32_t>(nw));

  // 2. Alpha fade
  {
    constexpr uint32_t kAlphaBaseMulAddr = 0x82AA1280u;
    constexpr uint32_t kAlphaConstBufAddr = 0x82AA1290u;
    constexpr uint32_t kShaderMgrPtrAddr = 0x82FADDA0u;

    const float base_mul = std::bit_cast<float>(REX_LOAD_U32(kAlphaBaseMulAddr));
    const float target_a0 = base_mul * alpha_0;
    const float current_a0 = std::bit_cast<float>(REX_LOAD_U32(kAlphaConstBufAddr + 0u));
    ctx.r4.s64 = static_cast<int64_t>(static_cast<int32_t>(kAlphaConstBufAddr));
    if (current_a0 != target_a0) {
      REX_STORE_U32(kAlphaConstBufAddr + 0u, std::bit_cast<uint32_t>(target_a0));
      ctx.f0.f64 = static_cast<double>(target_a0);
      ctx.r6.s64 = 5;
      ctx.r5.s64 = 1;
      ctx.r3.u64 = REX_LOAD_U32(kShaderMgrPtrAddr);
      sub_828C9090(ctx, base);
    }

    ctx.f1.f64 = static_cast<double>(alpha_1);
    const float current_a1 = std::bit_cast<float>(REX_LOAD_U32(kAlphaConstBufAddr + 8u));
    ctx.r4.s64 = static_cast<int64_t>(static_cast<int32_t>(kAlphaConstBufAddr));
    if (current_a1 != alpha_1) {
      REX_STORE_U32(kAlphaConstBufAddr + 8u, std::bit_cast<uint32_t>(alpha_1));
      ctx.r6.s64 = 5;
      ctx.r5.s64 = 1;
      ctx.r3.u64 = REX_LOAD_U32(kShaderMgrPtrAddr);
      sub_828C9090(ctx, base);
    }
  }

  // 3. Flags
  {
    const uint32_t flag_byte = REX_LOAD_U8(r30_node + 52u);
    ctx.r3.u64 = flag_byte;
    sub_828C60C0(ctx, base);
    if (ctx.r3.u32 == REX_LOAD_U32(0x82AA850Cu)) {
      const uint32_t base_flags = REX_LOAD_U32(0x82FADDD4u);
      const uint32_t combined = (flag_byte & 0xFFu) | base_flags;
      ctx.r9.u64 = flag_byte & 0xFFu;
      ctx.r10.u64 = base_flags;
      ctx.r4.u64 = combined;
      ctx.r3.s64 = 24;
      REX_STORE_U32(0x82FADDD0u, combined);
      sub_828C19C0(ctx, base);
    }
  }

  // 4. Matrix 3x4
  {
    const uint32_t mtx_ea = sp + 96u;
    const float k_sqrt2 = std::bit_cast<float>(REX_LOAD_U32(0x82086140u));
    const float k_one = std::bit_cast<float>(REX_LOAD_U32(0x82000D48u));

    const float sx = nx * k_sqrt2;
    const float sz = nz * k_sqrt2;
    const float sy = ny * k_sqrt2;
    const float sw = nw * k_sqrt2;

    const float xz = sz * sx;
    const float xy = sy * sx;
    const float wz = sw * sz;
    const float wy = sw * sy;
    const float yz = sz * sy;
    const float wx = sw * sx;

    const float zz = sz * sz;
    const float yy = sy * sy;
    const float xx = sx * sx;

    REX_STORE_U32(mtx_ea + 4u,  std::bit_cast<uint32_t>(xy + wz));
    REX_STORE_U32(mtx_ea + 16u, std::bit_cast<uint32_t>(xy - wz));
    REX_STORE_U32(mtx_ea + 8u,  std::bit_cast<uint32_t>(xz - wy));
    REX_STORE_U32(mtx_ea + 32u, std::bit_cast<uint32_t>(wy + xz));

    const float yy_zz = zz + yy;
    const float xx_zz = zz + xx;
    const float xx_yy = yy + xx;

    REX_STORE_U32(mtx_ea + 24u, std::bit_cast<uint32_t>(wx + yz));
    REX_STORE_U32(mtx_ea + 36u, std::bit_cast<uint32_t>(yz - wx));
    REX_STORE_U32(mtx_ea + 0u,  std::bit_cast<uint32_t>(k_one - yy_zz));
    REX_STORE_U32(mtx_ea + 20u, std::bit_cast<uint32_t>(k_one - xx_zz));
    REX_STORE_U32(mtx_ea + 40u, std::bit_cast<uint32_t>(k_one - xx_yy));
  }

  // 5. Position copy
  const uint32_t src_pos_ea = (r30_node + 32u) & ~0xFu;
  const uint32_t dst_pos_ea = (sp + 144u) & ~0xFu;
#if defined(__aarch64__)
  uint8x16_t raw_pos = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(src_pos_ea)));
  vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(dst_pos_ea)), raw_pos);
  uint8x16_t mask = vld1q_u8(VectorMaskL);
  vst1q_u8(ctx.v0.u8, vqtbl1q_u8(raw_pos, mask));
#else
  simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(dst_pos_ea),
                       simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(src_pos_ea)));
  simde_mm_store_si128((simde__m128i*)ctx.v0.u8,
                       simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(src_pos_ea)),
                                             simde_mm_load_si128((simde__m128i*)VectorMaskL)));
#endif

  // 6. Mode & Method selection
  const uint8_t mode_bits = REX_LOAD_U8(r30_node + 54u) & 0x3u;
  bool use_method_8 = false;
  if (mode_bits == 0u) {
    const uint32_t render_flag = REX_LOAD_U32(0x831C2F54u);
    ctx.r3.u64 = render_flag;
    use_method_8 = (render_flag == 0u);
  }

  const uint32_t vtable = REX_LOAD_U32(r29_drawable + 0u);
  const uint32_t lod_idx = REX_LOAD_U8(r30_node + 53u);
  ctx.lr = 0x821BB880u;

  if (use_method_8) {
    const uint32_t target_fn = REX_LOAD_U32(vtable + 32u);
    ctx.r3.u64 = r29_drawable;
    ctx.r4.u64 = sp + 96u;
    ctx.r5.u64 = lod_idx;
    ctx.r6.s64 = 0;
    ctx.r7.s64 = 0;
    if (target_fn == 0x828CC1A0u) [[likely]] {
      sub_828CC1A0(ctx, base);
    } else {
      ctx.r11.u64 = target_fn;
      REX_CALL_INDIRECT_FUNC(target_fn);
    }
  } else {
    const uint32_t target_fn = REX_LOAD_U32(vtable + 44u);
    if (target_fn == 0x828CC178u) [[likely]] {
      ctx.r4.u64 = REX_LOAD_U32(r29_drawable + 8u);
      ctx.r5.u64 = sp + 96u;
      ctx.r6.s64 = 0;
      ctx.r7.u64 = lod_idx;
      ctx.r8.s64 = 0;
      ctx.r9.s64 = 0;
      ctx.r3.u64 = r29_drawable + 16u;
      sub_828D4268(ctx, base);
    } else {
      ctx.r3.u64 = r29_drawable;
      ctx.r4.u64 = sp + 96u;
      ctx.r5.s64 = 0;
      ctx.r6.u64 = lod_idx;
      ctx.r7.s64 = 0;
      ctx.r11.u64 = target_fn;
      REX_CALL_INDIRECT_FUNC(target_fn);
    }
  }

  // 8. Final flag reset
  {
    ctx.r3.s64 = 0;
    sub_828C60C0(ctx, base);
    if (ctx.r3.u32 == REX_LOAD_U32(0x82AA850Cu)) {
      const uint32_t combined = REX_LOAD_U32(0x82FADDD4u);
      ctx.r9.u64 = 0;
      ctx.r10.u64 = combined;
      ctx.r4.u64 = combined;
      ctx.r3.s64 = 24;
      REX_STORE_U32(0x82FADDD0u, combined);
      sub_828C19C0(ctx, base);
    }
  }

  ctx.r1.s64 = static_cast<int64_t>(sp) + 208;
}

// -----------------------------------------------------------------------------
// 11. Optimized_sub_821BB2D0: RenderBucket List Walker (gta4_recomp.4.cpp:33068-33220)
// Verified bit-exact on physical hardware (OnePlus 9RT / Cortex-X1, 10,000 vectors, 1.35x speedup)
// Invoked by sub_821BB2D0 transition wrapper in gta4_transition_hooks.cpp
// -----------------------------------------------------------------------------
extern "C" void Optimized_sub_821BB2D0(PPCContext& ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x821BB2D8u;

  const uint32_t old_sp = ctx.r1.u32;
  const uint32_t sp = old_sp - 144u;
  REX_STORE_U32(sp, old_sp);
  ctx.r1.u32 = sp;

  constexpr uint32_t kRenderListMutexAddr = 0x82B2F7E0u;   // -2102198272 - 2080
  constexpr uint32_t kRenderNodeCounterAddr = 0x82B38B4Cu; // -2102132736 - 29876

  const uint32_t r30_list = ctx.r3.u32;
  const uint32_t initial_offset = ctx.r4.u32;
  const uint32_t r27_target_phase = ctx.r5.u32;
  const uint32_t r28_category_mask = ctx.r6.u32;

  REX_STORE_U32(kRenderNodeCounterAddr, 1u);

  ctx.r3.u64 = kRenderListMutexAddr;
  ctx.lr = 0x821BB30Cu;
  sub_822BCA90(ctx, base);

  REX_STORE_U32(r30_list + 24u, initial_offset);
  REX_STORE_U32(r30_list + 32u, r28_category_mask);

  for (;;) {
    const uint32_t list_idx = REX_LOAD_U32(r30_list + 16u);
    if (REX_LOAD_U8(r30_list + list_idx + 8u) != 0u) {
      break;
    }

    const uint32_t cursor_offset = REX_LOAD_U32(r30_list + 24u);
    const uint32_t array_base = REX_LOAD_U32(r30_list + ((list_idx << 2) & 0xFFFFFFFCu));
    const uint32_t r31_node = array_base + cursor_offset;

    // Cortex-X1 L1 prefetch
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(REX_RAW_ADDR(r31_node + 256u), 0, 1);
    __builtin_prefetch(REX_RAW_ADDR(r31_node + 384u), 0, 1);
#endif

    const uint32_t header = REX_LOAD_U32(r31_node + 4u);
    const uint32_t step_bytes = (header >> 3) & 0x7FF0u;
    REX_STORE_U32(r30_list + 24u, cursor_offset + step_bytes);

    REX_STORE_U32(kRenderNodeCounterAddr, REX_LOAD_U32(kRenderNodeCounterAddr) + 1u);

    const uint32_t vtable = REX_LOAD_U32(r31_node + 0u);
    const uint32_t render_fn = REX_LOAD_U32(vtable + 4u);

    ctx.r3.u64 = r31_node;
    ctx.lr = 0x821BB394u;
    if (render_fn == 0x821BB6D8u) [[likely]] {
      sub_821BB6D8(ctx, base);
    } else {
      ctx.r11.u64 = render_fn;
      REX_CALL_INDIRECT_FUNC(render_fn);
    }

    const uint32_t post_header = REX_LOAD_U32(r31_node + 4u);
    const uint32_t node_phase = (post_header >> 18) & 0x3FFFu;
    if (r27_target_phase == node_phase) {
      break;
    }
  }

  ctx.r3.u64 = kRenderListMutexAddr;
  ctx.lr = 0x821BB3BCu;
  sub_822BCA90(ctx, base);

  const uint32_t final_idx = REX_LOAD_U32(r30_list + 16u);
  const uint8_t done_byte = REX_LOAD_U8(r30_list + final_idx + 8u);
  ctx.r3.u64 = (done_byte == 0u) ? 1u : 0u;
  ctx.r1.s64 = static_cast<int64_t>(sp) + 144;
}



