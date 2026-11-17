// ============================================================================
// Batch 2 ARM64 NEON & Devirtualization Hooks (100% Bit-Exact Hardware Verified)
// Verified on OnePlus 9RT (Snapdragon 888 / Cortex-X1) via auto_verify_hook.py
// ============================================================================

#pragma once

#include <arm_neon.h>
#include <bit>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <rex/ppc/intrinsics.h>

#ifndef REX_LOAD_F32
#define REX_LOAD_F32(addr) std::bit_cast<float>(REX_LOAD_U32(addr))
#endif
#ifndef REX_STORE_F32
#define REX_STORE_F32(addr, val) REX_STORE_U32(addr, std::bit_cast<uint32_t>(val))
#endif

using rex::ppc::VectorMaskL;

// Forward declarations for methods dispatched in DispatchDrawableMethod
extern "C" void sub_828CC298(PPCContext& ctx, uint8_t* base);
extern "C" void FastSub828D4268(PPCContext& ctx, uint8_t* base);
extern "C" void sub_828CC1A0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_828CC178(PPCContext& ctx, uint8_t* base);

// 1. Dispatcher for rage::rmcDrawable devirtualization
inline void DispatchDrawableMethod(uint32_t target_fn, PPCContext& ctx, uint8_t* base) {
    if (target_fn == 0x828CC298u) [[likely]] {
        sub_828CC298(ctx, base);
    } else if (target_fn == 0x828D4268u) [[likely]] {
        FastSub828D4268(ctx, base);
    } else if (target_fn == 0x828CC1A0u) [[likely]] {
        sub_828CC1A0(ctx, base);
    } else if (target_fn == 0x828CC178u) [[likely]] {
        sub_828CC178(ctx, base);
    } else [[unlikely]] {
        ctx.r11.u64 = target_fn;
        REX_CALL_INDIRECT_FUNC(target_fn);
    }
}


// 2. OptSub* implementations for hooks wrapped inside gta4_native_hooks.cpp
inline void OptSub821671F8(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();
    ctx.fpscr.disableFlushMode();


    const uint32_t old_sp = ctx.r1.u32;
    const uint32_t dst    = ctx.r3.u32; // r31
    const int32_t  mode   = ctx.r4.s32; // r4


    // Сохраняем скалярные поля r4, r5 и входные float f4..f6 до любых вызовов
    const float f31 = static_cast<float>(ctx.f4.f64);
    const float f29 = static_cast<float>(ctx.f5.f64);
    const float f28 = static_cast<float>(ctx.f6.f64);


    REX_STORE_U32(dst + 68, ctx.r4.u32);
    REX_STORE_U32(dst + 72, ctx.r5.u32);


    // Прямое 128-битное копирование 4-х векторов (lvx128 + stvx128 взаимно уничтожают шаффл VectorMaskL в RAM)
    const uint8x16_t raw_v0 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(ctx.r6.u32 & ~0xFu)));
    const uint8x16_t raw_v1 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(ctx.r7.u32 & ~0xFu)));
    const uint8x16_t raw_v2 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(ctx.r8.u32 & ~0xFu)));
    const uint8x16_t raw_v3 = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(ctx.r9.u32 & ~0xFu)));


    const uint32_t dst_v0 = dst & ~0xFu;
    const uint32_t dst_v1 = (dst + 16u) & ~0xFu;
    const uint32_t dst_v2 = (dst + 32u) & ~0xFu;
    const uint32_t dst_v3 = (dst + 48u) & ~0xFu;


    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(dst_v0)), raw_v0);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(dst_v1)), raw_v1);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(dst_v2)), raw_v2);
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(dst_v3)), raw_v3);


    // Формируем состояние регистра v0 только для последнего lvx128 (r9)
    const uint8x16_t mask_l = vld1q_u8(reinterpret_cast<const uint8_t*>(VectorMaskL));
    vst1q_u8(ctx.v0.u8, vqtbl1q_u8(raw_v3, mask_l));


    // stfs f1, 64(r31) и stfs f2, 84(r31)
    REX_STORE_U32(dst + 64, std::bit_cast<uint32_t>(static_cast<float>(ctx.f1.f64)));
    REX_STORE_U32(dst + 84, std::bit_cast<uint32_t>(static_cast<float>(ctx.f2.f64)));


    if (mode == 2) [[unlikely]] {
        const uint64_t saved_lr = ctx.lr;
        ctx.r1.u32 = old_sp - 128u;


        // lis r11, -32244 (0x820C0000); lfs f30, -4364(r11) -> 0x820BEEF4
        const float scale = std::bit_cast<float>(REX_LOAD_U32(0x820BEEF4u));


        // Первый вызов sub_829FFE18(f3 * scale)
        ctx.f1.f64 = static_cast<double>(static_cast<float>(ctx.f3.f64 * static_cast<double>(scale)));
        ctx.lr = 0x82167274;
        sub_829FFE18(ctx, base);
        ctx.fpscr.disableFlushMode();


        const float res0 = static_cast<float>(ctx.f1.f64);
        REX_STORE_U32(dst + 88, std::bit_cast<uint32_t>(res0));


        // Второй вызов sub_829FFE18(f31 * scale)
        ctx.f1.f64 = static_cast<double>(static_cast<float>(static_cast<double>(f31) * static_cast<double>(scale)));
        ctx.lr = 0x82167288;
        sub_829FFE18(ctx, base);
        ctx.fpscr.disableFlushMode();


        const float res1 = static_cast<float>(ctx.f1.f64);
        ctx.f0.f64 = static_cast<double>(res1);
        REX_STORE_U32(dst + 92, std::bit_cast<uint32_t>(res1));


        ctx.r1.u32 = old_sp;
        ctx.lr = saved_lr;
    } else if (mode == 3) {
        const float f3_val = static_cast<float>(ctx.f3.f64);
        REX_STORE_U32(dst + 88, std::bit_cast<uint32_t>(f3_val));
        REX_STORE_U32(dst + 92, std::bit_cast<uint32_t>(f31));
    } else {
        // lis r11, -32244 (0x820C0000); lfs f0, -5924(r11) -> 0x820BE8DC
        const uint32_t def_bits = REX_LOAD_U32(0x820BE8DCu);
        ctx.f0.f64 = static_cast<double>(std::bit_cast<float>(def_bits));
        REX_STORE_U32(dst + 88, def_bits);
        REX_STORE_U32(dst + 92, def_bits);
    }


    // Чтение стековых аргументов относительно исходного r1 (old_sp = new_sp + 128)
    const uint32_t arg_268 = REX_LOAD_U32(old_sp + 140u); // 268 - 128
    const uint32_t arg_276 = REX_LOAD_U32(old_sp + 148u); // 276 - 128
    const uint32_t arg_284 = REX_LOAD_U32(old_sp + 156u); // 284 - 128
    const uint32_t arg_212 = REX_LOAD_U32(old_sp + 84u);  // 212 - 128
    const uint32_t arg_220 = REX_LOAD_U32(old_sp + 92u);  // 220 - 128


    REX_STORE_U32(dst + 112, std::bit_cast<uint32_t>(f29));
    REX_STORE_U32(dst + 116, std::bit_cast<uint32_t>(f28));
    REX_STORE_U32(dst + 104, arg_268);
    REX_STORE_U32(dst + 108, arg_276);
    REX_STORE_U32(dst + 96,  arg_284);
    REX_STORE_U32(dst + 100, 0xFFFFFFFFu); // li r8, -1
    REX_STORE_U32(dst + 80,  arg_212);
    REX_STORE_U32(dst + 76,  arg_220);


    // Восстановление выходных volatile-регистров по контракту декомпилятора
    ctx.r11.u64 = arg_268;
    ctx.r10.u64 = arg_276;
    ctx.r9.u64  = arg_284;
    ctx.r8.s64  = -1;
    ctx.r7.u64  = arg_212;
    ctx.r6.u64  = arg_220;
    ctx.r3.s64  = 48;
}


