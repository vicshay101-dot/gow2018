#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "guest_cpu.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uintptr_t address;
    void *host_fn;
    GuestReplaceKind kind;
    const char *name;
    size_t len;
} RecordedHook;

static RecordedHook recorded[64];
static size_t recorded_count;

int guest_cpu_replace_function(uintptr_t address, void *host_fn, GuestReplaceKind kind,
                               const unsigned char *expected_bytes, size_t expected_len,
                               const char *name) {
    if (!address || !host_fn || (expected_bytes && expected_len < 6)) return 0;
    if (expected_bytes && memcmp((const void *)address, expected_bytes, expected_len) != 0) return 0;
    if (recorded_count >= sizeof(recorded) / sizeof(recorded[0])) return 0;
    recorded[recorded_count++] = (RecordedHook){address, host_fn, kind, name, expected_len};
    return 1;
}

static const RecordedHook *find_recorded(const char *name) {
    for (size_t i = 0; i < recorded_count; ++i) {
        if (recorded[i].name && strcmp(recorded[i].name, name) == 0) return &recorded[i];
    }
    return NULL;
}

static int near_f32(float a, float b, float tol) {
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    return fabsf(a - b) <= tol;
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void test_import_names_coverage(void) {
    static const struct { const char *nid; const char *symbol; } entries[] = {
#include "import_names.inc"
#include "import_names_extra.inc"
    };
    static const struct { const char *nid; const char *expected; } phase1_nids[] = {
        {"JaRMy+QcpeU#libkernel", "scePthreadAttrGetdetachstate"},
        {"-fA+7ZlGDQs#libkernel", "scePthreadAttrGetstack"},
        {"El+cQ20DynU#libkernel", "scePthreadAttrSetguardsize"},
        {"4DM06U2BNEY#libkernel", "sceKernelCancelSema"},
        {"2emaaluWzUw#libc", "__cxa_guard_abort"},
        {"H2e8t5ScQGc#libc", "__cxa_finalize"},
        {"hHrGoGoNf+s#libSceSysmodule", "sceSysmoduleLoadModuleInternal"},
        {"NesIgTmfF0Q#libc", "bsearch"},
        {"KGKBeVcqJjc#libc", "ldexp"},
        {"9fs1btfLoUs#libc", "ldexpf"},
    };
    const size_t total = sizeof(entries) / sizeof(entries[0]);
    for (size_t k = 0; k < sizeof(phase1_nids) / sizeof(phase1_nids[0]); ++k) {
        const char *found = NULL;
        for (size_t i = 0; i < total; ++i) {
            if (strcmp(entries[i].nid, phase1_nids[k].nid) == 0) {
                found = entries[i].symbol;
                break;
            }
        }
        assert(found != NULL);
        assert(strcmp(found, phase1_nids[k].expected) == 0);
    }
}

static void seed_prologues(unsigned char *img) {
    static const struct { size_t off; size_t len; unsigned char bytes[24]; } seeds[] = {
        {0xa56820, 16, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xce, 0xab}},
        {0xc36300, 16, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xee, 0xb0}},
        {0xa53220, 16, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x1d, 0xd0, 0xe1, 0x3f, 0x00}},
        {0xa53fe0, 16, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x1d, 0x10, 0xd4, 0x3f, 0x00}},
        {0xa55900, 16, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x1d, 0xf0, 0xba, 0x3f, 0x00}},
        {0xa545b0, 16, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b, 0x35, 0x3e, 0xce}},
        {0xa57080, 16, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x6e, 0xa3}},
        {0xa53790, 16, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x5e, 0xdc}},
        {0x0231a0, 24, {0xc5, 0xf9, 0x6e, 0x46, 0xfe, 0x89, 0xd0, 0xc5, 0xf9, 0x6e, 0x4c, 0x06,
                        0xfe, 0x4c, 0x8d, 0x44, 0x00, 0xfe, 0xc4, 0xa1, 0x79, 0x6e, 0x14, 0x06}},
        {0x971520, 24, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                        0x53, 0x48, 0x83, 0xec, 0x58, 0x48, 0x8b, 0x1d, 0xc8, 0xfe, 0x4d, 0x00}},
        {0xbdbc00, 24, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
                        0x83, 0xec, 0x20, 0xc5, 0xf8, 0x10, 0x1d, 0xf9, 0xfd, 0x0c, 0x00, 0xc5}},
        {0x033390, 24, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x70, 0x49,
                        0x89, 0xf6, 0xc5, 0xfa, 0x6f, 0x05, 0x1a, 0x82, 0xed, 0x00, 0xc5, 0xfa}},
    };
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        memcpy(img + seeds[i].off, seeds[i].bytes, seeds[i].len);
    }
}

