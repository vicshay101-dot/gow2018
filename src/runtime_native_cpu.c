/* Native host replacements for hot statically-linked math/CPU routines and pure-compute kernels
 * inside eboot.bin. Each candidate matches an exact 16..24 byte prologue before patching, so
 * another title or binary version skips cleanly. */
#include "runtime.h"
#include "guest_cpu.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && !defined(GOW3_NATIVE_CPU_NO_NEON)
#include <arm_neon.h>
#define NATIVE_USE_NEON 1
#else
#define NATIVE_USE_NEON 0
#endif

static int flag_file_exists(const char *path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    FILE *flag = fopen(path, "r");
    if (flag) {
        fclose(flag);
        return 1;
    }
    return 0;
#endif
}

static int native_math_disabled(void) {
    const char *env = getenv("GOW3_NATIVE_CPU_MATH");
    if (env && env[0] == '0') return 1;
    return flag_file_exists("/sdcard/Download/gow2018_no_native_math");
}

static int native_kernels_disabled(void) {
    const char *env = getenv("GOW3_NATIVE_CPU_KERNELS");
    if (env && env[0] == '0') return 1;
    return flag_file_exists("/sdcard/Download/gow2018_no_native_kernels");
}

/* ---- Phase 1: statically-linked libm replacements ---- */

static float native_sincosf_dispatch(float x, int32_t is_cos) {
    return is_cos ? cosf(x) : sinf(x);
}
static float native_powf(float x, float y) { return powf(x, y); }
static float native_acosf(float x) { return acosf(x); }
static float native_expf(float x) { return expf(x); }
static float native_fmodf(float x, float y) { return fmodf(x, y); }
static float native_tanf(float x) { return tanf(x); }
static float native_asinf(float x) { return asinf(x); }

/* ---- Phase 2: pure-compute NEON kernels ---- */

#if !NATIVE_USE_NEON
static int16_t sar_s16(int16_t v, int shift) {
    int32_t s = v;
    if (s >= 0) return (int16_t)(s >> shift);
    return (int16_t)(-(((-s) + (1 << shift) - 1) >> shift));
}

static uint8_t clamp_s16_to_u8(int16_t v) {
    if (v <= 0) return 0;
    if (v >= 255) return 255;
    return (uint8_t)v;
}
#endif

/* FUN_004231a0 (eboot_off=0x231a0): 8x4 byte vertical/horizontal deblock filter. */
static void native_filter_8x4_u8(const uint16_t *masks, uint8_t *buf, uint32_t stride) {
    uint16_t m[4];
    memcpy(m, masks, sizeof(m));
    uint8_t *base = buf - 2;
    const uint64_t step = (uint64_t)stride;

#if NATIVE_USE_NEON
    uint32_t rows[8];
    for (unsigned r = 0; r < 8; ++r) {
        memcpy(&rows[r], base + (uint64_t)r * step, sizeof(uint32_t));
    }
    uint8x8x4_t cols = vld4_u8((const uint8_t *)rows);
    int16x8_t c0 = vreinterpretq_s16_u16(vmovl_u8(cols.val[0]));
    int16x8_t c1 = vreinterpretq_s16_u16(vmovl_u8(cols.val[1]));
    int16x8_t c2 = vreinterpretq_s16_u16(vmovl_u8(cols.val[2]));
    int16x8_t c3 = vreinterpretq_s16_u16(vmovl_u8(cols.val[3]));
    int16x8_t diff = vsubq_s16(c2, c1);
    int16x8_t d4 = vshrq_n_s16(vaddq_s16(diff, vdupq_n_s16(2)), 2);
    int16x8_t d8 = vshrq_n_s16(vaddq_s16(diff, vdupq_n_s16(4)), 3);
    c0 = vaddq_s16(c0, vandq_s16(d8, vdupq_n_s16((int16_t)m[0])));
    c1 = vaddq_s16(c1, vandq_s16(d4, vdupq_n_s16((int16_t)m[1])));
    c2 = vsubq_s16(c2, vandq_s16(d4, vdupq_n_s16((int16_t)m[2])));
    c3 = vsubq_s16(c3, vandq_s16(d8, vdupq_n_s16((int16_t)m[3])));
    cols.val[0] = vqmovun_s16(c0);
    cols.val[1] = vqmovun_s16(c1);
    cols.val[2] = vqmovun_s16(c2);
    cols.val[3] = vqmovun_s16(c3);
    vst4_u8((uint8_t *)rows, cols);
    for (unsigned r = 0; r < 8; ++r) {
        memcpy(base + (uint64_t)r * step, &rows[r], sizeof(uint32_t));
    }
#else
    for (unsigned r = 0; r < 8; ++r) {
        uint8_t *p = base + (uint64_t)r * step;
        int16_t c0 = (int16_t)p[0];
        int16_t c1 = (int16_t)p[1];
        int16_t c2 = (int16_t)p[2];
        int16_t c3 = (int16_t)p[3];
        int16_t diff = (int16_t)(c2 - c1);
        uint16_t d4 = (uint16_t)sar_s16((int16_t)(diff + 2), 2);
        uint16_t d8 = (uint16_t)sar_s16((int16_t)(diff + 4), 3);
        c0 = (int16_t)((uint16_t)c0 + (d8 & m[0]));
        c1 = (int16_t)((uint16_t)c1 + (d4 & m[1]));
        c2 = (int16_t)((uint16_t)c2 - (d4 & m[2]));
        c3 = (int16_t)((uint16_t)c3 - (d8 & m[3]));
        p[0] = clamp_s16_to_u8(c0);
        p[1] = clamp_s16_to_u8(c1);
        p[2] = clamp_s16_to_u8(c2);
        p[3] = clamp_s16_to_u8(c3);
    }
#endif
}

