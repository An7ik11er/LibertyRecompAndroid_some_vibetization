// Derived by tools/derive_draw_distance_hooks.py --write.
// Complete retail PPC logic; only the documented operand below is replaced.
// Hand-optimized for ARM64 NEON, inlined sub_821D0478/sub_823B32E0, and
// non-volatile L1-cached guest loads with bit-exact floating-point parity.

namespace scan_fast {

inline uint8_t FastLoadU8(uint8_t* base, uint32_t address) {
  return *GuestPointer(base, address);
}

inline uint16_t FastLoadU16(uint8_t* base, uint32_t address) {
  uint16_t raw;
  std::memcpy(&raw, GuestPointer(base, address), sizeof(raw));
  return __builtin_bswap16(raw);
}

inline uint32_t FastLoadU32(uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, GuestPointer(base, address), sizeof(raw));
  return __builtin_bswap32(raw);
}

inline float FastLoadF32(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(FastLoadU32(base, address));
}

inline void FastStoreU8(uint8_t* base, uint32_t address, uint8_t value) {
  *GuestPointer(base, address) = value;
}

inline void FastStoreU16(uint8_t* base, uint32_t address, uint16_t value) {
  const uint16_t raw = __builtin_bswap16(value);
  std::memcpy(GuestPointer(base, address), &raw, sizeof(raw));
}

inline void FastStoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  const uint32_t raw = __builtin_bswap32(value);
  std::memcpy(GuestPointer(base, address), &raw, sizeof(raw));
}

inline void FastStoreF32(uint8_t* base, uint32_t address, float value) {
  FastStoreU32(base, address, std::bit_cast<uint32_t>(value));
}

struct RdataConsts {
  float f26;  // 0x820BBDC4
  float f24;  // 0x82000DC8
  float f16;  // 0x82000D74
  float f27;  // 0x82000D48
  float f20;  // 0x82019670
  float f21;  // 0x8200251C
  float f22;  // 0x82002518
  float f17;  // 0x82000A34
  float f18;  // 0x820BBCD8
  float f28;  // 0x82000E0C
  float f19;  // 0x82002510
};

inline RdataConsts LoadRdataConsts(uint8_t* base) {
  return RdataConsts{
      .f26 = FastLoadF32(base, 0x820BBDC4u),
      .f24 = FastLoadF32(base, 0x82000DC8u),
      .f16 = FastLoadF32(base, 0x82000D74u),
      .f27 = FastLoadF32(base, 0x82000D48u),
      .f20 = FastLoadF32(base, 0x82019670u),
      .f21 = FastLoadF32(base, 0x8200251Cu),
      .f22 = FastLoadF32(base, 0x82002518u),
      .f17 = FastLoadF32(base, 0x82000A34u),
      .f18 = FastLoadF32(base, 0x820BBCD8u),
      .f28 = FastLoadF32(base, 0x82000E0Cu),
      .f19 = FastLoadF32(base, 0x82002510u),
  };
}

// Inlined sub_823B32E0: clears bit 0x4 in sel+92 and returns true if already faded out (>= 239).
__attribute__((always_inline)) inline bool IsParentEntityFadedOut(
    uint8_t* base, uint32_t parent_ent, bool lod_fade_enabled) {
  if (!lod_fade_enabled) {
    return false;
  }
  auto is_valid_parent = [&](uint32_t ptr) -> bool {
    return ptr != 0 &&
           (FastLoadU8(base, ptr + 36) & 1u) != 0 &&
           (FastLoadU32(base, ptr + 40) & 0x40u) == 0;
  };
  uint32_t sel = 0;
  if (is_valid_parent(parent_ent)) {
    sel = parent_ent;
  } else if (parent_ent != 0) {
    const uint32_t grand_parent = FastLoadU32(base, parent_ent + 76);
    if (is_valid_parent(grand_parent)) {
      sel = grand_parent;
    }
  }
  if (sel == 0) {
    return false;
  }
  const int16_t model_idx = static_cast<int16_t>(FastLoadU16(base, sel + 46));
  const uint32_t model_info = FastLoadU32(base, 0x82B3A1A0u + uint32_t(int32_t(model_idx) * 4));
  if ((FastLoadU32(base, model_info + 64) & 0x8u) == 0) {
    return false;
  }
  const uint16_t f92 = FastLoadU16(base, sel + 92) & 65531u;
  FastStoreU16(base, sel + 92, f92);
  return FastLoadU8(base, sel + 99) >= 239;
}