static void seed_oodle_bss(unsigned char *img, uint32_t sa, uint32_t sb, uint32_t sc) {
    uint32_t s[12] = {sa, sa, sa, sa, sb, sb, sb, sb, sc, sc, sc, sc};
    memcpy(img + 0xf0b5a0, s, sizeof(s));
}

/* ---- Independent x86 reference models ---- */

static int16_t ref_floor_shr_s16(int32_t x, int shift) {
    int32_t div = 1 << shift;
    if (x >= 0) return (int16_t)(x / div);
    return (int16_t)(-(((-x) + div - 1) / div));
}

static uint8_t ref_sat_u8(int32_t x) {
    return x < 0 ? 0 : (x > 255 ? 255 : (uint8_t)x);
}

static void ref_filter_8x4_u8(const uint16_t masks[4], uint8_t *buf, uint32_t stride) {
    for (int r = 0; r < 8; ++r) {
        uint8_t *p = buf - 2 + (uint64_t)r * (uint64_t)stride;
        int32_t c0 = p[0], c1 = p[1], c2 = p[2], c3 = p[3];
        int32_t diff = c2 - c1;
        uint16_t d4 = (uint16_t)ref_floor_shr_s16(diff + 2, 2);
        uint16_t d8 = (uint16_t)ref_floor_shr_s16(diff + 4, 3);
        int16_t o0 = (int16_t)((uint16_t)c0 + (d8 & masks[0]));
        int16_t o1 = (int16_t)((uint16_t)c1 + (d4 & masks[1]));
        int16_t o2 = (int16_t)((uint16_t)c2 - (d4 & masks[2]));
        int16_t o3 = (int16_t)((uint16_t)c3 - (d8 & masks[3]));
        p[0] = ref_sat_u8(o0);
        p[1] = ref_sat_u8(o1);
        p[2] = ref_sat_u8(o2);
        p[3] = ref_sat_u8(o3);
    }
}

static void ref_rot3x3_to_quat(float out_q[4], const float mat[12]) {
    const float m00 = mat[0], m01 = mat[1], m02 = mat[2];
    const float m10 = mat[4], m11 = mat[5], m12 = mat[6];
    const float m20 = mat[8], m21 = mat[9], m22 = mat[10];
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f);
        float inv = 0.5f / s;
        out_q[0] = (m12 - m21) * inv;
        out_q[1] = (m20 - m02) * inv;
        out_q[2] = (m01 - m10) * inv;
        out_q[3] = 0.5f * s;
    } else if (m00 >= m11 && m00 >= m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22);
        float inv = (s != 0.0f) ? 0.5f / s : 0.0f;
        out_q[0] = 0.5f * s;
        out_q[1] = (m01 + m10) * inv;
        out_q[2] = (m02 + m20) * inv;
        out_q[3] = (m12 - m21) * inv;
    } else if (m11 >= m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22);
        float inv = (s != 0.0f) ? 0.5f / s : 0.0f;
        out_q[0] = (m10 + m01) * inv;
        out_q[1] = 0.5f * s;
        out_q[2] = (m12 + m21) * inv;
        out_q[3] = (m20 - m02) * inv;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11);
        float inv = (s != 0.0f) ? 0.5f / s : 0.0f;
        out_q[0] = (m20 + m02) * inv;
        out_q[1] = (m21 + m12) * inv;
        out_q[2] = 0.5f * s;
        out_q[3] = (m01 - m10) * inv;
    }
}