/* FUN_00d71520 (eboot_off=0x971520): convert 3x3 rotation matrix (16B row stride) to quaternion. */
static void native_rot3x3_to_quat(float *out_quat, const float *mat) {
    float m[3][3];
    for (int r = 0; r < 3; ++r) {
        memcpy(m[r], mat + r * 4, sizeof(m[r]));
    }
    float q[4];
    float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f);
        float inv = 0.5f / s;
        q[0] = inv * (m[1][2] - m[2][1]);
        q[1] = inv * (m[2][0] - m[0][2]);
        q[2] = inv * (m[0][1] - m[1][0]);
        q[3] = s * 0.5f;
    } else {
        static const int next[3] = {1, 2, 0};
        int i = (m[0][0] < m[1][1]) ? 1 : 0;
        if (m[i][i] < m[2][2]) i = 2;
        int j = next[i];
        int k = next[j];
        float s = sqrtf((m[i][i] - (m[j][j] + m[k][k])) + 1.0f);
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 0.0f;
        q[i] = s * 0.5f;
        if (s != 0.0f) s = 0.5f / s;
        q[3] = s * (m[j][k] - m[k][j]);
        q[j] = s * (m[i][j] + m[j][i]);
        q[k] = s * (m[i][k] + m[k][i]);
    }
    memcpy(out_quat, q, sizeof(q));
}