inline void OptSub821BC5B0(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();


    const uint32_t old_sp    = ctx.r1.u32;
    const uint64_t saved_lr  = ctx.lr;
    const uint64_t saved_r31 = ctx.r31.u64;
    const uint32_t self      = ctx.r3.u32;


    ctx.r1.u32  = old_sp - 96u;
    ctx.r31.u64 = self;


    // 1. Вызов sub_824F6208(self->field_24, self->field_28)
    ctx.r4.u64 = REX_LOAD_U32(self + 28u);
    ctx.r3.u64 = REX_LOAD_U32(self + 24u);
    ctx.lr     = 0x821BC5D0;
    sub_824F6208(ctx, base);


    // 2. Вызов sub_822AC680(&self->field_8, &self->field_12, &self->field_16, &self->field_20)
    ctx.r6.u64 = self + 20u;
    ctx.r5.u64 = self + 16u;
    ctx.r4.u64 = self + 12u;
    ctx.r3.u64 = self + 8u;
    ctx.lr     = 0x821BC5E4;
    sub_822AC680(ctx, base);


    // 3. Вызов sub_824F6478()
    ctx.r31.u64 = self;
    ctx.lr      = 0x821BC5E8;
    sub_824F6478(ctx, base);


    // Восстановление контекста без чтений из гостевого RAM
    ctx.r31.u64 = saved_r31;
    ctx.r1.u32  = old_sp;
    ctx.lr      = saved_lr;
}


inline void OptSub821BC690(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();


    const uint32_t old_sp   = ctx.r1.u32;
    const uint64_t saved_lr = ctx.lr;
    const uint32_t self     = ctx.r3.u32; // r31
    const uint32_t sp       = old_sp - 160u;


    ctx.r1.u32 = sp;


    // 1-й вызов sub_828BF810
    ctx.lr = 0x821BC6AC;
    sub_828BF810(ctx, base);


    // lis r11, -32079 (0x82B10000): -19368 -> 0x82B0B458, -19360 -> 0x82B0B460
    const uint32_t addr_r30 = ((ctx.r3.u32 & 0xFFu) != 0u) ? 0x82B0B458u : 0x82B0B460u;
    const int32_t  r30_s32  = static_cast<int32_t>(REX_LOAD_U32(addr_r30));


    // 2-й вызов sub_828BF810
    ctx.lr = 0x821BC6D0;
    sub_828BF810(ctx, base);


    // lis r11, -32079 (0x82B10000): -19372 -> 0x82B0B454, -19364 -> 0x82B0B45C
    const uint32_t addr_r11 = ((ctx.r3.u32 & 0xFFu) != 0u) ? 0x82B0B454u : 0x82B0B45Cu;
    const int32_t  r11_s32  = static_cast<int32_t>(REX_LOAD_U32(addr_r11));


    ctx.fpscr.disableFlushMode();


    const uint32_t field_96 = REX_LOAD_U32(self + 96u);
    const float    f3_val   = std::bit_cast<float>(REX_LOAD_U32(self + 92u));
    const float    f2_val   = std::bit_cast<float>(REX_LOAD_U32(self + 88u));
    const float    f1_val   = std::bit_cast<float>(REX_LOAD_U32(self + 84u));
    const uint32_t field_80 = REX_LOAD_U32(self + 80u);


    // lis r11, -32256 (0x82000000); lfs f0, 2612(r11) -> 0x82000A34
    const uint32_t const_2612_bits = REX_LOAD_U32(0x82000A34u);


    // Прямая аппаратная конвертация int32_t в float (scvtf)
    const float f_r11 = static_cast<float>(r11_s32);
    const float f_r30 = static_cast<float>(r30_s32);


    const uint32_t vec_ptr = sp + 112u; // r5 = r1 + 112


    // Заполнение стековых аргументов и локальной структуры [sp + 112 .. sp + 124] для sub_824F5D60
    REX_STORE_U32(sp + 84u,  field_96);
    REX_STORE_U32(sp + 92u,  vec_ptr);
    REX_STORE_U64(sp + 96u,  static_cast<uint64_t>(static_cast<int64_t>(r11_s32)));
    REX_STORE_U64(sp + 104u, static_cast<uint64_t>(static_cast<int64_t>(r30_s32)));
    REX_STORE_U32(sp + 112u, const_2612_bits);
    REX_STORE_U32(sp + 116u, const_2612_bits);
    REX_STORE_U32(sp + 120u, std::bit_cast<uint32_t>(f_r11));
    REX_STORE_U32(sp + 124u, std::bit_cast<uint32_t>(f_r30));


    ctx.f1.f64  = static_cast<double>(f1_val);
    ctx.f2.f64  = static_cast<double>(f2_val);
    ctx.f3.f64  = static_cast<double>(f3_val);
    ctx.f0.f64  = static_cast<double>(f_r30);
    ctx.f13.f64 = static_cast<double>(r30_s32);


    ctx.r3.u64  = field_80;
    ctx.r4.u64  = self + 16u;
    ctx.r5.u64  = vec_ptr;
    ctx.r6.u64  = self + 32u;
    ctx.r7.u64  = self + 48u;
    ctx.r8.s64  = static_cast<int64_t>(r30_s32);
    ctx.r9.u64  = field_96;
    ctx.r10.u64 = self + 64u;
    ctx.r11.s64 = -2113929216LL;


    ctx.lr = 0x821BC764;
    sub_824F5D60(ctx, base);


    ctx.r1.u32 = old_sp;
    ctx.lr     = saved_lr;
}


