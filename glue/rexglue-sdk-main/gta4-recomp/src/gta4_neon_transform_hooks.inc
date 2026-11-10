// ARM64 NEON Vector Transform Hooks for GTA IV (Rockstar RAGE engine)
// Accelerates sub_829046A0 and sub_82904740 (Top hot matrix-vector math in RageStream).
#pragma once

#if defined(__aarch64__)
#include <arm_neon.h>

namespace gta4::neon_transform {

static inline float32x4_t LoadBigEndianFloat4(const void* ptr) {
  uint8x16_t raw = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));
  return vreinterpretq_f32_u8(vrev32q_u8(raw));
}

static inline void StoreBigEndianFloat4(void* ptr, float32x4_t vec) {
  uint8x16_t raw = vrev32q_u8(vreinterpretq_u8_f32(vec));
  vst1q_u8(reinterpret_cast<uint8_t*>(ptr), raw);
}

static inline void ExecuteVectorMatrixTransform(PPCContext& ctx, uint8_t* base, int matrix_offset_delta) {
  // 1. Calculate base matrix address
  const uint32_t r13_0 = REX_LOAD_U32(ctx.r13.u32 + 0);
  const uint32_t r10_val = REX_LOAD_U32(1712 + r13_0);
  const uint64_t r8_val = ctx.r6.s64 + matrix_offset_delta;
  const uint32_t r11_shift1 = static_cast<uint32_t>((r8_val << 2) & 0xFFFFFFFC);
  const uint32_t r11_sum = r11_shift1 + r10_val;
  const uint32_t r11_shift2 = static_cast<uint32_t>((static_cast<uint64_t>(r11_sum) << 6) & 0xFFFFFFC0);
  const uint32_t matrix_ea = r11_shift2 + ctx.r4.u32;

  // 2. Load scale factor
  const uint32_t scale_raw = REX_LOAD_U32(ctx.r4.u32 + 5908);
  const float scale_val = std::bit_cast<float>(scale_raw);

  // 3. Stack spill preservation (for caller sub_829134C8 compatibility)
  REX_STORE_U32(ctx.r1.u32 - 4, REX_LOAD_U32(-2113929216u + 3400));
  REX_STORE_U32(ctx.r1.u32 - 32, scale_raw);

  // 4. Load input vector and matrix rows with single-cycle REV32 endian conversion
  const uint32_t in_ea = ctx.r5.u32 & ~0xFu;
  const float32x4_t v_in = LoadBigEndianFloat4(REX_RAW_ADDR(in_ea));
  const float32x4_t row0 = LoadBigEndianFloat4(REX_RAW_ADDR(matrix_ea + 0));
  const float32x4_t row1 = LoadBigEndianFloat4(REX_RAW_ADDR(matrix_ea + 16));
  const float32x4_t row2 = LoadBigEndianFloat4(REX_RAW_ADDR(matrix_ea + 32));
  const float32x4_t row3 = LoadBigEndianFloat4(REX_RAW_ADDR(matrix_ea + 48));

  // Spill v_in to r1 - 16 as expected by guest stack convention
  StoreBigEndianFloat4(REX_RAW_ADDR((ctx.r1.u32 - 16) & ~0xFu), v_in);

  // 5. Native ARM64 Fused Multiply-Accumulate
  float32x4_t res = vmulq_laneq_f32(row0, v_in, 0);
  res = vfmaq_laneq_f32(res, row1, v_in, 1);
  res = vfmaq_laneq_f32(res, row2, v_in, 2);
  res = vfmaq_laneq_f32(res, row3, v_in, 3);
  res = vmulq_n_f32(res, scale_val);

  // 6. Store result to destination
  const uint32_t out_ea = ctx.r3.u32 & ~0xFu;
  StoreBigEndianFloat4(REX_RAW_ADDR(out_ea), res);

  // 7. Update caller-visible PPCContext registers
  ctx.r7.s64 = 16;
  ctx.r8.s64 = 48;
  ctx.r9.s64 = 32;
  ctx.r10.s64 = ctx.r1.s64 - 16;
  ctx.r11.u64 = matrix_ea;
  ctx.f0.f64 = static_cast<double>(scale_val);

  // Sync vector registers in ctx.
  // Note: PPCContext vector registers hold native host IEEE-754 floats (Little Endian on ARM64).
  // Writing them via StoreBigEndianFloat4 byte-swapped floats into NaNs/denormals, breaking 3D audio rolloff!
  vst1q_f32(ctx.v0.f32, res);
  vst1q_f32(ctx.v7.f32, row0);
  vst1q_f32(ctx.v8.f32, row1);
  vst1q_f32(ctx.v9.f32, row2);
  vst1q_f32(ctx.v10.f32, vdupq_laneq_f32(v_in, 1));
  vst1q_f32(ctx.v11.f32, vdupq_laneq_f32(v_in, 2));

  float32x4_t v12_acc = vmulq_laneq_f32(row3, v_in, 3);
  v12_acc = vfmaq_laneq_f32(v12_acc, row2, v_in, 2);
  v12_acc = vfmaq_laneq_f32(v12_acc, row1, v_in, 1);
  vst1q_f32(ctx.v12.f32, v12_acc);
  vst1q_f32(ctx.v13.f32, vdupq_n_f32(scale_val));
}

} // namespace gta4::neon_transform

extern "C" void sub_829046A0(PPCContext& ctx, uint8_t* base) {
  gta4::neon_transform::ExecuteVectorMatrixTransform(ctx, base, 10);
}

extern "C" void sub_82904740(PPCContext& ctx, uint8_t* base) {
  gta4::neon_transform::ExecuteVectorMatrixTransform(ctx, base, 15);
}

#else // Non-ARM64 fallback

extern "C" void sub_829046A0(PPCContext& ctx, uint8_t* base) {
  __imp__sub_829046A0(ctx, base);
}

extern "C" void sub_82904740(PPCContext& ctx, uint8_t* base) {
  __imp__sub_82904740(ctx, base);
}

#endif