/* FUN_00fdbc00 (eboot_off=0xbdbc00): propagate skeleton node transforms iteratively/recursively. */
static void bone_propagate_impl(const uint8_t *node, const uint8_t *nodes_base,
                                uint8_t *out_transforms,
                                const float parent_pos[4], const float parent_rot[4]) {
    float p[4], q[4];
    memcpy(p, parent_pos, sizeof(p));
    memcpy(q, parent_rot, sizeof(q));

    for (;;) {
        uint64_t tx_idx;
        memcpy(&tx_idx, node + 0x98, sizeof(tx_idx));
        uint8_t *entry = out_transforms + tx_idx * 0x60ULL;

        float lv[4], lr[4];
        memcpy(lv, entry + 0x30, sizeof(lv));
        memcpy(lr, entry + 0x40, sizeof(lr));

        float wp[4], wr[4];
#if NATIVE_USE_NEON
        static const uint32_t kNegXyzMask[4] = {0x80000000u, 0x80000000u, 0x80000000u, 0u};
        float32x4_t pv = vld1q_f32(p);
        float32x4_t qv = vld1q_f32(q);
        float32x4_t vv = vld1q_f32(lv);
        float32x4_t tv = vsetq_lane_f32(0.0f, vmulq_n_f32(vv, 2.0f), 3);

        float qx = vgetq_lane_f32(qv, 0), qy = vgetq_lane_f32(qv, 1);
        float qz = vgetq_lane_f32(qv, 2), qw = vgetq_lane_f32(qv, 3);
        float tx = vgetq_lane_f32(tv, 0), ty = vgetq_lane_f32(tv, 1), tz = vgetq_lane_f32(tv, 2);
        float32x4_t cross = {qy * tz - qz * ty, qz * tx - qx * tz, qx * ty - qy * tx, 0.0f};
        float dot_qt = vaddvq_f32(vmulq_f32(qv, tv));
        float32x4_t part1 = vmulq_n_f32(vsetq_lane_f32(0.0f, qv, 3), dot_qt);
        float32x4_t part2 = vmulq_n_f32(vaddq_f32(vmulq_n_f32(tv, qw), cross), qw);
        float32x4_t neg_v = vreinterpretq_f32_u32(
            veorq_u32(vreinterpretq_u32_f32(vv), vld1q_u32(kNegXyzMask)));
        float32x4_t world_p = vaddq_f32(pv, vaddq_f32(vaddq_f32(part1, part2), neg_v));
        vst1q_f32(wp, world_p);

        float rx = lr[0], ry = lr[1], rz = lr[2], rw = lr[3];
        float32x4_t h = {
            qw * rx + qx * rw + qy * rz - qz * ry,
            qw * ry + qy * rw + qz * rx - qx * rz,
            qw * rz + qz * rw + qx * ry - qy * rx,
            qw * rw - qx * rx - qy * ry - qz * rz,
        };
        float norm2 = vaddvq_f32(vmulq_f32(h, h));
        float inv_len = 1.0f / sqrtf(norm2);
        vst1q_f32(wr, vmulq_n_f32(h, inv_len));
#else
        float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
        float tx = 2.0f * lv[0], ty = 2.0f * lv[1], tz = 2.0f * lv[2];
        float cx = qy * tz - qz * ty;
        float cy = qz * tx - qx * tz;
        float cz = qx * ty - qy * tx;
        float dot_qt = qx * tx + qy * ty + qz * tz;
        wp[0] = p[0] + (qx * dot_qt + qw * (qw * tx + cx) - lv[0]);
        wp[1] = p[1] + (qy * dot_qt + qw * (qw * ty + cy) - lv[1]);
        wp[2] = p[2] + (qz * dot_qt + qw * (qw * tz + cz) - lv[2]);
        wp[3] = p[3] + lv[3];

        float rx = lr[0], ry = lr[1], rz = lr[2], rw = lr[3];
        float hx = qw * rx + qx * rw + qy * rz - qz * ry;
        float hy = qw * ry + qy * rw + qz * rx - qx * rz;
        float hz = qw * rz + qz * rw + qx * ry - qy * rx;
        float hw = qw * rw - qx * rx - qy * ry - qz * rz;
        float inv_len = 1.0f / sqrtf(hx * hx + hy * hy + hz * hz + hw * hw);
        wr[0] = hx * inv_len;
        wr[1] = hy * inv_len;
        wr[2] = hz * inv_len;
        wr[3] = hw * inv_len;
#endif
        memcpy(entry + 0x00, wp, sizeof(wp));
        memcpy(entry + 0x10, wr, sizeof(wr));

        int32_t child_count;
        memcpy(&child_count, node + 0x90, sizeof(child_count));
        if (child_count == 1) {
            uint64_t first_child;
            memcpy(&first_child, node + 0x88, sizeof(first_child));
            node = nodes_base + first_child * 0xb0ULL;
            memcpy(p, wp, sizeof(p));
            memcpy(q, wr, sizeof(q));
            continue;
        }
        if (child_count <= 0) {
            return;
        }
        for (int64_t i = 0; i < (int64_t)child_count; ++i) {
            uint64_t first_child;
            memcpy(&first_child, node + 0x88, sizeof(first_child));
            const uint8_t *child = nodes_base + (first_child + (uint64_t)i) * 0xb0ULL;
            bone_propagate_impl(child, nodes_base, out_transforms, wp, wr);
            memcpy(&child_count, node + 0x90, sizeof(child_count));
        }
        return;
    }
}