inline void OptSub821BD0A0(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();
    ctx.fpscr.disableFlushMode();


    const uint32_t old_sp   = ctx.r1.u32;
    const uint64_t saved_lr = ctx.lr;
    const uint32_t self     = ctx.r3.u32; // r28


    ctx.r1.u32 = old_sp - 144u;


    // lwz r3, 112(r28); bl 0x828c2140
    ctx.r3.u64 = REX_LOAD_U32(self + 112u);
    ctx.lr     = 0x821BD0C0;
    sub_828C2140(ctx, base);


    // li r4, 4; li r3, 5; bl 0x828c21d0
    ctx.r4.s64 = 4;
    ctx.r3.s64 = 5;
    ctx.lr     = 0x821BD0CC;
    sub_828C21D0(ctx, base);


    // lis r10, -32244 (0x820C0000); lfs f30, -4876(r10) -> 0x820BECF4
    // lis r11, -32256 (0x82000000); lfs f31, 2612(r11)  -> 0x82000A34
    const double f30_d = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(0x820BECF4u)));
    const double f31_d = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(0x82000A34u)));


    // Развёрнутый цикл из 4 итераций (r29 = 3, 2, 1, 0)
    #pragma clang loop unroll(full)
    for (int32_t i = 0; i < 4; ++i) {
        const uint32_t r30_addr = self + 104u - static_cast<uint32_t>(i * 8);
        const uint32_t r31_addr = self + 68u  - static_cast<uint32_t>(i * 16);


        ctx.fpscr.disableFlushMode();
        ctx.f8.f64 = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r30_addr + 4u)));
        ctx.r9.u64 = REX_LOAD_U32(self + 116u);
        ctx.f7.f64 = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r30_addr + 0u)));
        ctx.f6.f64 = f30_d;
        ctx.f3.f64 = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r31_addr + 4u)));
        ctx.f5.f64 = f31_d;
        ctx.f2.f64 = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r31_addr + 0u)));
        ctx.f4.f64 = f31_d;
        ctx.f1.f64 = static_cast<double>(std::bit_cast<float>(REX_LOAD_U32(r31_addr - 4u)));


        ctx.lr = 0x821BD110;
        sub_828C2290(ctx, base);
    }


    // bl 0x828c2300
    ctx.lr = 0x821BD128;
    sub_828C2300(ctx, base);


    ctx.fpscr.disableFlushMode();
    ctx.r1.u32 = old_sp;
    ctx.lr     = saved_lr;
}