static void quat_rotate_vec3_plus_w(float out[4], const float q[4], const float v[4]) {
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float vx = v[0], vy = v[1], vz = v[2];
    /* q * (vx, vy, vz, 0) * conj(q) = v + 2*qw*(q_xyz x v) + 2*(q_xyz x (q_xyz x v)) */
    float uvx = qy * vz - qz * vy;
    float uvy = qz * vx - qx * vz;
    float uvz = qx * vy - qy * vx;
    float uuvx = qy * uvz - qz * uvy;
    float uuvy = qz * uvx - qx * uvz;
    float uuvz = qx * uvy - qy * uvx;
    out[0] = vx + 2.0f * (qw * uvx + uuvx);
    out[1] = vy + 2.0f * (qw * uvy + uuvy);
    out[2] = vz + 2.0f * (qw * uvz + uuvz);
    out[3] = v[3];
}

static void ref_bone_propagate(const uint8_t *node, const uint8_t *nodes_base,
                               uint8_t *out_transforms, const float p[4], const float q[4]) {
    uint64_t tx_idx;
    memcpy(&tx_idx, node + 0x98, sizeof(tx_idx));
    uint8_t *entry = out_transforms + tx_idx * 0x60ULL;
    float lv[4], lr[4];
    memcpy(lv, entry + 0x30, sizeof(lv));
    memcpy(lr, entry + 0x40, sizeof(lr));

    float rot_lv[4], wp[4];
    quat_rotate_vec3_plus_w(rot_lv, q, lv);
    for (int i = 0; i < 4; ++i) wp[i] = p[i] + rot_lv[i];

    float hx = q[3] * lr[0] + q[0] * lr[3] + q[1] * lr[2] - q[2] * lr[1];
    float hy = q[3] * lr[1] + q[1] * lr[3] + q[2] * lr[0] - q[0] * lr[2];
    float hz = q[3] * lr[2] + q[2] * lr[3] + q[0] * lr[1] - q[1] * lr[0];
    float hw = q[3] * lr[3] - q[0] * lr[0] - q[1] * lr[1] - q[2] * lr[2];
    float inv = 1.0f / sqrtf(hx * hx + hy * hy + hz * hz + hw * hw);
    float wr[4] = {hx * inv, hy * inv, hz * inv, hw * inv};

    memcpy(entry + 0x00, wp, sizeof(wp));
    memcpy(entry + 0x10, wr, sizeof(wr));

    int32_t child_count;
    uint64_t first_child;
    memcpy(&child_count, node + 0x90, sizeof(child_count));
    memcpy(&first_child, node + 0x88, sizeof(first_child));
    for (int i = 0; i < child_count; ++i) {
        ref_bone_propagate(nodes_base + (first_child + (uint64_t)i) * 0xb0ULL,
                           nodes_base, out_transforms, wp, wr);
    }
}

static uint32_t ref_rotl32(uint32_t v, unsigned s) { return (v << s) | (v >> (32U - s)); }
static uint32_t ref_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void ref_mix(uint32_t *a, uint32_t *b, uint32_t *c) {
    *a -= *c; *a ^= ref_rotl32(*c, 4);  *c += *b;
    *b -= *a; *b ^= ref_rotl32(*a, 6);  *a += *c;
    *c -= *b; *c ^= ref_rotl32(*b, 8);  *b += *a;
    *a -= *c; *a ^= ref_rotl32(*c, 16); *c += *b;
    *b -= *a; *b ^= ref_rotl32(*a, 19); *a += *c;
    *c -= *b; *c ^= ref_rotl32(*b, 4);  *b += *a;
}
static void ref_final(uint32_t *a, uint32_t *b, uint32_t *c) {
    *c ^= *b; *c -= ref_rotl32(*b, 14);
    *a ^= *c; *a -= ref_rotl32(*c, 11);
    *b ^= *a; *b -= ref_rotl32(*a, 25);
    *c ^= *b; *c -= ref_rotl32(*b, 16);
    *a ^= *c; *a -= ref_rotl32(*c, 4);
    *b ^= *a; *b -= ref_rotl32(*a, 14);
    *c ^= *b; *c -= ref_rotl32(*b, 24);
}
static uint64_t ref_oodle_hash48(const unsigned char *img, const uint8_t *data, size_t len) {
    uint32_t seeds[12];
    memcpy(seeds, img + 0xf0b5a0, sizeof(seeds));
    uint32_t av[4], bv[4], cv[4];
    for (int i = 0; i < 4; ++i) {
        av[i] = seeds[i] + (uint32_t)len;
        bv[i] = seeds[4 + i];
        cv[i] = seeds[8 + i] - (uint32_t)len;
    }
    while (len >= 48) {
        for (int i = 0; i < 4; ++i) {
            av[i] += ref_be32(data + i * 4);
            bv[i] += ref_be32(data + 16 + i * 4);
            cv[i] += ref_be32(data + 32 + i * 4);
            ref_mix(&av[i], &bv[i], &cv[i]);
        }
        data += 48;
        len -= 48;
    }
    if (len > 0) {
        uint8_t tail[48];
        memset(tail, 0, sizeof(tail));
        memcpy(tail, data, len);
        for (int i = 0; i < 4; ++i) {
            av[i] += ref_be32(tail + i * 4);
            bv[i] += ref_be32(tail + 16 + i * 4);
            cv[i] += ref_be32(tail + 32 + i * 4);
            ref_mix(&av[i], &bv[i], &cv[i]);
        }
    }
    uint32_t A = av[0], B = av[1], C = av[2];
    ref_mix(&A, &B, &C);
    A += av[3]; B += bv[0]; C += bv[1]; ref_mix(&A, &B, &C);
    A += bv[2]; B += bv[3]; C += cv[0]; ref_mix(&A, &B, &C);
    A += cv[1]; B += cv[2]; C += cv[3]; ref_final(&A, &B, &C);
    return ((uint64_t)B << 32) | C;
}