static void native_bone_propagate(uintptr_t node, uintptr_t nodes_base, uintptr_t out_transforms,
                                  const float *parent_pos, const float *parent_rot) {
    bone_propagate_impl((const uint8_t *)node, (const uint8_t *)nodes_base,
                        (uint8_t *)out_transforms, parent_pos, parent_rot);
}

/* FUN_00433390 (eboot_off=0x33390): 48-byte SIMD hash in OodleLZ; reads live 48-byte seeds at
 * image_base + 0xf0b5a0..0xf0b5cf. */
static uintptr_t g_oodle_seeds_va = 0;

static uint32_t rotl32(uint32_t v, unsigned s) {
    return (v << s) | (v >> (32U - s));
}

#define OODLE_MIX32(a, b, c) do {                \
    (a) -= (c); (a) ^= rotl32((c), 4U);  (c) += (b); \
    (b) -= (a); (b) ^= rotl32((a), 6U);  (a) += (c); \
    (c) -= (b); (c) ^= rotl32((b), 8U);  (b) += (a); \
    (a) -= (c); (a) ^= rotl32((c), 16U); (c) += (b); \
    (b) -= (a); (b) ^= rotl32((a), 19U); (a) += (c); \
    (c) -= (b); (c) ^= rotl32((b), 4U);  (b) += (a); \
} while (0)

#define OODLE_FINAL32(a, b, c) do { \
    (c) ^= (b); (c) -= rotl32((b), 14U); \
    (a) ^= (c); (a) -= rotl32((c), 11U); \
    (b) ^= (a); (b) -= rotl32((a), 25U); \
    (c) ^= (b); (c) -= rotl32((b), 16U); \
    (a) ^= (c); (a) -= rotl32((c), 4U);  \
    (b) ^= (a); (b) -= rotl32((a), 14U); \
    (c) ^= (b); (c) -= rotl32((b), 24U); \
} while (0)

#if NATIVE_USE_NEON
#define NEON_ROTL32(v, s) vsliq_n_u32(vshrq_n_u32((v), 32 - (s)), (v), (s))
#define NEON_MIX4(a, b, c, t) do {                                   \
    (t) = NEON_ROTL32((c), 4);  (a) = veorq_u32(vsubq_u32((a), (c)), (t)); (c) = vaddq_u32((c), (b)); \
    (t) = NEON_ROTL32((a), 6);  (b) = veorq_u32(vsubq_u32((b), (a)), (t)); (a) = vaddq_u32((a), (c)); \
    (t) = NEON_ROTL32((b), 8);  (c) = veorq_u32(vsubq_u32((c), (b)), (t)); (b) = vaddq_u32((b), (a)); \
    (t) = NEON_ROTL32((c), 16); (a) = veorq_u32(vsubq_u32((a), (c)), (t)); (c) = vaddq_u32((c), (b)); \
    (t) = NEON_ROTL32((a), 19); (b) = veorq_u32(vsubq_u32((b), (a)), (t)); (a) = vaddq_u32((a), (c)); \
    (t) = NEON_ROTL32((b), 4);  (c) = veorq_u32(vsubq_u32((c), (b)), (t)); (b) = vaddq_u32((b), (a)); \
} while (0)
#else
static uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
#endif