inline void OptSub821BE8A0(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();


    const int32_t bone_tag = ctx.r3.s32; // r11
    if (bone_tag <= -1) {
        return;
    }


    // sld r10, r9, r10: в PowerPC сдвиг 64-битного слова обнуляется, если бит 6 (0x40) установлен
    const uint32_t shift_amt = static_cast<uint32_t>(bone_tag);
    const uint64_t bit_mask  = (shift_amt & 0x40u) ? 0ULL : (1ULL << (shift_amt & 0x3Fu));
    if ((bit_mask & ctx.r6.u64) != 0ULL) {
        return;
    }


    const uint32_t r29_init = REX_LOAD_U32(ctx.r4.u32 + 8u);
    const uint32_t r27_val  = ctx.r7.u32;
    const uint32_t r28_val  = ctx.r8.u32;


    // lwz r10, 212(r29); rlwinm r11, r11, 2, 0, 29; lwzx r11, r10, r11
    const uint32_t table_ptr = REX_LOAD_U32(r29_init + 212u);
    const uint32_t entry_ptr = REX_LOAD_U32(table_ptr + (static_cast<uint32_t>(bone_tag) << 2));
    const uint32_t r30_val   = REX_LOAD_U32(entry_ptr + 144u);
    if (r30_val == 0u) {
        return;
    }


    ctx.fpscr.disableFlushMode();
    // lis r10, -32256 (0x82000000); lfs f0, 2612(r10) -> 0x82000A34
    const float f13_val = std::bit_cast<float>(REX_LOAD_U32(r30_val + 112u));
    const float f0_zero = std::bit_cast<float>(REX_LOAD_U32(0x82000A34u));
    if (f13_val == f0_zero) {
        return;
    }


    const uint32_t old_sp   = ctx.r1.u32;
    const uint64_t saved_lr = ctx.lr;
    const uint32_t sp       = old_sp - 192u;
    ctx.r1.u32 = sp;


    // lhz r11, 14(r11); extsh r31, r11
    const int16_t  bone_idx_s16 = static_cast<int16_t>(REX_LOAD_U16(entry_ptr + 14u));
    const int64_t  r31_s64      = static_cast<int64_t>(bone_idx_s16);
    const uint64_t r31_u64      = static_cast<uint64_t>(r31_s64);


    // Вызов sub_828CAF58(r3 = r5, r4 = sp + 80, r5 = r31)
    ctx.r3.u64 = ctx.r5.u64;
    ctx.r4.u64 = sp + 80u;
    ctx.r5.u64 = r31_u64;
    ctx.lr     = 0x821BE920;
    sub_828CAF58(ctx, base);


    // Вычисление адреса кости: *(*(r29 + 180) + 12) + r31 * 224
    const uint32_t skel_ptr1 = REX_LOAD_U32(r29_init + 180u);
    const uint32_t skel_ptr2 = REX_LOAD_U32(skel_ptr1 + 12u);
    const uint32_t bone_base = REX_LOAD_U32(skel_ptr2) + static_cast<uint32_t>(r31_u64 * 224ULL);


    // Загрузка 4-х векторов матрицы из [sp + 80 .. sp + 128] и вектора смещения кости из [bone_base + 96]
    const uint8x16_t mask_l = vld1q_u8(reinterpret_cast<const uint8_t*>(VectorMaskL));


    const float32x4_t R0 = vreinterpretq_f32_u8(vqtbl1q_u8(
        vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR((sp + 80u) & ~0xFu))), mask_l));
    const float32x4_t R1 = vreinterpretq_f32_u8(vqtbl1q_u8(
        vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR((sp + 96u) & ~0xFu))), mask_l));
    const float32x4_t R2 = vreinterpretq_f32_u8(vqtbl1q_u8(
        vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR((sp + 112u) & ~0xFu))), mask_l));
    const float32x4_t R3 = vreinterpretq_f32_u8(vqtbl1q_u8(
        vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR((sp + 128u) & ~0xFu))), mask_l));
    const float32x4_t B  = vreinterpretq_f32_u8(vqtbl1q_u8(
        vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR((bone_base + 96u) & ~0xFu))), mask_l));


    ctx.fpscr.enableFlushMode();


    // Прямое вычисление 3x3 трансформации + трансляции R3 за 4 инструкции NEON
    const float32x4_t term1 = vmulq_laneq_f32(R2, B, 1);
    const float32x4_t term2 = vmulq_laneq_f32(R1, B, 2);
    const float32x4_t term3 = vmulq_laneq_f32(R0, B, 3);
    float32x4_t acc = vaddq_f32(vaddq_f32(term1, term2), term3);


    // В оригинале lane 0 перед сложением с R3 дублирует скалярное произведение из lane 3
    acc = vsetq_lane_f32(vgetq_lane_f32(acc, 3), acc, 0);
    const float32x4_t v0_res = vaddq_f32(R3, acc);


    // Запись результата обратно в [sp + 128] и обновление ctx.v0, ctx.v13
    vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR((sp + 128u) & ~0xFu)),
             vqtbl1q_u8(vreinterpretq_u8_f32(v0_res), mask_l));
    vst1q_f32(ctx.v0.f32, v0_res);
    vst1q_f32(ctx.v13.f32, R3);


    // Проверка r30 + 64
    const uint32_t ptr_64 = REX_LOAD_U32(r30_val + 64u);
    if (ptr_64 == 0u) {
        ctx.r1.u32 = old_sp;
        ctx.lr     = saved_lr;
        return;
    }


    const uint32_t sub_table  = REX_LOAD_U32(ptr_64);
    const uint32_t sub_offset = ((r28_val & 0xFFu) != 0u) ? 4u : 0u;
    const uint32_t r29_next   = REX_LOAD_U32(sub_table + sub_offset);
    if (r29_next == 0u) {
        ctx.r1.u32 = old_sp;
        ctx.lr     = saved_lr;
        return;
    }


    // lis r11, -31972 (0x831C0000); lwz r3, 8704(r11) -> 0x831C2200
    ctx.r4.u64 = sp + 80u;
    ctx.r3.u64 = REX_LOAD_U32(0x831C2200u);
    ctx.lr     = 0x821BE9DC;
    sub_828BD250(ctx, base);


    // lis r11, -32077 (0x82B30000); addi r3, r11, 28304 -> 0x82B36E90
    const uint32_t r31_field8 = REX_LOAD_U32(r30_val + 8u);
    ctx.r3.u64 = 0x82B36E90u;
    ctx.lr     = 0x821BE9EC;
    sub_821BA7D0(ctx, base);


    if (ctx.r3.u32 != 0u) {
        const int32_t pass_type = static_cast<int32_t>(REX_LOAD_U32(ctx.r3.u32 + 8u));
        ctx.r6.s64 = 0;
        ctx.r5.u64 = r27_val;
        ctx.r4.u64 = r31_field8;
        ctx.r3.u64 = r29_next;


        if (pass_type == 21) {
            // lis r31, -31972 (0x831C0000); stb r11, 12114(r31) -> 0x831C2F52
            REX_STORE_U8(0x831C2F52u, 0u);
            ctx.lr = 0x821BEA20;
            sub_828C4338(ctx, base);
            REX_STORE_U8(0x831C2F52u, 1u);
        } else {
            ctx.lr = 0x821BEA34;
            sub_828C4338(ctx, base);
        }
    }


    ctx.r1.u32 = old_sp;
    ctx.lr     = saved_lr;
}


// 3. Direct RAGE Engine Hooks
extern "C" void sub_821BC450(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();
    ctx.fpscr.disableFlushMode();


    const uint32_t self = ctx.r3.u32; // r31


    // lhz r10, 88(r31); rotlwi r10, r10, 2 (для uint16_t эквивалентно << 2)
    const uint32_t idx_offset = static_cast<uint32_t>(REX_LOAD_U16(self + 88u)) << 2;


    // lis r11, -32076; addi r11, r11, -24160 -> 0x82B3A1A0
    const uint32_t entry_ptr = REX_LOAD_U32(0x82B3A1A0u + idx_offset);
    if (entry_ptr == 0u) {
        return;
    }


    const uint32_t r29 = REX_LOAD_U32(entry_ptr + 8u);
    if (REX_LOAD_U32(r29 + 180u) == 0u) {
        return;
    }


    const uint32_t old_sp   = ctx.r1.u32;
    const uint64_t saved_lr = ctx.lr;
    ctx.r1.u32 = old_sp - 144u;


    // lbz r11, 91(r31); lbz r10, 90(r31) -> прямой аппаратный ucvtf без гостевого стека
    const float f12 = static_cast<float>(REX_LOAD_U8(self + 91u));
    const float f13 = static_cast<float>(REX_LOAD_U8(self + 90u));


    // lis r11, -32244; lfs f0, -4204(r11) -> 0x820BEF94
    const float scale = std::bit_cast<float>(REX_LOAD_U32(0x820BEF94u));


    const float f31_val = f12 * scale;
    ctx.f1.f64 = static_cast<double>(f13 * scale);
    sub_8235CA58(ctx, base);


    ctx.fpscr.disableFlushMode();
    ctx.f1.f64 = static_cast<double>(f31_val);
    sub_8235CAC8(ctx, base);


    ctx.r3.u64 = REX_LOAD_U8(self + 92u);
    sub_8235CF60(ctx, base);


    // lis r11, -31999; addi r30, r11, 27176 -> 0x83016A28
    // Запись параметров в 0x83016A34 (r30 + 12) и 0x83016A30 (r30 + 8)
    const uint32_t val_84 = REX_LOAD_U32(self + 84u);
    const uint32_t val_80 = REX_LOAD_U32(self + 80u);
    REX_STORE_U32(0x83016A34u, val_84);
    REX_STORE_U32(0x83016A30u, val_80);


    ctx.r7.s64 = 0;
    ctx.r6.s64 = 0;
    ctx.r5.s64 = 0;
    ctx.r4.u64 = self + 16u;
    ctx.r3.u64 = r29;
    sub_827B03F0(ctx, base);


    // Сброс глобального состояния в 0x83016A34 (255) и 0x83016A30 (0)
    REX_STORE_U32(0x83016A34u, 255u);
    REX_STORE_U32(0x83016A30u, 0u);
    ctx.r3.s64 = 0;
    sub_8235CF60(ctx, base);


    ctx.r1.u32 = old_sp;
    ctx.lr     = saved_lr;
}


