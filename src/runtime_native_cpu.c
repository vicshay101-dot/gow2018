/* Native host replacements for hot statically-linked math/CPU routines inside eboot.bin.
 * Each candidate matches an exact 16-byte RIP-relative prologue before patching, so another title
 * or binary version skips cleanly. */
#include "runtime.h"
#include "guest_cpu.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int native_math_disabled(void) {
    const char *env = getenv("GOW3_NATIVE_CPU_MATH");
    if (env && env[0] == '0') return 1;
#ifndef _WIN32
    FILE *flag = fopen("/sdcard/Download/gow2018_no_native_math", "r");
    if (flag) {
        fclose(flag);
        return 1;
    }
#endif
    return 0;
}

static float native_sincosf_dispatch(float x, int32_t is_cos) {
    return is_cos ? cosf(x) : sinf(x);
}
static float native_powf(float x, float y) { return powf(x, y); }
static float native_acosf(float x) { return acosf(x); }
static float native_expf(float x) { return expf(x); }
static float native_fmodf(float x, float y) { return fmodf(x, y); }
static float native_tanf(float x) { return tanf(x); }
static float native_asinf(float x) { return asinf(x); }

typedef struct {
    uint64_t offset;
    void *fn;
    GuestReplaceKind kind;
    const char *name;
    unsigned char expected[16];
} NativeReplacementEntry;

unsigned runtime_native_cpu_install(uintptr_t image_base, uint64_t exec_size) {
    static const NativeReplacementEntry kMathTable[] = {
        {
            0xa56820ULL, (void *)native_sincosf_dispatch, GUEST_REPLACE_F32_I32_TO_F32, "native:sincosf",
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xce, 0xab},
        },
        {
            0xc36300ULL, (void *)native_powf, GUEST_REPLACE_F32_F32_TO_F32, "native:powf",
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xee, 0xb0},
        },
        {
            0xa53220ULL, (void *)native_acosf, GUEST_REPLACE_F32_TO_F32, "native:acosf",
            {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x1d, 0xd0, 0xe1, 0x3f, 0x00},
        },
        {
            0xa53fe0ULL, (void *)native_expf, GUEST_REPLACE_F32_TO_F32, "native:expf_scalar",
            {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x1d, 0x10, 0xd4, 0x3f, 0x00},
        },
        {
            0xa55900ULL, (void *)native_expf, GUEST_REPLACE_F32_TO_F32, "native:expf_table",
            {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x1d, 0xf0, 0xba, 0x3f, 0x00},
        },
        {
            0xa545b0ULL, (void *)native_fmodf, GUEST_REPLACE_F32_F32_TO_F32, "native:fmodf",
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b, 0x35, 0x3e, 0xce},
        },
        {
            0xa57080ULL, (void *)native_tanf, GUEST_REPLACE_F32_TO_F32, "native:tanf",
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x6e, 0xa3},
        },
        {
            0xa53790ULL, (void *)native_asinf, GUEST_REPLACE_F32_TO_F32, "native:asinf",
            {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x5e, 0xdc},
        },
    };
    const unsigned total = (unsigned)(sizeof(kMathTable) / sizeof(kMathTable[0]));
    if (!image_base || !exec_size || native_math_disabled()) {
        printf("Native CPU math: disabled or unavailable (0/%u)\n", total);
        return 0;
    }
    unsigned installed = 0;
    for (unsigned i = 0; i < total; ++i) {
        const NativeReplacementEntry *e = &kMathTable[i];
        if (e->offset + sizeof(e->expected) > exec_size) continue;
        if (guest_cpu_replace_function(image_base + e->offset, e->fn, e->kind,
                                       e->expected, sizeof(e->expected), e->name)) {
            ++installed;
        }
    }
    printf("Native CPU math: %u/%u function(s) installed\n", installed, total);
    return installed;
}