#if defined(__aarch64__) || defined(_M_ARM64)
__attribute__((always_inline)) inline float CalcDist3dNEON(
    uint8_t* base, uint32_t pos_addr, float32x4_t cam_xyz) {
  const uint32_t aligned = pos_addr & ~0xFu;
  const float32x4_t pos_xyz =
      vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(GuestPointer(base, aligned))));
  const float32x4_t diff = vsubq_f32(pos_xyz, cam_xyz);
  const float dx = vgetq_lane_f32(diff, 0);
  const float dy = vgetq_lane_f32(diff, 1);
  const float dz = vgetq_lane_f32(diff, 2);
  return std::sqrt(std::fma(dx, dx, std::fma(dy, dy, dz * dz)));
}
#endif

}  // namespace scan_fast

extern "C" void sub_821D6260(PPCContext& ctx, uint8_t* base) {
  if (!IsNativeMode()) {
    __imp__sub_821D6260(ctx, base);
    return;
  }
  const float native_remap_limit = gta4::draw_distance::ResolveRemapLimit(
      scan_fast::FastLoadF32(base, kDistanceScaleInputGlobal));
  REX_FUNC_PROLOGUE();

  // Preserve guest stack pointer and LR for indirect virtual calls; local variables
  // live in host registers/stack so PPC non-volatile r14..r31 / f16..f31 remain untouched.
  const uint64_t saved_lr = ctx.lr;
  const uint64_t saved_r1 = ctx.r1.u64;
  const uint32_t frame_r1 = static_cast<uint32_t>(saved_r1) - 464u;
  scan_fast::FastStoreU32(base, frame_r1, static_cast<uint32_t>(saved_r1));
  ctx.r1.u32 = frame_r1;
  auto restore_and_return = [&]() {
    ctx.r1.u64 = saved_r1;
    ctx.lr = saved_lr;
  };

  uint32_t r27 = ctx.r3.u32;
  const uint32_t r15 = ctx.r4.u32;
  const uint32_t r25 = ctx.r5.u32;

  // Inlined sub_821D0478 (reads byte at 0x82D515A1).
  const uint8_t dflag = scan_fast::FastLoadU8(base, 0x82D515A1u);
  ctx.fpscr.disableFlushMode();
  const float f23 = scan_fast::FastLoadF32(base, r25 + 2336u);
  constexpr uint32_t kCounterAddr = 0x82B85390u;
  scan_fast::FastStoreU32(base, kCounterAddr, scan_fast::FastLoadU32(base, kCounterAddr) + 1u);
  const bool enable_lod_mark = (dflag == 0);

  if (r27 >= r15) {
    restore_and_return();
    return;
  }

  static const scan_fast::RdataConsts kConsts = scan_fast::LoadRdataConsts(base);
  const float f26 = kConsts.f26;
  const float f24 = kConsts.f24;
  const float f16 = kConsts.f16;
  const float f27 = kConsts.f27;
  const float f20 = kConsts.f20;
  const float f21 = kConsts.f21;
  const float f22 = kConsts.f22;
  const float f17 = kConsts.f17;
  const float f18 = kConsts.f18;
  const float f28 = kConsts.f28;
  const float f25 = native_remap_limit;
  const float f19 = kConsts.f19;

  // Hoist per-scan invariants.
  const bool lod_fade_enabled = (scan_fast::FastLoadU32(base, 0x82D928E0u) != 0);
  const float mult2340 = scan_fast::FastLoadF32(base, r25 + 2340u);
  const float mult12708_plus_f28 = scan_fast::FastLoadF32(base, 0x82A90000u + 12708u) + f28;
  const bool check_parent_flag =
      (scan_fast::FastLoadU8(base, 0x83030000u + 10579u) != 0) &&
      (static_cast<int32_t>(scan_fast::FastLoadU32(base, 0x82AB0000u - 30228u)) > 0);
  const uint8_t skip_alpha = scan_fast::FastLoadU8(base, 0x82BA0000u - 31646u);

  const uint32_t cam_ea = (r25 + 2304u) & ~0xFu;
#if defined(__aarch64__) || defined(_M_ARM64)
  const float32x4_t cam_xyz =
      vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(GuestPointer(base, cam_ea))));
  auto calc_dist3d = [&](uint32_t pos_addr) -> float {
    return scan_fast::CalcDist3dNEON(base, pos_addr, cam_xyz);
  };