static uint64_t native_oodle_hash48(const uint8_t *data, size_t len) {
    uint32_t seeds[12];
    memcpy(seeds, (const void *)g_oodle_seeds_va, sizeof(seeds));
    const uint32_t len32 = (uint32_t)len;

    uint32_t av[4], bv[4], cv[4];
#if NATIVE_USE_NEON
    uint32x4_t lvec = vdupq_n_u32(len32);
    uint32x4_t a = vaddq_u32(vld1q_u32(&seeds[0]), lvec);
    uint32x4_t b = vld1q_u32(&seeds[4]);
    uint32x4_t c = vsubq_u32(vld1q_u32(&seeds[8]), lvec);
    uint32x4_t t;
    while (len >= 48) {
        uint32x4_t w0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 0)));
        uint32x4_t w1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16)));
        uint32x4_t w2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 32)));
        a = vaddq_u32(a, w0);
        b = vaddq_u32(b, w1);
        c = vaddq_u32(c, w2);
        NEON_MIX4(a, b, c, t);
        data += 48;
        len -= 48;
    }
    if (len > 0) {
        uint8_t tail[48];
        memset(tail, 0, sizeof(tail));
        memcpy(tail, data, len);
        uint32x4_t w0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(tail + 0)));
        uint32x4_t w1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(tail + 16)));
        uint32x4_t w2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(tail + 32)));
        a = vaddq_u32(a, w0);
        b = vaddq_u32(b, w1);
        c = vaddq_u32(c, w2);
        NEON_MIX4(a, b, c, t);
    }
    vst1q_u32(av, a);
    vst1q_u32(bv, b);
    vst1q_u32(cv, c);
#else
    for (int i = 0; i < 4; ++i) {
        av[i] = seeds[0 + i] + len32;
        bv[i] = seeds[4 + i];
        cv[i] = seeds[8 + i] - len32;
    }
    while (len >= 48) {
        for (int i = 0; i < 4; ++i) {
            av[i] += load_be32(data + i * 4);
            bv[i] += load_be32(data + 16 + i * 4);
            cv[i] += load_be32(data + 32 + i * 4);
            OODLE_MIX32(av[i], bv[i], cv[i]);
        }
        data += 48;
        len -= 48;
    }
    if (len > 0) {
        uint8_t tail[48];
        memset(tail, 0, sizeof(tail));
        memcpy(tail, data, len);
        for (int i = 0; i < 4; ++i) {
            av[i] += load_be32(tail + i * 4);
            bv[i] += load_be32(tail + 16 + i * 4);
            cv[i] += load_be32(tail + 32 + i * 4);
            OODLE_MIX32(av[i], bv[i], cv[i]);
        }
    }
#endif

    uint32_t A = av[0], B = av[1], C = av[2];
    OODLE_MIX32(A, B, C);
    A += av[3]; B += bv[0]; C += bv[1];
    OODLE_MIX32(A, B, C);
    A += bv[2]; B += bv[3]; C += cv[0];
    OODLE_MIX32(A, B, C);
    A += cv[1]; B += cv[2]; C += cv[3];
    OODLE_FINAL32(A, B, C);
    return ((uint64_t)B << 32) | (uint64_t)C;
}

/* ---- Installation ---- */

typedef struct {
    uint64_t offset;
    void *fn;
    GuestReplaceKind kind;
    const char *name;
    size_t expected_len;
    uint64_t min_image_size;
    unsigned char expected[24];
} NativeReplacementEntry;

static unsigned install_table(uintptr_t image_base, uint64_t exec_size, uint64_t image_size,
                              const NativeReplacementEntry *entries, size_t count) {
    unsigned installed = 0;
    for (size_t i = 0; i < count; ++i) {
        const NativeReplacementEntry *e = &entries[i];
        if (e->offset + e->expected_len > exec_size) continue;
        if (e->min_image_size && image_size < e->min_image_size) continue;
        if (guest_cpu_replace_function(image_base + e->offset, e->fn, e->kind,
                                       e->expected, e->expected_len, e->name)) {
            ++installed;
        }
    }
    return installed;
}