static void test_filter_8x4(const RecordedHook *hk) {
    assert(hk && hk->kind == GUEST_REPLACE_INT6);
    void (*fn)(const uint16_t *, uint8_t *, uint32_t) = (void (*)(const uint16_t *, uint8_t *, uint32_t))hk->host_fn;

    static const uint32_t strides[] = {4, 7, 16, 64, 512};
    static const uint16_t mask_presets[][4] = {
        {0xffff, 0xffff, 0xffff, 0xffff},
        {0x0000, 0xffff, 0xffff, 0x0000},
        {0x0000, 0x0000, 0x0000, 0x0000},
        {0x00ff, 0x7fff, 0xffff, 0x0f0f},
    };
    uint8_t buf_a[8 * 512 + 32];
    uint8_t buf_b[8 * 512 + 32];
    uint32_t rng = 0x12345678u;

    for (unsigned t = 0; t < 200; ++t) {
        uint32_t stride = strides[t % (sizeof(strides) / sizeof(strides[0]))];
        const uint16_t *masks = mask_presets[t % (sizeof(mask_presets) / sizeof(mask_presets[0]))];
        size_t total = 8ULL * stride + 16;
        for (size_t i = 0; i < total; ++i) {
            uint8_t v = (t < 10 && (i & 1)) ? 0xff : (t < 10 ? 0x00 : (uint8_t)xorshift32(&rng));
            buf_a[i] = v;
            buf_b[i] = v;
        }
        uint32_t offset = 6 + (t % 3); /* cover misaligned buf-2 */
        ref_filter_8x4_u8(masks, buf_a + offset, stride);
        fn(masks, buf_b + offset, stride);
        assert(memcmp(buf_a, buf_b, total) == 0);
    }
}