#else
  const float cam_x = scan_fast::FastLoadF32(base, cam_ea + 0);
  const float cam_y = scan_fast::FastLoadF32(base, cam_ea + 4);
  const float cam_z = scan_fast::FastLoadF32(base, cam_ea + 8);
  auto calc_dist3d = [&](uint32_t pos_addr) -> float {
    const uint32_t aligned = pos_addr & ~0xFu;
    const float dx = scan_fast::FastLoadF32(base, aligned + 0) - cam_x;
    const float dy = scan_fast::FastLoadF32(base, aligned + 4) - cam_y;
    const float dz = scan_fast::FastLoadF32(base, aligned + 8) - cam_z;
    return std::sqrt(std::fma(dx, dx, std::fma(dy, dy, dz * dz)));
  };
#endif

  auto float_to_alpha_byte = [](float val) -> uint8_t {
    const int32_t iv = std::isnan(val)
                           ? int32_t(0x80000000u)
                           : (val > float(INT_MAX) ? INT_MAX : int32_t(val));
    return uint8_t(iv);
  };
  auto clear_parent_fade = [&](uint32_t parent) {
    if (parent != 0) {
      const uint32_t f40 = scan_fast::FastLoadU32(base, parent + 40);
      if ((f40 >> 12) & 0x1u) scan_fast::FastStoreU8(base, parent + 99, 255);
      scan_fast::FastStoreU32(base, parent + 40, f40 & ~0x1000u);
    }
  };

  constexpr uint32_t kModelInfoTable = 0x82B3A1A0u;
  constexpr uint32_t kSpecialEntityMask = 12582912u;

  for (; r27 < r15; r27 += 8) {
    const uint32_t r31 = scan_fast::FastLoadU32(base, r27);
    if (r31 == 0) continue;

    const int16_t model_idx = static_cast<int16_t>(scan_fast::FastLoadU16(base, r31 + 46));
    const uint32_t r30 = scan_fast::FastLoadU32(base, r31 + 76);
    const uint32_t r29 =
        scan_fast::FastLoadU32(base, kModelInfoTable + uint32_t(int32_t(model_idx) * 4));

    if (scan_fast::IsParentEntityFadedOut(base, r30, lod_fade_enabled)) {
      scan_fast::FastStoreU32(base, r27, 0);
      const uint16_t flags92 = scan_fast::FastLoadU16(base, r31 + 92) & 65531u;
      scan_fast::FastStoreU8(base, r31 + 99, 0);
      scan_fast::FastStoreU16(base, r31 + 92, flags92);
      continue;
    }

    if (check_parent_flag && r30 != 0 && (scan_fast::FastLoadU8(base, r30 + 36) & 0x1u) == 0) {
      scan_fast::FastStoreU32(base, r27, 0);
      continue;
    }

    const uint32_t flags36 = scan_fast::FastLoadU32(base, r31 + 36);
    const uint32_t flags40_31 = scan_fast::FastLoadU32(base, r31 + 40);
    if ((flags36 & 0x4000000u) == 0 || (flags40_31 & 0x100u) != 0) {
      scan_fast::FastStoreU32(base, r27, 0);
      continue;
    }

    ctx.r3.u64 = r29;
    ctx.ctr.u64 = scan_fast::FastLoadU32(base, scan_fast::FastLoadU32(base, r29) + 12);
    ctx.lr = 0x821D63E8;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

    const uint32_t type_id = ctx.r3.u32 & 0xFFu;
    if (type_id == 5) {
      if ((flags36 & 0x80000u) == 0) continue;
      const uint32_t p32 = scan_fast::FastLoadU32(base, r31 + 32);
      scan_fast::FastStoreF32(base, r27 + 4, calc_dist3d((p32 != 0) ? (p32 + 48) : (r31 + 16)));
      continue;
    }
    if (type_id == 6) {
      if ((flags36 >> 6) & 0x1u) {
        const uint32_t p120 = scan_fast::FastLoadU32(base, r31 + 120);
        if (p120 == 0 || (scan_fast::FastLoadU8(base, p120) & 0x40u) == 0) {
          scan_fast::FastStoreU32(base, r27, 0);
          continue;
        }
      }
      if ((flags36 & 0x80000u) == 0) continue;
      const uint32_t p32 = scan_fast::FastLoadU32(base, r31 + 32);
      float cur_dist = calc_dist3d((p32 != 0) ? (p32 + 48) : (r31 + 16));
      scan_fast::FastStoreF32(base, r27 + 4, cur_dist);

      if ((flags40_31 & 0x3C00000u) == kSpecialEntityMask &&
          (scan_fast::FastLoadU32(base, r31 + 572) & 0x20000000u) != 0) {
        const uint32_t sub_obj = scan_fast::FastLoadU32(base, r31 + 2688);
        if (sub_obj != 0) {
          ctx.r3.u64 = sub_obj;
          ctx.ctr.u64 = scan_fast::FastLoadU32(base, scan_fast::FastLoadU32(base, sub_obj) + 84);
          ctx.lr = 0x821D688C;
          REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
          cur_dist = static_cast<float>(ctx.f1.f64 + static_cast<double>(cur_dist));
          scan_fast::FastStoreF32(base, r27 + 4, cur_dist);
        }
      }

      ctx.r3.u64 = r31;
      ctx.ctr.u64 = scan_fast::FastLoadU32(base, scan_fast::FastLoadU32(base, r31) + 36);
      ctx.lr = 0x821D68AC;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      if ((ctx.r3.u32 & 0xFFu) == 0) continue;

      ctx.r3.u64 = r31;
      ctx.lr = 0x821D68C0;
      sub_821E5278(ctx, base);
      if (ctx.r3.u32 == 0) continue;

      ctx.ctr.u64 = scan_fast::FastLoadU32(base, scan_fast::FastLoadU32(base, ctx.r3.u32) + 84);
      ctx.lr = 0x821D68D8;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      const float latest = scan_fast::FastLoadF32(base, r27 + 4);
      scan_fast::FastStoreF32(base, r27 + 4, static_cast<float>(ctx.f1.f64 + static_cast<double>(latest)));
      continue;
    }

    const uint32_t p32_31 = scan_fast::FastLoadU32(base, r31 + 32);
    const uint32_t pos_ptr_31 = (p32_31 != 0) ? (p32_31 + 48) : (r31 + 16);
    uint32_t pos_ptr = pos_ptr_31;
    if (r30 != 0) {
      const uint32_t p32_30 = scan_fast::FastLoadU32(base, r30 + 32);
      pos_ptr = (p32_30 != 0) ? (p32_30 + 48) : (r30 + 16);
    }

    const float lod_dist_raw = scan_fast::FastLoadF32(base, r31 + 80);
    float f29 = lod_dist_raw;
    const uint32_t r10_bit = (flags36 >> 23) & 0x1u;
    float f30 = calc_dist3d(pos_ptr);

    if (r10_bit != 0 && f23 > f19) {
      f29 = f23;
    }

    const float f31 = mult2340 * f29;

    if ((flags36 & 0x2000000u) == 0) {
      if (f30 > f25 && f31 > f25 && f30 < (f31 + f28)) {
        f30 = (f31 - f25) + f30;
      }
    }

    float sort_dist = (r10_bit != 0) ? f18 : f17;
    sort_dist += (r30 != 0) ? calc_dist3d(pos_ptr_31) : f30;
    scan_fast::FastStoreF32(base, r27 + 4, sort_dist);

    ctx.r3.u64 = r31;
    ctx.ctr.u64 = scan_fast::FastLoadU32(base, scan_fast::FastLoadU32(base, r31) + 84);
    ctx.lr = 0x821D6548;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

    float f0 = static_cast<float>(ctx.f1.f64 + static_cast<double>(f23));
    float f13 = f28;
    if (f31 < f0) f0 = f31;

    if (r30 == 0) {
      const float f12 = (f29 >= f0) ? f0 : f29;
      if (f12 > f22) {
        f13 = std::fma(f12, f21, f20);
      }
      if ((flags36 & 0x2000000u) != 0) {
        f0 = scan_fast::FastLoadF32(base, 0x82A90000u + 12728u) * f0;
      }
    }

    const uint32_t r10_flag = (scan_fast::FastLoadU32(base, r29 + 64) >> 3) & 0x1u;
    if (r10_flag != 0) {
      if (((f13 + f30) - f28) < f31) {
        clear_parent_fade(r30);
        continue;
      }
      if ((f30 - f28) < f31) {
        f0 = ((f0 + f28) - f30) / f13;
        if (f0 >= f27) f0 = f27;

        const bool can_write_31_alpha = (((flags40_31 >> 12) & 0x1u) == 0) && (skip_alpha == 0);
        if (r30 != 0) {
          scan_fast::FastStoreU32(base, r30 + 40, scan_fast::FastLoadU32(base, r30 + 40) | 4096u);
          if (f0 < f16) {
            f0 = f0 * f24;
          } else {
            if (skip_alpha == 0) {
              scan_fast::FastStoreU8(base, r30 + 99, float_to_alpha_byte(((f27 - f0) * f24) * f26));
            }
            f0 = f27;
          }
          f0 = f0 * f26;
          if (!can_write_31_alpha) continue;
          const float cur99 = float(scan_fast::FastLoadU8(base, r31 + 99));
          if (f0 >= cur99) f0 = cur99;
          scan_fast::FastStoreU8(base, r31 + 99, float_to_alpha_byte(f0));
          continue;
        } else {
          const float cur99 = float(scan_fast::FastLoadU8(base, r31 + 99));
          f0 = f0 * f26;
          if (!can_write_31_alpha) continue;
          if (f0 >= cur99) f0 = cur99;
          scan_fast::FastStoreU8(base, r31 + 99, float_to_alpha_byte(f0));
          continue;
        }
      }
    }

    clear_parent_fade(r30);
    if ((f30 - mult12708_plus_f28) < f0) {
      if (r10_flag != 0 || !enable_lod_mark) {
        scan_fast::FastStoreU32(base, r27, 0);
        continue;
      }
      const float lim = lod_dist_raw * mult2340;
      if ((f30 - f28) < lim) continue;
      scan_fast::FastStoreU32(base, r31 + 8, scan_fast::FastLoadU32(base, r31 + 8) | 0x40000000u);
      scan_fast::FastStoreU32(base, r31 + 12, scan_fast::FastLoadU32(base, r31 + 12) | 0x40000000u);
      continue;
    }
    scan_fast::FastStoreU32(base, r27, 0);
  }

  restore_and_return();
}

