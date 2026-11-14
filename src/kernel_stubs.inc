/* kernel_stubs.inc: stubs de libkernel. Formato de linha igual ao import_names.inc.
 * Para adicionar uma funcao: crie a funcao no bloco FUNCOES e uma linha no bloco TABELA.
 * Modo 1 (padrao): define as funcoes. Modo 2 (com KSTUB_ENTRIES definido): gera a tabela. */
#ifndef KSTUB_ENTRIES
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

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

/* ================= eboot imports (God of War 2018): kernel, posix, offline services ================= */
#define KERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))   /* SCE_KERNEL_ERROR_* = 0x80020000|errno */
#define KNP_SIGNED_OUT ((int32_t)0x80550006)             /* same offline answer runtime_services.c gives */
#define KHTTP_NETWORK ((int32_t)0x80431063)
#define KSYSTEM_PARAMETER ((int32_t)0x80A10003)

static ABI int32_t kernel_ok(void) { return 0; }

/* ---- event flags: 64-bit handle = id; one global condvar, broadcast on every change ---- */
typedef struct KEvf { uint64_t id, bits; int deleted; struct KEvf *next; } KEvf;
static pthread_mutex_t kevf_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kevf_cond=PTHREAD_COND_INITIALIZER;
static KEvf *kevf_list;
static uint64_t kevf_next=1;
static KEvf *kevf_find(uint64_t id) {
    for (KEvf *e=kevf_list;e;e=e->next) if (e->id==id && !e->deleted) return e;
    return NULL;
}
static ABI int32_t kernel_evf_create(uint64_t *out,const char *name,uint32_t attr,uint64_t initial,const void *opt) {
    (void)name; (void)attr; (void)opt;
    if (!out) return KERR(22);
    KEvf *e=calloc(1,sizeof(*e));
    if (!e) return KERR(12);
    pthread_mutex_lock(&kevf_lock);
    e->id=kevf_next++; e->bits=initial; e->next=kevf_list; kevf_list=e; *out=e->id;
    pthread_mutex_unlock(&kevf_lock);
    return 0;
}
static ABI int32_t kernel_evf_delete(uint64_t id) {
    pthread_mutex_lock(&kevf_lock);
    KEvf *e=kevf_find(id);
    if (!e) { pthread_mutex_unlock(&kevf_lock); return KERR(3); }
    e->deleted=1;                       /* node is kept (tiny leak) so woken waiters never touch freed memory */
    pthread_cond_broadcast(&kevf_cond);
    pthread_mutex_unlock(&kevf_lock);
    return 0;
}
static ABI int32_t kernel_evf_set(uint64_t id,uint64_t pattern) {
    pthread_mutex_lock(&kevf_lock);
    KEvf *e=kevf_find(id);
    if (!e) { pthread_mutex_unlock(&kevf_lock); return KERR(3); }
    e->bits|=pattern;
    pthread_cond_broadcast(&kevf_cond);
    pthread_mutex_unlock(&kevf_lock);
    return 0;
}
#define KEVF_AND 0x01
#define KEVF_OR 0x02
#define KEVF_CLEAR_ALL 0x10
#define KEVF_CLEAR_PAT 0x20
static ABI int32_t kernel_evf_wait(uint64_t id,uint64_t pattern,uint32_t mode,uint64_t *result,uint32_t *timeout_us) {
    int and_=!!(mode&KEVF_AND), or_=!!(mode&KEVF_OR);
    if (!pattern || and_==or_) return KERR(22);     /* exactly one of AND/OR */
    struct timespec deadline;
    if (timeout_us) {
        clock_gettime(CLOCK_REALTIME,&deadline);
        deadline.tv_sec+=*timeout_us/1000000;
        deadline.tv_nsec+=(long)(*timeout_us%1000000)*1000L;
        if (deadline.tv_nsec>=1000000000L) { ++deadline.tv_sec; deadline.tv_nsec-=1000000000L; }
    }
    int first=1;
    pthread_mutex_lock(&kevf_lock);
    for (;;) {
        KEvf *e=kevf_find(id);
        if (!e) { pthread_mutex_unlock(&kevf_lock); return first ? KERR(3) : KERR(89); }  /* ESRCH / ECANCELED */
        first=0;
        uint64_t b=e->bits;
        if (and_ ? (b&pattern)==pattern : (b&pattern)!=0) {
            if (result) *result=b;
            if (mode&KEVF_CLEAR_ALL) e->bits=0; else if (mode&KEVF_CLEAR_PAT) e->bits&=~pattern;
            pthread_mutex_unlock(&kevf_lock);
            return 0;
        }
        if (timeout_us) {
            if (pthread_cond_timedwait(&kevf_cond,&kevf_lock,&deadline)==ETIMEDOUT) {
                if (result) *result=e->bits;
                *timeout_us=0;
                pthread_mutex_unlock(&kevf_lock);
                return KERR(60);                          /* ETIMEDOUT */
            }
        } else pthread_cond_wait(&kevf_cond,&kevf_lock);
    }
}