static void test_rot3x3_to_quat(const RecordedHook *hk) {
    assert(hk && hk->kind == GUEST_REPLACE_INT6);
    void (*fn)(float *, const float *) = (void (*)(float *, const float *))hk->host_fn;

    static const float axis[5][3] = {
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 2, -3}, {-2, 1, 4}
    };
    static const float angles[6] = {0.0f, 0.7f, -1.3f, 3.14159265f, 3.1415f, -2.5f};

    /* Degenerate zero matrix (s == 0 branch must not NaN/Inf) */
    float zero_mat[12] = {0};
    float q_got[4] = {9, 9, 9, 9}, q_ref[4] = {9, 9, 9, 9};
    fn(q_got, zero_mat);
    ref_rot3x3_to_quat(q_ref, zero_mat);
    for (int i = 0; i < 4; ++i) assert(near_f32(q_got[i], q_ref[i], 1e-6f));

    for (size_t a = 0; a < 5; ++a) {
        float ax = axis[a][0], ay = axis[a][1], az = axis[a][2];
        float len = sqrtf(ax * ax + ay * ay + az * az);
        ax /= len; ay /= len; az /= len;
        for (size_t th = 0; th < 6; ++th) {
            float half = 0.5f * angles[th];
            float sh = sinf(half), ch = cosf(half);
            float x = ax * sh, y = ay * sh, z = az * sh, w = ch;
            float mat[12] = {
                1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),     99.0f,
                2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),     88.0f,
                2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y), 77.0f,
            };
            ref_rot3x3_to_quat(q_ref, mat);
            fn(q_got, mat);
            for (int i = 0; i < 4; ++i) assert(near_f32(q_got[i], q_ref[i], 1e-6f));

            /* Verify aliasing out == mat works safely */
            float alias_mat[12];
            memcpy(alias_mat, mat, sizeof(alias_mat));
            fn(alias_mat, alias_mat);
            for (int i = 0; i < 4; ++i) assert(near_f32(alias_mat[i], q_ref[i], 1e-6f));
        }
    }
}

static void init_bone_node(uint8_t *nodes_base, uint64_t idx, uint64_t tx_idx,
                           uint64_t first_child, int32_t child_count) {
    uint8_t *n = nodes_base + idx * 0xb0ULL;
    memcpy(n + 0x88, &first_child, sizeof(first_child));
    memcpy(n + 0x90, &child_count, sizeof(child_count));
    memcpy(n + 0x98, &tx_idx, sizeof(tx_idx));
}

static void init_bone_local(uint8_t *tx_base, uint64_t tx_idx,
                            const float lv[4], const float lr[4]) {
    uint8_t *e = tx_base + tx_idx * 0x60ULL;
    memcpy(e + 0x30, lv, 16);
    memcpy(e + 0x40, lr, 16);
}

static void test_bone_propagate(const RecordedHook *hk) {
    assert(hk && hk->kind == GUEST_REPLACE_PTR3_V128X2);
    void (*fn)(uintptr_t, uintptr_t, uintptr_t, const float *, const float *) =
        (void (*)(uintptr_t, uintptr_t, uintptr_t, const float *, const float *))hk->host_fn;

    /* 7-bone tree:
     * 0 (3 children: 1,2,3)
     * 1 (1 child: 4 -> linear chain to leaf 5)
     * 2 (0 children leaf)
     * 3 (-1 child_count leaf edge case)
     * 4 (1 child: 5)
     * 5 (0 children)
     * 6 untouched sentinel
     */
    uint8_t nodes[7 * 0xb0];
    memset(nodes, 0, sizeof(nodes));
    init_bone_node(nodes, 0, 0, 1, 3);
    init_bone_node(nodes, 1, 1, 4, 1);
    init_bone_node(nodes, 2, 2, 0, 0);
    init_bone_node(nodes, 3, 3, 0, -1);
    init_bone_node(nodes, 4, 4, 5, 1);
    init_bone_node(nodes, 5, 5, 0, 0);
    init_bone_node(nodes, 6, 6, 0, 0);

    uint8_t tx_a[7 * 0x60], tx_b[7 * 0x60];
    memset(tx_a, 0x3c, sizeof(tx_a));
    for (uint64_t i = 0; i < 7; ++i) {
        float lv[4] = {(float)(i + 1) * 0.5f, -0.25f * (float)i, 1.5f, 0.1f * (float)(i + 1)};
        float a = 0.2f * (float)(i + 1);
        float lr[4] = {sinf(a) * 0.6f, sinf(a) * 0.8f, 0.0f, cosf(a)};
        init_bone_local(tx_a, i, lv, lr);
    }
    memcpy(tx_b, tx_a, sizeof(tx_b));

    float root_p[4] = {10.0f, -20.0f, 30.0f, 1.5f};
    float root_q[4] = {0.2f, -0.4f, 0.4f, sqrtf(1.0f - 0.04f - 0.16f - 0.16f)};

    ref_bone_propagate(nodes, nodes, tx_a, root_p, root_q);
    fn((uintptr_t)nodes, (uintptr_t)nodes, (uintptr_t)tx_b, root_p, root_q);

    for (uint64_t i = 0; i < 6; ++i) {
        float pa[4], pb[4], ra[4], rb[4];
        memcpy(pa, tx_a + i * 0x60 + 0x00, 16);
        memcpy(pb, tx_b + i * 0x60 + 0x00, 16);
        memcpy(ra, tx_a + i * 0x60 + 0x10, 16);
        memcpy(rb, tx_b + i * 0x60 + 0x10, 16);
        for (int c = 0; c < 4; ++c) {
            assert(near_f32(pa[c], pb[c], 1e-4f));
            assert(near_f32(ra[c], rb[c], 1e-5f));
        }
    }
    /* Node 6 untouched */
    assert(memcmp(tx_a + 6 * 0x60, tx_b + 6 * 0x60, 0x60) == 0);
}