extern "C" void sub_821D8CD8(PPCContext& ctx, uint8_t* base) {
  if (!IsNativeMode()) {
    __imp__sub_821D8CD8(ctx, base);
    return;
  }
  const float native_remap_limit = gta4::draw_distance::ResolveRemapLimit(
      LoadF32(base, kDistanceScaleInputGlobal));
	REX_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// bl 0x829ff7c4
	ctx.lr = 0x821D8CE0;
	__savegprlr_27(ctx, base);
	// stwu r1,-144(r1)
	ea = -144 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// mr r29,r6
	ctx.r29.u64 = ctx.r6.u64;
	// mr r31,r3
	ctx.r31.u64 = ctx.r3.u64;
	// mr r28,r4
	ctx.r28.u64 = ctx.r4.u64;
	// mr r27,r5
	ctx.r27.u64 = ctx.r5.u64;
	// lwz r11,60(r29)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + 60);
	// cmpwi cr6,r11,2
	ctx.cr6.compare<int32_t>(ctx.r11.s32, 2, ctx.xer);
	// lwz r11,36(r31)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 36);
	// beq cr6,0x821d8d1c
	if (ctx.cr6.eq) goto loc_821D8D1C;
	// rlwinm r10,r11,0,16,16
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x8000;
	// cmplwi cr6,r10,32768
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 32768, ctx.xer);
	// bne cr6,0x821d8d34
	if (!ctx.cr6.eq) goto loc_821D8D34;