/* ---- POSIX semaphores: the guest sem_t just stores a pointer to a host object ---- */
typedef struct KPsem { pthread_mutex_t m; pthread_cond_t c; uint32_t value; } KPsem;
static ABI int32_t kernel_sem_init(KPsem **sem,int pshared,uint32_t value) {
    (void)pshared;
    if (!sem) return -1;
    KPsem *s=calloc(1,sizeof(*s));
    if (!s) return -1;
    pthread_mutex_init(&s->m,NULL); pthread_cond_init(&s->c,NULL); s->value=value;
    *sem=s;
    return 0;
}
static ABI int32_t kernel_sem_destroy(KPsem **sem) {
    if (!sem || !*sem) return -1;
    KPsem *s=*sem; *sem=NULL;
    pthread_mutex_destroy(&s->m); pthread_cond_destroy(&s->c); free(s);
    return 0;
}
static ABI int32_t kernel_sem_post(KPsem **sem) {
    if (!sem || !*sem) return -1;
    KPsem *s=*sem;
    pthread_mutex_lock(&s->m); ++s->value; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m);
    return 0;
}
static ABI int32_t kernel_sem_wait(KPsem **sem) {
    if (!sem || !*sem) return -1;
    KPsem *s=*sem;
    pthread_mutex_lock(&s->m);
    while (!s->value) pthread_cond_wait(&s->c,&s->m);
    --s->value;
    pthread_mutex_unlock(&s->m);
    return 0;
}
static ABI int32_t kernel_sem_timedwait(KPsem **sem,const struct timespec *abs) {   /* abs: CLOCK_REALTIME deadline */
    if (!sem || !*sem || !abs) return -1;
    KPsem *s=*sem;
    pthread_mutex_lock(&s->m);
    while (!s->value) {
        if (pthread_cond_timedwait(&s->c,&s->m,abs)==ETIMEDOUT && !s->value) { pthread_mutex_unlock(&s->m); return -1; }
    }
    --s->value;
    pthread_mutex_unlock(&s->m);
    return 0;
}

/* ---- offline sockets: every call fails (the game must already cope: sceNet* says "unreachable") ---- */
static ABI int64_t kernel_sock_fail(void) { return -1; }

/* ---- misc kernel ---- */
static uint64_t kuuid_state;
static ABI int32_t kernel_uuid_create(uint8_t *uuid) {
    if (!uuid) return KERR(22);
    struct timespec t; clock_gettime(CLOCK_REALTIME,&t);
    if (!kuuid_state) kuuid_state=(uint64_t)t.tv_nsec*6364136223846793005ULL+(uint64_t)t.tv_sec+1;
    for (int i=0;i<16;++i) { kuuid_state^=kuuid_state<<13; kuuid_state^=kuuid_state>>7; kuuid_state^=kuuid_state<<17; uuid[i]=(uint8_t)kuuid_state; }
    uuid[6]=(uuid[6]&0x0f)|0x40; uuid[8]=(uuid[8]&0x3f)|0x80;   /* RFC 4122 v4 */
    return 0;
}
static ABI int32_t kernel_is_stack(const void *addr,void **start,void **end) {
    (void)addr; (void)start; (void)end;
    return KERR(22);                    /* "not a stack address": the safe answer */
}
static ABI int32_t kernel_dlsym(int32_t handle,const char *name,void **out) {
    (void)handle; (void)out;
    printf("Runtime: sceKernelDlsym('%s') -> not found\n",name?name:"(null)");
    return KERR(2);
}
static ABI int32_t kernel_load_start_module(const char *path,uint64_t args,const void *argp,uint32_t flags,const void *opt,int32_t *res) {
    (void)args; (void)argp; (void)flags; (void)opt; (void)res;
    printf("Runtime: sceKernelLoadStartModule('%s') -> not found\n",path?path:"(null)");
    return KERR(2);
}
static ABI int32_t kernel_pthread_getname(void *thread,char *name) {
    (void)thread;
    if (name) strcpy(name,"thread");
    return 0;
}
static ABI int32_t kernel_pthread_getschedparam(void *thread,int32_t *policy,int32_t *param) {
    (void)thread;
    if (policy) *policy=1;              /* SCHED_FIFO, the PS4 default */
    if (param) *param=700;              /* default PS4 priority */
    return 0;
}

