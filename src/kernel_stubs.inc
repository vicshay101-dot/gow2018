/* kernel_stubs.inc: stubs de libkernel. Formato de linha igual ao import_names.inc.
 * Para adicionar uma funcao: crie a funcao no bloco FUNCOES e uma linha no bloco TABELA.
 * Modo 1 (padrao): define as funcoes. Modo 2 (com KSTUB_ENTRIES definido): gera a tabela. */
#ifndef KSTUB_ENTRIES
#include <stdlib.h>
#include <unistd.h>

static ABI int32_t kernel_get_sanitizer_malloc_replace(void *out) { (void)out; return 0; }
static ABI int32_t kernel_get_sanitizer_malloc_replace_external(void *out) { (void)out; return 0; }
static ABI int32_t kernel_get_sanitizer_new_replace(void *out) { (void)out; return 0; }
static ABI int32_t kernel_get_sanitizer_new_replace_external(void *out) { (void)out; return 0; }
static ABI int32_t kernel_is_address_sanitizer_enabled(void) { return 0; }
static ABI int32_t kernel_map_sanitizer_shadow_memory(void *a, void *b) { (void)a; (void)b; return 0; }
static ABI int32_t kernel_rtld_thread_atexit_increment(void) { return 0; }
static ABI int32_t kernel_rtld_thread_atexit_decrement(void) { return 0; }
static ABI int32_t kernel_rtld_set_application_heap_api(void *api) { (void)api; return 0; }
static ABI int32_t kernel_get_libkernel_text_location(void *out) { (void)out; return 0; }

static ABI void *kernel_libc_heap_mutex_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
static ABI void kernel_libc_heap_mutex_free(void *ptr) { free(ptr); }
/* Sem trace de heap ativo: responde erro (-1) e nao preenche a saida. A libc so usa a saida
 * quando recebe sucesso; antes, sucesso sem dados causava NULL deref logo apos a chamada. */
/* A libc chama com uma struct de 32 bytes. Os campos +0x10 e +0x18 viram ponteiros globais
 * (0xf90a0 e 0xf9050) e sao escritos logo depois; NULL ai causa o signal 11. O runtime usa
 * espaco de enderecos plano, entao um buffer estatico do host e valido para o convidado. */
static uint64_t libc_trace_block[8];
static ABI int32_t kernel_libc_heap_get_trace_info(void *out) {
    uint64_t *q = out;
    q[2] = (uint64_t)(uintptr_t)libc_trace_block;
    q[3] = (uint64_t)(uintptr_t)libc_trace_block;
    return 0;
}
static ABI int32_t kernel_libc_heap_get_address_ranges(void *out) { (void)out; return -1; }
static void *heap_range_callback;
static ABI int32_t kernel_libc_heap_set_address_range_callback(void *cb) { heap_range_callback=cb; return 0; }
static ABI int32_t kernel_libc_heap_set_trace_marker(void *marker) { (void)marker; return 0; }
static ABI int32_t kernel_libc_heap_unset_trace_marker(void) { return 0; }

static ABI int32_t kernel_get_compiled_sdk_version(int32_t *version) { if (version) *version=0x05008001; return 0; }
static ABI int32_t kernel_is_neo_mode(void) { return 0; }
static ABI int32_t kernel_get_current_cpu(void) { return 0; }
static ABI uint32_t kernel_sleep(uint32_t seconds) { sleep(seconds); return 0; }

#else

/* TABELA: uma linha por funcao, formato {"nome","funcao"} */
{"sceKernelGetSanitizerMallocReplace",kernel_get_sanitizer_malloc_replace},
{"sceKernelGetSanitizerMallocReplaceExternal",kernel_get_sanitizer_malloc_replace_external},
{"sceKernelGetSanitizerNewReplace",kernel_get_sanitizer_new_replace},
{"sceKernelGetSanitizerNewReplaceExternal",kernel_get_sanitizer_new_replace_external},
{"sceKernelIsAddressSanitizerEnabled",kernel_is_address_sanitizer_enabled},
{"sceKernelMapSanitizerShadowMemory",kernel_map_sanitizer_shadow_memory},
{"_sceKernelRtldThreadAtexitIncrement",kernel_rtld_thread_atexit_increment},
{"_sceKernelRtldThreadAtexitDecrement",kernel_rtld_thread_atexit_decrement},
{"_sceKernelRtldSetApplicationHeapAPI",kernel_rtld_set_application_heap_api},
{"sceKernelGetLibkernelTextLocation",kernel_get_libkernel_text_location},

{"sceLibcHeapGetAddressRanges",kernel_libc_heap_get_address_ranges},
{"sceLibcHeapGetTraceInfo",kernel_libc_heap_get_trace_info},
{"sceLibcHeapMutexCalloc",kernel_libc_heap_mutex_calloc},
{"sceLibcHeapMutexFree",kernel_libc_heap_mutex_free},
{"sceLibcHeapSetAddressRangeCallback",kernel_libc_heap_set_address_range_callback},
{"sceLibcHeapSetTraceMarker",kernel_libc_heap_set_trace_marker},
{"sceLibcHeapUnsetTraceMarker",kernel_libc_heap_unset_trace_marker},

{"sceKernelGetCompiledSdkVersion",kernel_get_compiled_sdk_version},
{"sceKernelIsNeoMode",kernel_is_neo_mode},
{"sceKernelGetCurrentCpu",kernel_get_current_cpu},
{"sceKernelSleep",kernel_sleep},

#endif