loc_821D8D10:
	// li r3,2
	ctx.r3.s64 = 2;
	// addi r1,r1,144
	ctx.r1.s64 = ctx.r1.s64 + 144;
	// b 0x829ff814
	__restgprlr_27(ctx, base);
	return;
loc_821D8D1C:
	// rlwinm r10,r11,0,17,17
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x4000;
	// cmplwi cr6,r10,16384
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 16384, ctx.xer);
	// bne cr6,0x821d8d34
	if (!ctx.cr6.eq) goto loc_821D8D34;
	// li r3,2
	ctx.r3.s64 = 2;
	// addi r1,r1,144
	ctx.r1.s64 = ctx.r1.s64 + 144;
	// b 0x829ff814
	__restgprlr_27(ctx, base);
	return;
loc_821D8D34:
	// rlwinm r11,r11,0,26,26
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x20;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x821d8d70
	if (ctx.cr6.eq) goto loc_821D8D70;
	// lwz r11,40(r31)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 40);
	// lis r10,256
	ctx.r10.s64 = 16777216;
	// rlwinm r11,r11,0,6,9
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x3C00000;
	// cmplw cr6,r11,r10
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, ctx.r10.u32, ctx.xer);
	// bne cr6,0x821d8d70
	if (!ctx.cr6.eq) goto loc_821D8D70;
	// lis r11,-32071
	ctx.r11.s64 = -2101805056;
	// lbz r11,24256(r11)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r11.u32 + 24256);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x821d8d70
	if (ctx.cr6.eq) goto loc_821D8D70;