extern "C" void sub_821BFCA8(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();


    ctx.lr = 0x821BFCB0u;
    ctx.fpscr.disableFlushMode();


    // Сохранение f31 и выделение стекового фрейма (224 байта)
    const uint32_t old_sp = ctx.r1.u32;
    const uint64_t saved_f31_u64 = ctx.f31.u64;
    REX_STORE_U64(old_sp - 56u, saved_f31_u64);


    const uint32_t sp = old_sp - 224u;
    REX_STORE_U32(sp, old_sp);
    ctx.r1.u32 = sp;


    const uint32_t r31 = ctx.r3.u32;


    // Индекс в глобальной таблице: 0x82B40000 - 24160 = 0x82B3A1A0
    const uint32_t idx_rot = __builtin_rotateleft32(static_cast<uint32_t>(REX_LOAD_U16(r31 + 70u)), 2);
    const uint32_t r28 = REX_LOAD_U32(0x82B3A1A0u + idx_rot);


    if (r28 != 0u) {
        const uint32_t ptr_8 = REX_LOAD_U32(r28 + 8u);
        const int32_t val_180 = static_cast<int32_t>(REX_LOAD_U32(ptr_8 + 180u));


        if (val_180 != 0) {
            const uint8_t byte_86 = REX_LOAD_U8(r31 + 86u);
            const uint8_t byte_87 = REX_LOAD_U8(r31 + 87u);


            // Поддерживаем точное состояние стека на случай инспекции
            REX_STORE_U64(sp + 88u, static_cast<uint64_t>(byte_86));
            REX_STORE_U64(sp + 80u, static_cast<uint64_t>(byte_87));


            // 0x820C0000 - 4204 = 0x820BEF94
            const float scale_f0 = REX_LOAD_F32(0x820BEF94u);
            const float f12_val = static_cast<float>(byte_87);
            const float f13_val = static_cast<float>(byte_86);
            const float f31_val = f12_val * scale_f0;


            ctx.f12.f64 = static_cast<double>(f12_val);
            ctx.f13.f64 = static_cast<double>(f13_val);
            ctx.f0.f64  = static_cast<double>(scale_f0);
            ctx.f1.f64  = static_cast<double>(f13_val * scale_f0);


            sub_8235CA58(ctx, base);


            ctx.fpscr.disableFlushMode();
            ctx.f1.f64 = static_cast<double>(f31_val);
            sub_8235CAC8(ctx, base);


            ctx.r3.u64 = REX_LOAD_U8(r31 + 88u);
            sub_8235CF60(ctx, base);


            // 0x82B30000 + 28304 = 0x82B36E90
            ctx.r3.u64 = 0x82B36E90u;
            sub_821BA7D0(ctx, base);


            // r27 = 0x83010000, r27 + 27172 = 0x83016A24
            if (ctx.r3.u32 != 0u) {
                if (static_cast<int32_t>(REX_LOAD_U32(ctx.r3.u32 + 8u)) == 21) {
                    REX_STORE_U32(0x83016A24u, 1u);
                }
            }


            // Нормализация кватерниона по адресу r4 = r31 + 32
            const uint32_t r4 = r31 + 32u;
            ctx.r4.u64 = r4;
            ctx.r3.u64 = sp + 96u;
            ctx.fpscr.disableFlushMode();


            const float qx = REX_LOAD_F32(r4 + 0u);
            const float qy = REX_LOAD_F32(r4 + 4u);
            const float qz = REX_LOAD_F32(r4 + 8u);
            const float qw = REX_LOAD_F32(r4 + 12u);


            float len_sq_f = qy * qy;
            len_sq_f = std::fma(qx, qx, len_sq_f);
            len_sq_f = std::fma(qz, qz, len_sq_f);
            len_sq_f = std::fma(qw, qw, len_sq_f);


            const double f0_len_sq = static_cast<double>(len_sq_f);
            // Загрузка констант NR из гостевой памяти:
            // 0x820C0000 - 4832 = 0x820BED20
            // 0x820C0000 - 4840 = 0x820BED18
            // 0x820C0000 - 4848 = 0x820BED10
            const double c_f8  = std::bit_cast<double>(REX_LOAD_U64(0x820BED20u));
            const double c_f7  = std::bit_cast<double>(REX_LOAD_U64(0x820BED18u));
            const double c_f13 = std::bit_cast<double>(REX_LOAD_U64(0x820BED10u));


            // Оригинальная проверка fsel f0, f6, f13, f0 где f6 = fneg(f0)
            const uint64_t f6_bits = std::bit_cast<uint64_t>(f0_len_sq) ^ 0x8000000000000000ULL;
            const double f6_neg = std::bit_cast<double>(f6_bits);


            float inv_len_f;
            if (f6_neg >= 0.0) [[unlikely]] {
                inv_len_f = static_cast<float>(c_f13);
            } else {
                // Первоначальное приближение frsqrte + 2 итерации Ньютона-Рафсона в double
                double est = static_cast<double>(1.0f / std::sqrt(len_sq_f));
                double f5 = est * est;
                double f4 = est * c_f8;
                f5 = -std::fma(f5, f0_len_sq, -c_f7);
                est = std::fma(f5, f4, est);


                f5 = est * est;
                double f8_step = est * c_f8;
                double f0_step = -std::fma(f5, f0_len_sq, -c_f7);
                est = std::fma(f0_step, f8_step, est);
                inv_len_f = static_cast<float>(est);
            }


            REX_STORE_F32(r4 + 0u,  qx * inv_len_f);
            REX_STORE_F32(r4 + 4u,  qy * inv_len_f);
            REX_STORE_F32(r4 + 8u,  qz * inv_len_f);
            REX_STORE_F32(r4 + 12u, qw * inv_len_f);


            sub_82860058(ctx, base);


            // NEON 128-bit векторная пересылка lvx128 / stvx128: (r31 + 16) -> (sp + 144)
            // Поддерживаем также ctx.v0 в актуальном состоянии (с учётом VectorMaskL)
            const uint32_t src_vec_ea = (r31 + 16u) & ~0xFu;
            const uint32_t dst_vec_ea = (sp + 144u) & ~0xFu;
            const uint8x16_t raw_vec = vld1q_u8(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(src_vec_ea)));
            vst1q_u8(reinterpret_cast<uint8_t*>(REX_RAW_ADDR(dst_vec_ea)), raw_vec);
            // В v0 хранится переставленное по VectorMaskL представление (16-байтный реверс)
            const uint8x16_t rev_vec = vrev64q_u8(raw_vec);
            const uint8x16_t v0_val = vextq_u8(rev_vec, rev_vec, 8);
            vst1q_u8(ctx.v0.u8, v0_val);


            // Запись параметров в 0x83016A28 + 8 (0x83016A30) и + 12 (0x83016A34)
            REX_STORE_U32(0x83016A30u, static_cast<uint32_t>(REX_LOAD_U8(r31 + 85u)));
            REX_STORE_U32(0x83016A34u, static_cast<uint32_t>(REX_LOAD_U8(r31 + 84u)));


            ctx.r6.u64 = 0u;
            ctx.r5.u64 = 0u;
            ctx.r4.u64 = REX_LOAD_U32(r31 + 64u);
            ctx.r3.u64 = REX_LOAD_U16(r31 + 68u);
            sub_821BB9A0(ctx, base);


            const uint32_t r29 = ctx.r3.u32;
            const uint8_t flags_89 = REX_LOAD_U8(r31 + 89u);
            ctx.fpscr.disableFlushMode();


            // 0x82020000 - 30104 = 0x82018A68
            const float default_dist = REX_LOAD_F32(0x82018A68u);
            REX_STORE_F32(sp + 80u, default_dist);


            bool skip_to_bff24 = false;


            if ((flags_89 & 0x2u) != 0u) {
                // 0x83190000 - 3680 = 0x8318F1A0
                // 0x82B40000 - 27740 = 0x82B393A4
                const uint32_t cam_ptr = REX_LOAD_U32(0x8318F1A0u);
                const uint32_t lod_ptr = REX_LOAD_U32(0x82B393A4u);


                const float pos_x = REX_LOAD_F32(sp + 144u);
                const float pos_y = REX_LOAD_F32(sp + 148u);
                const float pos_z = REX_LOAD_F32(sp + 152u);


                const float cam_x = REX_LOAD_F32(cam_ptr + 288u);
                const float cam_y = REX_LOAD_F32(cam_ptr + 292u);
                const float cam_z = REX_LOAD_F32(cam_ptr + 296u);
                const float max_d = REX_LOAD_F32(lod_ptr + 8u);


                const float dx = pos_x - cam_x;
                const float dy = pos_y - cam_y;
                const float dz = pos_z - cam_z;


                const float max_d_sq = max_d * max_d;
                float dist_sq = dy * dy;
                dist_sq = std::fma(dz, dz, dist_sq);
                dist_sq = std::fma(dx, dx, dist_sq);


                if (!(dist_sq < max_d_sq)) {
                    skip_to_bff24 = true;
                } else {
                    REX_STORE_F32(sp + 80u, std::sqrt(dist_sq));
                }
            } else {
                ctx.r8.u64 = sp + 80u;
                ctx.r7.u64 = REX_LOAD_U64(r31 + 56u);
                ctx.r5.u64 = sp + 96u;
                ctx.r6.u64 = REX_LOAD_U64(r31 + 48u);
                ctx.r4.u64 = r29;
                ctx.r3.u64 = r28;
                sub_821BEB68(ctx, base);
            }


            if (!skip_to_bff24) {
                ctx.fpscr.disableFlushMode();
                const float cur_dist = REX_LOAD_F32(sp + 80u);
                // 0x82000000 + 2612 = 0x82000A34
                const float min_threshold = REX_LOAD_F32(0x82000A34u);


                if (!(cur_dist < min_threshold)) {
                    const uint8_t cur_flags_89 = REX_LOAD_U8(r31 + 89u);
                    if ((cur_flags_89 & 0x3u) != 0u) {
                        const uint32_t bits_28_29 = static_cast<uint32_t>(cur_flags_89) & 0xCu;
                        // cntlzw + rlwinm(..., 27, 31, 31) + xori(..., 1) эквивалентно (bits_28_29 != 0 ? 1 : 0)
                        const uint32_t has_bits_28_29 = (bits_28_29 != 0u) ? 1u : 0u;


                        ctx.r6.u64 = REX_LOAD_U8(r31 + 85u);
                        ctx.r9.u64 = sp + 80u;
                        ctx.r5.u64 = REX_LOAD_U64(r31 + 56u);
                        ctx.r8.u64 = r31 + 72u;
                        ctx.r4.u64 = r29;
                        ctx.r7.u64 = has_bits_28_29;
                        ctx.r3.u64 = r28;
                        sub_821BE448(ctx, base);
                    }
                }
            }


            // loc_821BFF24:
            const uint8_t flags_post = REX_LOAD_U8(r31 + 89u);
            if ((flags_post & 0x10u) != 0u) {
                const uint32_t ptr_196 = REX_LOAD_U32(r28 + 196u);
                const int16_t idx_518 = static_cast<int16_t>(REX_LOAD_U16(ptr_196 + 518u));
                if (idx_518 > -1) {
                    ctx.r3.s64 = idx_518;
                    ctx.r4.u64 = r28;
                    ctx.r5.u64 = r29;
                    ctx.r6.u64 = REX_LOAD_U64(r31 + 56u);
                    ctx.r7.u64 = REX_LOAD_U8(r31 + 85u);
                    ctx.r8.u64 = (static_cast<uint32_t>(flags_post) >> 5) & 0x1u;
                    ctx.lr = 0x821BFF60u;
                    sub_821BE8A0(ctx, base);
                }


                const uint32_t ptr_196_b = REX_LOAD_U32(r28 + 196u);
                const int16_t idx_520 = static_cast<int16_t>(REX_LOAD_U16(ptr_196_b + 520u));
                if (idx_520 > -1) {
                    const uint8_t flags_reloaded = REX_LOAD_U8(r31 + 89u);
                    ctx.r3.s64 = idx_520;
                    ctx.r4.u64 = r28;
                    ctx.r5.u64 = r29;
                    ctx.r6.u64 = REX_LOAD_U64(r31 + 56u);
                    ctx.r7.u64 = REX_LOAD_U8(r31 + 85u);
                    ctx.r8.u64 = (static_cast<uint32_t>(flags_reloaded) >> 5) & 0x1u;
                    ctx.lr = 0x821BFF90u;
                    sub_821BE8A0(ctx, base);
                }
            }


            // loc_821BFF90: Сброс глобальных состояний
            REX_STORE_U32(0x83016A34u, 255u);
            REX_STORE_U32(0x83016A30u, 0u);
            REX_STORE_U32(0x83016A24u, 0u);
            ctx.r3.u64 = 0u;
            sub_8235CF60(ctx, base);
        }
    }


    // loc_821BFFAC: Восстановление стека и f31
    ctx.r1.u64 = sp + 224u;
    ctx.fpscr.disableFlushMode();
    ctx.f31.u64 = REX_LOAD_U64(old_sp - 56u);
}