unsigned runtime_native_cpu_install(uintptr_t image_base, uint64_t exec_size, uint64_t image_size) {
    static const NativeReplacementEntry kMathTable[] = {
        {
            0xa56820ULL, (void *)native_sincosf_dispatch, GUEST_REPLACE_F32_I32_TO_F32, "native:sincosf", 16, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xce, 0xab},
        },
        {
            0xc36300ULL, (void *)native_powf, GUEST_REPLACE_F32_F32_TO_F32, "native:powf", 16, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xee, 0xb0},
        },
        {
            0xa53220ULL, (void *)native_acosf, GUEST_REPLACE_F32_TO_F32, "native:acosf", 16, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x1d, 0xd0, 0xe1, 0x3f, 0x00},
        },
        {
            0xa53fe0ULL, (void *)native_expf, GUEST_REPLACE_F32_TO_F32, "native:expf_scalar", 16, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x1d, 0x10, 0xd4, 0x3f, 0x00},
        },
        {
            0xa55900ULL, (void *)native_expf, GUEST_REPLACE_F32_TO_F32, "native:expf_table", 16, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x1d, 0xf0, 0xba, 0x3f, 0x00},
        },
        {
            0xa545b0ULL, (void *)native_fmodf, GUEST_REPLACE_F32_F32_TO_F32, "native:fmodf", 16, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b, 0x35, 0x3e, 0xce},
        },
        {
            0xa57080ULL, (void *)native_tanf, GUEST_REPLACE_F32_TO_F32, "native:tanf", 16, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x6e, 0xa3},
        },
        {
            0xa53790ULL, (void *)native_asinf, GUEST_REPLACE_F32_TO_F32, "native:asinf", 16, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x5e, 0xdc},
        },
    };
    static const NativeReplacementEntry kKernelTable[] = {
        {
            0x0231a0ULL, (void *)native_filter_8x4_u8, GUEST_REPLACE_INT6, "native:filter_8x4_u8", 24, 0,
            {0xc5, 0xf9, 0x6e, 0x46, 0xfe, 0x89, 0xd0, 0xc5, 0xf9, 0x6e, 0x4c, 0x06,
             0xfe, 0x4c, 0x8d, 0x44, 0x00, 0xfe, 0xc4, 0xa1, 0x79, 0x6e, 0x14, 0x06},
        },
        {
            0x971520ULL, (void *)native_rot3x3_to_quat, GUEST_REPLACE_INT6, "native:rot3x3_to_quat", 24, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
             0x53, 0x48, 0x83, 0xec, 0x58, 0x48, 0x8b, 0x1d, 0xc8, 0xfe, 0x4d, 0x00},
        },
        {
            0xbdbc00ULL, (void *)native_bone_propagate, GUEST_REPLACE_PTR3_V128X2, "native:bone_propagate", 24, 0,
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
             0x83, 0xec, 0x20, 0xc5, 0xf8, 0x10, 0x1d, 0xf9, 0xfd, 0x0c, 0x00, 0xc5},
        },
        {
            0x033390ULL, (void *)native_oodle_hash48, GUEST_REPLACE_INT6, "native:oodle_hash48", 24, 0xf0b5d0ULL,
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x70, 0x49,
             0x89, 0xf6, 0xc5, 0xfa, 0x6f, 0x05, 0x1a, 0x82, 0xed, 0x00, 0xc5, 0xfa},
        },
    };
    const size_t math_total = sizeof(kMathTable) / sizeof(kMathTable[0]);
    const size_t kern_total = sizeof(kKernelTable) / sizeof(kKernelTable[0]);
    unsigned math_installed = 0;
    unsigned kern_installed = 0;

    if (!image_base || !exec_size) {
        printf("Native CPU math: disabled or unavailable (0/%zu)\n", math_total);
        printf("Native CPU kernels: disabled or unavailable (0/%zu)\n", kern_total);
        return 0;
    }
    if (native_math_disabled()) {
        printf("Native CPU math: disabled or unavailable (0/%zu)\n", math_total);
    } else {
        math_installed = install_table(image_base, exec_size, image_size, kMathTable, math_total);
        printf("Native CPU math: %u/%zu function(s) installed\n", math_installed, math_total);
    }
    if (native_kernels_disabled()) {
        printf("Native CPU kernels: disabled or unavailable (0/%zu)\n", kern_total);
    } else {
        g_oodle_seeds_va = image_base + 0xf0b5a0ULL;
        kern_installed = install_table(image_base, exec_size, image_size, kKernelTable, kern_total);
        printf("Native CPU kernels: %u/%zu function(s) installed\n", kern_installed, kern_total);
    }
    return math_installed + kern_installed;
}