loc_821D8D64:
	// li r3,1
	ctx.r3.s64 = 1;
	// addi r1,r1,144
	ctx.r1.s64 = ctx.r1.s64 + 144;
	// b 0x829ff814
	__restgprlr_27(ctx, base);
	return;
loc_821D8D70:
	// lhz r10,46(r31)
	ctx.r10.u64 = REX_LOAD_U16(ctx.r31.u32 + 46);
	// lis r11,-32076
	ctx.r11.s64 = -2102132736;
	// extsh r10,r10
	ctx.r10.s64 = ctx.r10.s16;
	// addi r11,r11,-24160
	ctx.r11.s64 = ctx.r11.s64 + -24160;
	// rlwinm r10,r10,2,0,29
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 2) & 0xFFFFFFFC;
	// lwzx r30,r10,r11
	ctx.r30.u64 = REX_LOAD_U32(ctx.r10.u32 + ctx.r11.u32);
	// mr r3,r30
	ctx.r3.u64 = ctx.r30.u64;
	// lwz r11,0(r30)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 0);
	// lwz r11,12(r11)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 12);
	// mtctr r11
	ctx.ctr.u64 = ctx.r11.u64;
	// bctrl 
	ctx.lr = 0x821D8D9C;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// clrlwi r11,r3,24
	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
	// cmpwi cr6,r11,3
	ctx.cr6.compare<int32_t>(ctx.r11.s32, 3, ctx.xer);
	// beq cr6,0x821d8e70
	if (ctx.cr6.eq) goto loc_821D8E70;
	// addi r11,r11,-5
	ctx.r11.s64 = ctx.r11.s64 + -5;
	// cmplwi cr6,r11,1
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 1, ctx.xer);
	// bgt cr6,0x821d8e90
	if (ctx.cr6.gt) goto loc_821D8E90;
	// lwz r11,36(r31)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 36);
	// rlwinm r11,r11,0,5,5
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x4000000;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x821d8e64
	if (ctx.cr6.eq) goto loc_821D8E64;
	// li r5,0
	ctx.r5.s64 = 0;
	// mr r4,r29
	ctx.r4.u64 = ctx.r29.u64;
	// mr r3,r31
	ctx.r3.u64 = ctx.r31.u64;
	// bl 0x821d6dd0
	ctx.lr = 0x821D8DD4;
	sub_821D6DD0(ctx, base);
	// clrlwi r11,r3,24
	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// bne cr6,0x821d8d10
	if (!ctx.cr6.eq) goto loc_821D8D10;
	// mr r4,r29
	ctx.r4.u64 = ctx.r29.u64;
	// mr r3,r31
	ctx.r3.u64 = ctx.r31.u64;
	// bl 0x822a2aa8
	ctx.lr = 0x821D8DEC;
	sub_822A2AA8(ctx, base);
	// clrlwi r11,r3,24
	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// bne cr6,0x821d8d10
	if (!ctx.cr6.eq) goto loc_821D8D10;
	// lwz r11,36(r31)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 36);
	// rlwinm r11,r11,0,12,12
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x80000;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x821d8d64
	if (ctx.cr6.eq) goto loc_821D8D64;
	// li r10,1
	ctx.r10.s64 = 1;
	// lwz r11,32(r31)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 32);
	// li r9,2304
	ctx.r9.s64 = 2304;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// addi r11,r11,48
	ctx.r11.s64 = ctx.r11.s64 + 48;
	// lvx128 v0,r29,r9
	ea = (ctx.r29.u32 + ctx.r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// slw r5,r10,r27
	ctx.r5.u64 = ctx.r27.u8 & 0x20 ? 0 : (ctx.r10.u32 << (ctx.r27.u8 & 0x3F));
	// bne cr6,0x821d8e2c
	if (!ctx.cr6.eq) goto loc_821D8E2C;
	// addi r11,r31,16
	ctx.r11.s64 = ctx.r31.s64 + 16;
loc_821D8E2C:
	// lvx128 v13,r0,r11
	ea = (ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// addi r11,r1,80
	ctx.r11.s64 = ctx.r1.s64 + 80;
	// vsubfp v0,v13,v0
	ctx.fpscr.enableFlushMode();
	simde_mm_store_ps(ctx.v0.f32, simde_mm_sub_ps(simde_mm_load_ps(ctx.v13.f32), simde_mm_load_ps(ctx.v0.f32)));
	// li r6,0
	ctx.r6.s64 = 0;
	// mr r3,r31
	ctx.r3.u64 = ctx.r31.u64;
	// stvx128 v0,r0,r11
	ea = (ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lfs f13,84(r1)
	ctx.fpscr.disableFlushModeUnconditional();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 84);
	ctx.f13.f64 = double(temp.f32);
	// fmuls f12,f13,f13
	ctx.f12.f64 = double(float(ctx.f13.f64 * ctx.f13.f64));
	// lfs f0,88(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 88);
	ctx.f0.f64 = double(temp.f32);
	// lfs f13,80(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 80);
	ctx.f13.f64 = double(temp.f32);
	// fmadds f0,f0,f0,f12
	ctx.f0.f64 = double(float(std::fma(ctx.f0.f64, ctx.f0.f64, ctx.f12.f64)));
	// fmadds f0,f13,f13,f0
	ctx.f0.f64 = double(float(std::fma(ctx.f13.f64, ctx.f13.f64, ctx.f0.f64)));
	// fsqrts f1,f0
	ctx.f1.f64 = double(float(sqrt(ctx.f0.f64)));
	// bl 0x821d7900
	ctx.lr = 0x821D8E64;
	sub_821D7900(ctx, base);
loc_821D8E64:
	// li r3,0
	ctx.r3.s64 = 0;
	// addi r1,r1,144
	ctx.r1.s64 = ctx.r1.s64 + 144;
	// b 0x829ff814
	__restgprlr_27(ctx, base);
	return;
loc_821D8E70:
	// li r6,0
	ctx.r6.s64 = 0;
	// li r5,1
	ctx.r5.s64 = 1;
	// li r4,0
	ctx.r4.s64 = 0;
	// mr r3,r31
	ctx.r3.u64 = ctx.r31.u64;
	// bl 0x821d8040
	ctx.lr = 0x821D8E84;
	sub_821D8040(ctx, base);
	// clrlwi r11,r3,24
	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// bne cr6,0x821d8e64
	if (!ctx.cr6.eq) goto loc_821D8E64;
loc_821D8E90:
	// lwz r11,76(r31)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 76);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x821d8eb8
	if (ctx.cr6.eq) goto loc_821D8EB8;
	// lwz r10,32(r11)
	ctx.r10.u64 = REX_LOAD_U32(ctx.r11.u32 + 32);
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// beq cr6,0x821d8eb0
	if (ctx.cr6.eq) goto loc_821D8EB0;
	// addi r11,r10,48
	ctx.r11.s64 = ctx.r10.s64 + 48;
	// b 0x821d8ecc
	goto loc_821D8ECC;
loc_821D8EB0:
	// addi r11,r11,16
	ctx.r11.s64 = ctx.r11.s64 + 16;
	// b 0x821d8ecc
	goto loc_821D8ECC;
loc_821D8EB8:
	// lwz r11,32(r31)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 32);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// addi r11,r11,48
	ctx.r11.s64 = ctx.r11.s64 + 48;
	// bne cr6,0x821d8ecc
	if (!ctx.cr6.eq) goto loc_821D8ECC;
	// addi r11,r31,16
	ctx.r11.s64 = ctx.r31.s64 + 16;
loc_821D8ECC:
	// lvx128 v0,r0,r11
	ea = (ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// addi r11,r1,80
	ctx.r11.s64 = ctx.r1.s64 + 80;
	// stvx128 v0,r0,r11
	ea = (ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// li r11,2304
	ctx.r11.s64 = 2304;
	// lvx128 v13,r29,r11
	ea = (ctx.r29.u32 + ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// addi r11,r1,80
	ctx.r11.s64 = ctx.r1.s64 + 80;
	// vsubfp v0,v0,v13
	ctx.fpscr.enableFlushMode();
	simde_mm_store_ps(ctx.v0.f32, simde_mm_sub_ps(simde_mm_load_ps(ctx.v0.f32), simde_mm_load_ps(ctx.v13.f32)));
	// stvx128 v0,r0,r11
	ea = (ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// lfs f13,88(r1)
	ctx.fpscr.disableFlushModeUnconditional();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 88);
	ctx.f13.f64 = double(temp.f32);
	// fmuls f12,f13,f13
	ctx.f12.f64 = double(float(ctx.f13.f64 * ctx.f13.f64));
	// lfs f0,84(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 84);
	ctx.f0.f64 = double(temp.f32);
	// lfs f13,80(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 80);
	ctx.f13.f64 = double(temp.f32);
	// lfs f11,9492(r11)
	temp.u32 = std::bit_cast<uint32_t>(native_remap_limit);
	ctx.f11.f64 = double(temp.f32);
	// fmadds f0,f0,f0,f12
	ctx.f0.f64 = double(float(std::fma(ctx.f0.f64, ctx.f0.f64, ctx.f12.f64)));
	// fmadds f0,f13,f13,f0
	ctx.f0.f64 = double(float(std::fma(ctx.f13.f64, ctx.f13.f64, ctx.f0.f64)));
	// fsqrts f0,f0
	ctx.f0.f64 = double(float(sqrt(ctx.f0.f64)));
	// stfs f0,0(r28)
	temp.f32 = float(ctx.f0.f64);
	REX_STORE_U32(ctx.r28.u32 + 0, temp.u32);
	// fcmpu cr6,f0,f11
	ctx.cr6.compare(ctx.f0.f64, ctx.f11.f64);
	// ble cr6,0x821d8f50
	if (!ctx.cr6.gt) goto loc_821D8F50;
	// lfs f13,2340(r29)
	temp.u32 = REX_LOAD_U32(ctx.r29.u32 + 2340);
	ctx.f13.f64 = double(temp.f32);
	// lfs f12,80(r31)
	temp.u32 = REX_LOAD_U32(ctx.r31.u32 + 80);
	ctx.f12.f64 = double(temp.f32);
	// fmuls f10,f12,f13
	ctx.f10.f64 = double(float(ctx.f12.f64 * ctx.f13.f64));
	// fcmpu cr6,f10,f11
	ctx.cr6.compare(ctx.f10.f64, ctx.f11.f64);
	// ble cr6,0x821d8f50
	if (!ctx.cr6.gt) goto loc_821D8F50;
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// lfs f10,3596(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 3596);
	ctx.f10.f64 = double(temp.f32);
	// fmadds f10,f12,f13,f10
	ctx.f10.f64 = double(float(std::fma(ctx.f12.f64, ctx.f13.f64, ctx.f10.f64)));
	// fcmpu cr6,f0,f10
	ctx.cr6.compare(ctx.f0.f64, ctx.f10.f64);
	// bge cr6,0x821d8f50
	if (!ctx.cr6.lt) goto loc_821D8F50;
	// fmsubs f13,f12,f13,f11
	ctx.f13.f64 = double(float(std::fma(ctx.f12.f64, ctx.f13.f64, -ctx.f11.f64)));
	// fadds f0,f13,f0
	ctx.f0.f64 = double(float(ctx.f13.f64 + ctx.f0.f64));
	// stfs f0,0(r28)
	temp.f32 = float(ctx.f0.f64);
	REX_STORE_U32(ctx.r28.u32 + 0, temp.u32);
loc_821D8F50:
	// mr r8,r29
	ctx.r8.u64 = ctx.r29.u64;
	// lfs f1,0(r28)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r28.u32 + 0);
	ctx.f1.f64 = double(temp.f32);
	// mr r7,r27
	ctx.r7.u64 = ctx.r27.u64;
	// li r6,1
	ctx.r6.s64 = 1;
	// mr r4,r30
	ctx.r4.u64 = ctx.r30.u64;
	// mr r3,r31
	ctx.r3.u64 = ctx.r31.u64;
	// bl 0x821d8848
	ctx.lr = 0x821D8F6C;
	sub_821D8848(ctx, base);
	// addi r1,r1,144
	ctx.r1.s64 = ctx.r1.s64 + 144;
	// b 0x829ff814
	__restgprlr_27(ctx, base);
	return;
}