static void test_oodle_hash48(const RecordedHook *hk, unsigned char *img) {
    assert(hk && hk->kind == GUEST_REPLACE_INT6);
    uint64_t (*fn)(const uint8_t *, size_t) = (uint64_t (*)(const uint8_t *, size_t))hk->host_fn;

    static const size_t lens[] = {0, 1, 15, 47, 48, 49, 95, 96, 383, 384, 385, 500, 1024};
    uint8_t buf[1050];
    uint32_t rng = 0xabcdef01u;
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = (uint8_t)xorshift32(&rng);

    for (unsigned seed_round = 0; seed_round < 2; ++seed_round) {
        if (seed_round == 1) {
            seed_oodle_bss(img, 0x13579bdfu, 0x2468ace0u, 0xfedcba98u);
        }
        for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); ++k) {
            size_t len = lens[k];
            for (size_t align = 0; align <= 7; align += (align == 0 ? 1 : 2)) {
                const uint8_t *p = buf + align;
                uint64_t got = fn(p, len);
                uint64_t exp = ref_oodle_hash48(img, p, len);
                assert(got == exp);
            }
        }
    }
}

int main(void) {
    test_import_names_coverage();

    const size_t img_size = 0x1000000;
    unsigned char *img = calloc(1, img_size);
    assert(img != NULL);
    seed_prologues(img);
    seed_oodle_bss(img, 0xdeadbeefu, 0x206f85b3u, 0x5768b525u);

    unsetenv("GOW3_NATIVE_CPU_MATH");
    unsetenv("GOW3_NATIVE_CPU_KERNELS");
    recorded_count = 0;
    unsigned installed = runtime_native_cpu_install((uintptr_t)img, img_size, img_size);
    assert(installed == 12);
    assert(recorded_count == 12);

    const RecordedHook *sc = find_recorded("native:sincosf");
    assert(sc && sc->kind == GUEST_REPLACE_F32_I32_TO_F32 && sc->address == (uintptr_t)(img + 0xa56820));
    float (*fn_sincos)(float, int32_t) = (float (*)(float, int32_t))sc->host_fn;
    assert(near_f32(fn_sincos(0.0f, 0), 0.0f, 1e-6f));
    assert(near_f32(fn_sincos(0.0f, 1), 1.0f, 1e-6f));
    assert(near_f32(fn_sincos(0.75f, 0), sinf(0.75f), 1e-6f));
    assert(near_f32(fn_sincos(0.75f, 1), cosf(0.75f), 1e-6f));
    assert(near_f32(fn_sincos(-1.25f, 0), sinf(-1.25f), 1e-6f));
    assert(near_f32(fn_sincos(-1.25f, 2), cosf(-1.25f), 1e-6f));
    assert(isnan(fn_sincos(NAN, 0)) && isnan(fn_sincos(NAN, 1)));

    const RecordedHook *pw = find_recorded("native:powf");
    assert(pw && pw->kind == GUEST_REPLACE_F32_F32_TO_F32);
    float (*fn_pow)(float, float) = (float (*)(float, float))pw->host_fn;
    assert(near_f32(fn_pow(2.0f, 3.5f), powf(2.0f, 3.5f), 1e-5f));
    assert(near_f32(fn_pow(-2.0f, 3.0f), -8.0f, 1e-6f));
    assert(near_f32(fn_pow(5.0f, 0.0f), 1.0f, 1e-6f));

    const RecordedHook *ac = find_recorded("native:acosf");
    assert(ac && ac->kind == GUEST_REPLACE_F32_TO_F32);
    float (*fn_acos)(float) = (float (*)(float))ac->host_fn;
    assert(near_f32(fn_acos(0.5f), acosf(0.5f), 1e-6f));
    assert(near_f32(fn_acos(1.0f), 0.0f, 1e-6f));
    assert(near_f32(fn_acos(-1.0f), acosf(-1.0f), 1e-6f));
    assert(isnan(fn_acos(2.0f)));

    const RecordedHook *ex1 = find_recorded("native:expf_scalar");
    const RecordedHook *ex2 = find_recorded("native:expf_table");
    assert(ex1 && ex2 && ex1->kind == GUEST_REPLACE_F32_TO_F32 && ex2->kind == GUEST_REPLACE_F32_TO_F32);
    float (*fn_exp1)(float) = (float (*)(float))ex1->host_fn;
    float (*fn_exp2)(float) = (float (*)(float))ex2->host_fn;
    assert(near_f32(fn_exp1(0.0f), 1.0f, 1e-6f));
    assert(near_f32(fn_exp2(1.0f), expf(1.0f), 1e-5f));
    assert(near_f32(fn_exp1(-1000.0f), 0.0f, 1e-6f));

    const RecordedHook *fm = find_recorded("native:fmodf");
    assert(fm && fm->kind == GUEST_REPLACE_F32_F32_TO_F32);
    float (*fn_fmod)(float, float) = (float (*)(float, float))fm->host_fn;
    assert(near_f32(fn_fmod(7.5f, 2.0f), 1.5f, 1e-6f));
    assert(near_f32(fn_fmod(-5.5f, 2.0f), -1.5f, 1e-6f));

    const RecordedHook *tn = find_recorded("native:tanf");
    assert(tn && tn->kind == GUEST_REPLACE_F32_TO_F32);
    assert(near_f32(((float (*)(float))tn->host_fn)(0.5f), tanf(0.5f), 1e-6f));

    const RecordedHook *as = find_recorded("native:asinf");
    assert(as && as->kind == GUEST_REPLACE_F32_TO_F32);
    assert(near_f32(((float (*)(float))as->host_fn)(0.5f), asinf(0.5f), 1e-6f));

    /* Phase 2 kernel differential tests */
    test_filter_8x4(find_recorded("native:filter_8x4_u8"));
    test_rot3x3_to_quat(find_recorded("native:rot3x3_to_quat"));
    test_bone_propagate(find_recorded("native:bone_propagate"));
    test_oodle_hash48(find_recorded("native:oodle_hash48"), img);

    /* Corrupt 1 byte at offset +20 of rot3x3_to_quat (proves 24-byte uniqueness check): skips 1. */
    img[0x971520 + 20] ^= 0xff;
    recorded_count = 0;
    assert(runtime_native_cpu_install((uintptr_t)img, img_size, img_size) == 11);
    assert(find_recorded("native:rot3x3_to_quat") == NULL);

    /* Image too small for oodle_hash48 .bss seeds: skips oodle_hash48 safely -> 11/12. */
    seed_prologues(img);
    recorded_count = 0;
    assert(runtime_native_cpu_install((uintptr_t)img, img_size, 0xf0b5a0) == 11);
    assert(find_recorded("native:oodle_hash48") == NULL);

    /* Truncate exec_size across sincosf entry: skips sincosf, tanf, powf, bone_propagate -> leaves 8. */
    recorded_count = 0;
    assert(runtime_native_cpu_install((uintptr_t)img, 0xa56820 + 10, img_size) == 8);

    /* Independent env opt-outs */
    setenv("GOW3_NATIVE_CPU_KERNELS", "0", 1);
    recorded_count = 0;
    assert(runtime_native_cpu_install((uintptr_t)img, img_size, img_size) == 8);
    unsetenv("GOW3_NATIVE_CPU_KERNELS");

    setenv("GOW3_NATIVE_CPU_MATH", "0", 1);
    recorded_count = 0;
    assert(runtime_native_cpu_install((uintptr_t)img, img_size, img_size) == 4);
    unsetenv("GOW3_NATIVE_CPU_MATH");

    free(img);
    puts("test_native_cpu: OK");
    return 0;
}