/* ---- offline services (same answers runtime_services.c uses) ---- */
static ABI int32_t kernel_np_signed_out(void) { return KNP_SIGNED_OUT; }
static ABI int32_t kernel_http_network(void) { return KHTTP_NETWORK; }
static ABI int32_t kernel_system_parameter(void) { return KSYSTEM_PARAMETER; }
static ABI int32_t kernel_net_get_mac(uint8_t *mac,int32_t flags) {
    (void)flags;
    if (!mac) return -1;
    static const uint8_t fake[6]={0x02,0x00,0x00,0x00,0x00,0x01};   /* locally administered */
    memcpy(mac,fake,6);
    return 0;
}
static ABI int32_t kernel_playgo_todolist(uint32_t h,void *list,uint32_t n,uint32_t *out) {
    (void)h; (void)list; (void)n;
    if (out) *out=0;                    /* nothing left to install */
    return 0;
}
static ABI int32_t kernel_playgo_eta(uint32_t h,const void *chunks,uint32_t n,uint64_t *eta) {
    (void)h; (void)chunks; (void)n;
    if (eta) *eta=0;
    return 0;
}
static ABI int32_t kernel_playgo_language_mask(uint32_t h,uint64_t *mask) {
    (void)h;
    if (mask) *mask=~UINT64_C(0);       /* every language available */
    return 0;
}

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

{"sceKernelCreateEventFlag",kernel_evf_create},
{"sceKernelDeleteEventFlag",kernel_evf_delete},
{"sceKernelSetEventFlag",kernel_evf_set},
{"sceKernelWaitEventFlag",kernel_evf_wait},
{"sem_init",kernel_sem_init},
{"sem_destroy",kernel_sem_destroy},
{"sem_post",kernel_sem_post},
{"sem_wait",kernel_sem_wait},
{"sem_timedwait",kernel_sem_timedwait},
{"connect",kernel_sock_fail},
{"recv",kernel_sock_fail},
{"send",kernel_sock_fail},
{"select",kernel_sock_fail},
{"shutdown",kernel_sock_fail},
{"sceKernelUuidCreate",kernel_uuid_create},
{"sceKernelIsStack",kernel_is_stack},
{"sceKernelDlsym",kernel_dlsym},
{"sceKernelLoadStartModule",kernel_load_start_module},
{"scePthreadGetname",kernel_pthread_getname},
{"scePthreadGetschedparam",kernel_pthread_getschedparam},
{"scePthreadSetschedparam",kernel_ok},
{"pthread_setschedparam",kernel_ok},
{"scePthreadCancel",kernel_ok},
{"sceKernelGetGPI",kernel_ok},
{"sceKernelGetPageTableStats",kernel_ok},
{"sceKernelMtypeprotect",kernel_ok},
{"sceKernelSetPrtAperture",kernel_ok},
{"sceKernelSetVirtualRangeName",kernel_ok},
{"sceCoredumpAttachMemoryRegion",kernel_ok},
{"sceCoredumpRegisterCoredumpHandler",kernel_ok},
{"sceCoredumpWriteUserData",kernel_ok},
{"scePlayGoDialogOpen",kernel_ok},
{"scePlayGoSetLanguageMask",kernel_ok},
{"scePlayGoGetToDoList",kernel_playgo_todolist},
{"scePlayGoGetEta",kernel_playgo_eta},
{"scePlayGoGetLanguageMask",kernel_playgo_language_mask},
{"sceHttpUriEscape",kernel_http_network},
{"sceNpGetAccountCountryA",kernel_np_signed_out},
{"sceNpGetAccountDateOfBirthA",kernel_np_signed_out},
{"sceNpGetAccountIdA",kernel_np_signed_out},
{"sceNpGetAccountLanguageA",kernel_np_signed_out},
{"sceNpTssGetData",kernel_np_signed_out},
{"sceNetGetMacAddress",kernel_net_get_mac},
{"sceSystemServiceGetPlatformPrivacySetting",kernel_system_parameter},

#endif
