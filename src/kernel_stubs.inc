/* kernel_stubs.inc: stubs de libkernel. Formato de linha igual ao import_names.inc.
 * Para adicionar uma funcao: crie a funcao no bloco FUNCOES e uma linha no bloco TABELA.
 * Modo 1 (padrao): define as funcoes. Modo 2 (com KSTUB_ENTRIES definido): gera a tabela. */
#ifndef KSTUB_ENTRIES

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

#endif