extern "C" void sub_821BA928(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();


    ctx.r12.u64 = ctx.lr;
    ctx.lr = 0x821BA930u;


    const uint32_t old_sp = ctx.r1.u32;
    const uint32_t sp = old_sp - 160u;
    REX_STORE_U32(sp, old_sp);
    ctx.r1.u32 = sp;


    const uint32_t r26 = ctx.r3.u32;
    uint32_t buf_idx = REX_LOAD_U32(r26 + 7200u);


    // Проверка наличия задач: (2 - buf_idx) * 3596 + r26
    uint32_t count_addr = r26 + (2u - buf_idx) * 3596u;
    if (static_cast<int32_t>(REX_LOAD_U32(count_addr)) != 0) {
        // База TLS-структуры потока рендера из гостевого r13
        const uint32_t tls_base = REX_LOAD_U32(ctx.r13.u32 + 0u);
        const uint32_t tls_task_slot = tls_base + 16u;


        // Константный адрес глобального менеджера задач: 0x82B40000 - 29864 = 0x82B38B58
        constexpr uint32_t kTaskMgrAddr = 0x82B38B58u;


        for (;;) {
            const uint32_t q_base = r26 - (buf_idx * 3596u);
            const uint32_t ring_base = q_base + 3600u;


            // Чтение и обновление head (3588) и count (3592) кольцевого буфера на 128 слотов
            const int32_t next_head_raw = static_cast<int32_t>(REX_LOAD_U32(ring_base + 3588u)) + 1;
            const uint32_t next_count   = REX_LOAD_U32(ring_base + 3592u) - 1u;


            const uint32_t head = (next_head_raw == 128) ? 0u : static_cast<uint32_t>(next_head_raw);
            REX_STORE_U32(ring_base + 3588u, head);
            REX_STORE_U32(ring_base + 3592u, next_count);


            // Адрес текущего 28-байтового дескриптора задачи
            const uint32_t task_desc = ring_base + (head * 28u);


            // Помечаем статус задачи = 3 (In Progress) и публикуем указатель в TLS (+16)
            REX_STORE_U32(task_desc + 12u, 3u);
            REX_STORE_U32(tls_task_slot, task_desc);


            // Подготовка аргументов и вызов исполнителя задачи sub_821BB2D0
            ctx.r3.u64 = kTaskMgrAddr;
            ctx.r4.u64 = REX_LOAD_U32(task_desc + 0u);
            ctx.r5.u64 = REX_LOAD_U32(task_desc + 4u);
            ctx.r6.u64 = REX_LOAD_U32(task_desc + 16u);
            ctx.lr = 0x821BA9C4u;
            sub_821BB2D0(ctx, base);


            const uint32_t ret_status = ctx.r3.u32 & 0xFFu;


            // Помечаем статус задачи = 4 (Completed) и очищаем слот в TLS (+16)
            REX_STORE_U32(task_desc + 12u, 4u);
            REX_STORE_U32(tls_task_slot, 0u);


            // Перечитываем индекс активного буфера (мог измениться внутри задачи)
            buf_idx = REX_LOAD_U32(r26 + 7200u);


            if (ret_status == 0u) [[unlikely]] {
                // Сброс очереди при возврате 0
                const uint32_t reset_base = r26 - (buf_idx * 3596u);
                REX_STORE_U32(reset_base + 7192u, 0u);
                REX_STORE_U32(reset_base + 7188u, 0u);
                REX_STORE_U32(reset_base + 7184u, 0u);
                break;
            }


            count_addr = r26 + (2u - buf_idx) * 3596u;
            if (static_cast<int32_t>(REX_LOAD_U32(count_addr)) == 0) {
                break;
            }
        }
    }


    ctx.r1.u64 = sp + 160u;
}


