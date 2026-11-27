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

static RecordedHook recorded[32];
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
    static const struct { size_t off; unsigned char bytes[16]; } seeds[] = {
        {0xa56820, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xce, 0xab}},
        {0xc36300, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xee, 0xb0}},
        {0xa53220, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x1d, 0xd0, 0xe1, 0x3f, 0x00}},
        {0xa53fe0, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x1d, 0x10, 0xd4, 0x3f, 0x00}},
        {0xa55900, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x1d, 0xf0, 0xba, 0x3f, 0x00}},
        {0xa545b0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b, 0x35, 0x3e, 0xce}},
        {0xa57080, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x6e, 0xa3}},
        {0xa53790, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x5e, 0xdc}},
    };
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        memcpy(img + seeds[i].off, seeds[i].bytes, 16);
    }
}

int main(void) {
    test_import_names_coverage();

    const size_t img_size = 0xd00000;
    unsigned char *img = calloc(1, img_size);
    assert(img != NULL);
    seed_prologues(img);

    unsetenv("GOW3_NATIVE_CPU_MATH");
    recorded_count = 0;
    unsigned installed = runtime_native_cpu_install((uintptr_t)img, img_size);
    assert(installed == 8);
    assert(recorded_count == 8);

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

    /* Corrupt 1 byte of sincosf prologue: must skip safely. */
    img[0xa56820 + 13] ^= 0xff;
    recorded_count = 0;
    assert(runtime_native_cpu_install((uintptr_t)img, img_size) == 7);
    assert(find_recorded("native:sincosf") == NULL);

    /* Truncate exec_size across sincosf entry: skips sincosf, tanf, powf -> leaves 5. */
    seed_prologues(img);
    recorded_count = 0;
    assert(runtime_native_cpu_install((uintptr_t)img, 0xa56820 + 10) == 5);

    /* Env opt-out. */
    setenv("GOW3_NATIVE_CPU_MATH", "0", 1);
    recorded_count = 0;
    assert(runtime_native_cpu_install((uintptr_t)img, img_size) == 0);
    assert(recorded_count == 0);
    unsetenv("GOW3_NATIVE_CPU_MATH");

    free(img);
    puts("test_native_cpu: OK");
    return 0;
}