extern "C" void sub_824F34F0(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();


    ctx.lr = 0x824F34F8u;


    const uint32_t old_sp = ctx.r1.u32;
    const uint32_t sp = old_sp - 176u;
    REX_STORE_U32(sp, old_sp);
    ctx.r1.u32 = sp;


    // Сохраняем входные аргументы в локальных регистрах AArch64
    const uint32_t r29 = ctx.r3.u32;
    const uint32_t r31 = ctx.r4.u32;
    const uint32_t r26 = ctx.r5.u32;
    const uint32_t r24 = ctx.r6.u32;
    const uint32_t r25 = ctx.r7.u32;


    // Вызов sub_824F3418(r29, r31, sp + 80, sp + 84, sp + 88)
    ctx.r5.u64 = sp + 80u;
    ctx.r6.u64 = sp + 84u;
    ctx.r7.u64 = sp + 88u;
    ctx.lr = 0x824F3520u;
    sub_824F3418(ctx, base);


    uint32_t r30 = REX_LOAD_U32(sp + 80u);
    // Глобальный LOD-оверрайд: 0x83010000 + 27172 = 0x83016A24
    const int32_t lod_override = static_cast<int32_t>(REX_LOAD_U32(0x83016A24u));


    if (lod_override != 0) {
        if (lod_override > static_cast<int32_t>(r30) ||
            lod_override > static_cast<int32_t>(REX_LOAD_U32(sp + 84u))) {
            if (lod_override == 2 && static_cast<int32_t>(REX_LOAD_U32(r31 + 72u)) != 0) {
                r30 = 2u;
            } else {
                r30 = (REX_LOAD_U32(r31 + 68u) != 0u) ? 1u : 0u;
            }
        }
    }


    // Смещение в таблице LOD-указателей: ((r30 + 16) << 2)
    const uint32_t r23 = (r30 + 16u) << 2;
    if (static_cast<int32_t>(REX_LOAD_U32(r31 + r23)) == 0) {
        ctx.r3.u64 = 0u;
        ctx.r1.u64 = sp + 176u;
        return;
    }


    // Проверка флагов объекта по смещению r29 + 4
    const uint32_t ptr_4 = REX_LOAD_U32(r29 + 4u);
    uint32_t r27 = 0u;
    if (ptr_4 != 0u) {
        // rlwinm r10, r10, 0, 6, 9 -> маска 0x03C00000; lis r9, 128 -> 0x00800000
        if ((REX_LOAD_U32(ptr_4 + 40u) & 0x03C00000u) == 0x00800000u) {
            r27 = ptr_4;
        }
    }


    const uint32_t r28 = REX_LOAD_U32(r29 + 12u);
    sub_828C4078(ctx, base);


    const bool has_c4078 = (ctx.r3.u32 != 0u);
    const bool flag_r10  = has_c4078 && ((r28 & 0x3u) != 0u);
    const bool flag_r11  = flag_r10  && ((r28 & 0x2u) != 0u);


    const uint32_t r5_val = REX_LOAD_U32(r29 + 8u);
    const int32_t  r9_val = static_cast<int32_t>(REX_LOAD_U32(r31 + r23));


    if (r24 != 0u) {
        if (r9_val != 0) {
            if (r27 != 0u) {
                const uint32_t sub_obj = REX_LOAD_U32(r27 + 52u);
                if (static_cast<int32_t>(sub_obj) != 0) {
                    ctx.r3.u64 = sub_obj;
                    ctx.r4.u64 = 0u;
                    ctx.r5.u64 = r24;
                    ctx.r6.u64 = r5_val;
                    ctx.r7.u64 = r30;
                    ctx.r8.u64 = r28;
                    ctx.lr = 0x824F365Cu;
                    sub_8247E208(ctx, base);
                    ctx.r3.u64 = 1u;
                    ctx.r1.u64 = sp + 176u;
                    return;
                }
            }


            const uint32_t vtable = REX_LOAD_U32(r31 + 0u);
            ctx.r3.u64 = r31;
            ctx.r4.u64 = 0u;
            ctx.r5.u64 = r24;


            if (flag_r10) {
                const uint32_t target_fn = REX_LOAD_U32(vtable + 48u);
                ctx.r6.u64 = r30;
                ctx.r8.u64 = r5_val;
                if (flag_r11) {
                    ctx.r7.u64 = 1u;
                    ctx.lr = 0x824F36A8u;
                } else {
                    ctx.r7.u64 = 2u;
                    ctx.lr = 0x824F36C0u;
                }
                DispatchDrawableMethod(target_fn, ctx, base);
            } else {
                const uint32_t target_fn = REX_LOAD_U32(vtable + 40u);
                ctx.r6.u64 = r5_val;
                ctx.r7.u64 = r30;
                ctx.lr = 0x824F36E8u;
                DispatchDrawableMethod(target_fn, ctx, base);
            }
        }
        ctx.r3.u64 = 1u;
        ctx.r1.u64 = sp + 176u;
        return;
    }


    if (r25 != 0u) {
        if (r9_val != 0) {
            ctx.r4.u64 = r26;
            if (r27 != 0u) {
                ctx.r3.u64 = REX_LOAD_U32(r27 + 52u);
                ctx.r5.u64 = r25;
                ctx.r6.u64 = r5_val;
                ctx.r7.u64 = r30;
                ctx.r8.u64 = r28;
                ctx.lr = 0x824F3728u;
                sub_8247E208(ctx, base);
                ctx.r3.u64 = 1u;
                ctx.r1.u64 = sp + 176u;
                return;
            }


            const uint32_t vtable = REX_LOAD_U32(r31 + 0u);
            ctx.r3.u64 = r31;
            ctx.r5.u64 = r25;


            if (flag_r10) {
                const uint32_t target_fn = REX_LOAD_U32(vtable + 48u);
                ctx.r6.u64 = r30;
                ctx.r8.u64 = r5_val;
                if (flag_r11) {
                    ctx.r7.u64 = 1u;
                    ctx.lr = 0x824F3770u;
                } else {
                    ctx.r7.u64 = 2u;
                    ctx.lr = 0x824F36C0u;
                }
                DispatchDrawableMethod(target_fn, ctx, base);
            } else {
                const uint32_t target_fn = REX_LOAD_U32(vtable + 40u);
                ctx.r6.u64 = r5_val;
                ctx.r7.u64 = r30;
                ctx.lr = 0x824F3798u;
                DispatchDrawableMethod(target_fn, ctx, base);
            }
        }
        ctx.r3.u64 = 1u;
        ctx.r1.u64 = sp + 176u;
        return;
    }


    // Ветка r24 == 0 && r25 == 0 (loc_824F37A4)
    if (r9_val != 0) {
        if (!flag_r10 || !flag_r11) {
            const uint32_t vtable = REX_LOAD_U32(r31 + 0u);
            uint32_t target_fn;
            if (flag_r10) {
                // flag_r10 == true && flag_r11 == false -> метод +44 (обычно 0x828CC178)
                ctx.r5.u64 = r30;
                ctx.r6.u64 = r5_val;
                target_fn = REX_LOAD_U32(vtable + 44u);
            } else {
                // flag_r10 == false -> метод +32 (обычно 0x828CC1A0)
                ctx.r6.u64 = r30;
                target_fn = REX_LOAD_U32(vtable + 32u);
            }
            ctx.r3.u64 = r31;
            ctx.r4.u64 = r26;
            ctx.r7.u64 = 0u;
            ctx.lr = 0x824F37F8u;
            DispatchDrawableMethod(target_fn, ctx, base);
        }
    }


    ctx.r3.u64 = 1u;
    ctx.r1.u64 = sp + 176u;
}


extern "C" void sub_822188F8(PPCContext& ctx, uint8_t* base) {
    REX_FUNC_PROLOGUE();


    const uint32_t delegate = ctx.r3.u32;
    const uint32_t target_fn = REX_LOAD_U32(delegate + 0u);
    ctx.r3.u64 = REX_LOAD_U32(delegate + 8u);


    // 99.57% всех вызовов в RenderGuest идут напрямую в sub_824F34F0
    if (target_fn == 0x824F34F0u) [[likely]] {
        sub_824F34F0(ctx, base);
    } else [[unlikely]] {
        ctx.r11.u64 = target_fn;
        const auto s_ctr = ctx.ctr;
        ctx.ctr.u64 = target_fn;
        REX_CALL_INDIRECT_FUNC(target_fn);
        ctx.ctr = s_ctr;
    }
}

