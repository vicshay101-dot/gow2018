/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#include "runtime.h"
#include "guest_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <signal.h>
#include <ucontext.h>
#endif
#include <sys/time.h>
#ifndef _WIN32
#include <sys/random.h>
#include <sys/resource.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

extern int runtime_memory_is_mapped(uintptr_t address, uint64_t size);

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
static struct timespec process_start;
__attribute__((constructor)) static void remember_start(void) { clock_gettime(CLOCK_MONOTONIC,&process_start); }
static int host_clock(uint32_t id,clockid_t *out) {
    switch (id) {
    case 0: case 9: case 10: *out=CLOCK_REALTIME; return 1;            /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: *out=CLOCK_MONOTONIC; return 1; /* MONOTONIC/UPTIME */
    case 13: *out=CLOCK_REALTIME_COARSE; return 1;                    /* SECOND */
    case 14: *out=CLOCK_THREAD_CPUTIME_ID; return 1;
    case 2: case 15: *out=CLOCK_PROCESS_CPUTIME_ID; return 1;         /* PROF/PROCTIME */
    case 16: case 17: case 18: case 19: *out=CLOCK_MONOTONIC; return 1; /* PS4 network clocks */
    default: return 0;
    }
}
static int clock_read(uint32_t id,GuestTimespec *ts) {
    clockid_t host;
    if (!ts || !host_clock(id,&host)) return EINVAL;
    struct timespec t;
    if (clock_gettime(host,&t)) return errno;
    if (id==13) t.tv_nsec=0;
    ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *ts) {
    clockid_t host; struct timespec t;
    if (!host_clock(id,&host)) return fail_posix(EINVAL);
    if (clock_getres(host,&t)) return fail_posix(errno);
    if (ts) { ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; }
    return 0;
}
static ABI uint64_t process_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000+(uint64_t)((t.tv_nsec-process_start.tv_nsec)/1000);
}
static ABI uint64_t process_time_counter(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000000+(uint64_t)(t.tv_nsec-process_start.tv_nsec);
}
static ABI uint64_t process_time_frequency(void) { return 1000000000; }
/* RDTSC goes through the boundary: the host's counter on x86-64 hosts, the guest CPU's own on the
 * JIT (the game reads it for timing, so it must be the guest's view). */
static ABI uint64_t read_tsc(void) { return guest_cpu_tsc(); }
static ABI uint64_t tsc_frequency(void) { return guest_cpu_tsc_frequency(); }
/* Shared with the GPU library so flip/label timestamps use the guest's clock. */
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }
static int sleep_ns(uint64_t ns) {
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)};
    while (nanosleep(&t,&t)) if (errno!=EINTR) return errno;
#endif
    return 0;
}
/* Spin diagnostic: a guest thread that calls usleep many times in a row is polling for something.
 * Every 2000 calls, print the guest return address (the function doing the polling) for that thread.
 * Subtract the "Image base" line from the printed address to get the image offset. */
static __thread unsigned spin_calls;
/* The main thread polls the global A (image+0x1f98c70, int) from the loop whose usleep call
 * returns to image+0x2d114a. After 20000 polls on that loop, set A=1 once unless GOW3_NO_FORCE_A=1. */
#define SPIN_A_ADDR 0x80209cc70ULL
#define SPIN_A_CALLER 0x8003d514aULL
#ifndef _WIN32
static volatile sig_atomic_t watch_a_armed;
static uintptr_t watch_a_page;
static size_t watch_a_page_size;
static int watch_a_original_prot;
static int watch_a_ready;
static int watch_a_find_protection(uintptr_t address,int *prot) {
    FILE *maps=fopen("/proc/self/maps","r");
    if (!maps) return 0;
    char line[512],perm[5]; unsigned long lo,hi; int found=0;
    while (fgets(line,sizeof(line),maps)) {
        if (sscanf(line,"%lx-%lx %4s",&lo,&hi,perm)==3 && address>=lo && address<hi) {
            *prot=(perm[0]=='r'?PROT_READ:0)|(perm[1]=='w'?PROT_WRITE:0)|(perm[2]=='x'?PROT_EXEC:0);
            found=1; break;
        }
    }
    fclose(maps); return found;
}
static void arm_wait_flag_watch(uintptr_t caller) {
    /* Auto-arm disabled: only enable mprotect watch on 0x80209c000 if GOW3_WATCH_A=1 is explicitly set. */
    if (!getenv("GOW3_WATCH_A")) return;
    if (caller!=SPIN_A_CALLER || watch_a_armed) return;
    if (!watch_a_ready) {
        long ps=sysconf(_SC_PAGESIZE);
        if (ps<=0) return;
        watch_a_page_size=(size_t)ps;
        watch_a_page=SPIN_A_ADDR & ~(uintptr_t)(watch_a_page_size-1);
        if (!watch_a_find_protection(SPIN_A_ADDR,&watch_a_original_prot) || !(watch_a_original_prot&PROT_WRITE)) {
            fprintf(stderr,"GOW3_WATCH_A: cannot watch A; original protection=%d page=%p\n",watch_a_original_prot,(void*)watch_a_page);
            watch_a_ready=-1; return;
        }
        watch_a_ready=1;
    }
    if (watch_a_ready<0) return;
    static unsigned arms;
    if (mprotect((void*)watch_a_page,watch_a_page_size,watch_a_original_prot&~PROT_WRITE)==0) {
        watch_a_armed=1;
        if (++arms==1 || arms%10000==0)
            fprintf(stderr,"GOW3_WATCH_A: armed page=%p original_prot=%d arms=%u\n",(void*)watch_a_page,watch_a_original_prot,arms);
    }
}
int runtime_watch_handle_fault(int sig,siginfo_t *info,void *context) {
    if (sig!=SIGSEGV || !info || !watch_a_armed ||
        (((uintptr_t)info->si_addr)&~(uintptr_t)(watch_a_page_size-1))!=watch_a_page) return 0;
    GuestRegs regs={0};
    int guest=guest_cpu_signal_regs(context,&regs);
    uintptr_t hostpc=guest_cpu_host_pc(context);
    fprintf(stderr,"GOW3_WATCH_A: page write addr=%p guest=%d rip=%#llx image_off=%#llx hostpc=%p A=%d B=%u tid=%ld\n",
        info->si_addr,guest,(unsigned long long)regs.rip,
        (unsigned long long)(regs.rip>=0x800104000ULL?regs.rip-0x800104000ULL:0),
        (void*)hostpc,(int)*(volatile int32_t*)(uintptr_t)SPIN_A_ADDR,
        (unsigned)*(volatile uint8_t*)(uintptr_t)0x802b172e1ULL,(long)(uintptr_t)pthread_self());
    if (mprotect((void*)watch_a_page,watch_a_page_size,watch_a_original_prot)!=0) {
        fprintf(stderr,"GOW3_WATCH_A: failed to restore page protection errno=%d\n",errno);
        return 0;
    }
    watch_a_armed=0;
    return 1;
}
#else
static void arm_wait_flag_watch(uintptr_t caller) { (void)caller; }
#endif
static void maybe_force_wait_flag(uintptr_t caller) {
    static int forced;
    if (forced || getenv("GOW3_NO_FORCE_A")) return;
    if (caller!=SPIN_A_CALLER || spin_calls<20000) return;
    if (!runtime_memory_is_mapped(SPIN_A_ADDR,sizeof(int32_t))) return;
    volatile int32_t *flag_a=(volatile int32_t *)(uintptr_t)SPIN_A_ADDR;
    if (*flag_a==0) {
        *flag_a=1;
        forced=1;
        fprintf(stderr,"Spin: forced A=1 at 0x%llx after %u polls (caller=0x%lx)\n",
                (unsigned long long)SPIN_A_ADDR,spin_calls,(unsigned long)caller);
    }
}
static void note_usleep_spin(uintptr_t caller) {
    ++spin_calls;
    maybe_force_wait_flag(caller);
    if (spin_calls % 2000 != 0) return;
    static int shown;
    if (shown >= 80) return;
    ++shown;
    /* The spin waits on two image globals (image base 0x800104000): A = int at image+0x1f98c70,
     * B = byte at image+0x2a132e1. Printing them shows whether anything ever signals the wait. */
    int val_a = runtime_memory_is_mapped(SPIN_A_ADDR, sizeof(int32_t))
        ? (int)*(volatile int32_t *)(uintptr_t)SPIN_A_ADDR : -1;
    unsigned val_b = runtime_memory_is_mapped(0x802b172e1ULL, sizeof(uint8_t))
        ? (unsigned)*(volatile uint8_t *)(uintptr_t)0x802b172e1ULL : 255u;
    fprintf(stderr,"Spin: usleep x%u thread=%llu caller=0x%lx A=%d B=%u\n",spin_calls,
            (unsigned long long)(uintptr_t)pthread_self(),
            (unsigned long)caller,
            val_a,
            val_b);
}
static ABI int32_t kernel_usleep(uint32_t usec) {
    uintptr_t caller=guest_cpu_return_address((uintptr_t)__builtin_return_address(0));
    note_usleep_spin(caller); arm_wait_flag_watch(caller); sleep_ns((uint64_t)usec*1000); return 0;
}
static ABI int32_t posix_usleep(uint32_t usec) {
    uintptr_t caller=guest_cpu_return_address((uintptr_t)__builtin_return_address(0));
    note_usleep_spin(caller); arm_wait_flag_watch(caller); sleep_ns((uint64_t)usec*1000); return 0;
}
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t posix_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) return fail_posix(EINVAL);
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    if (rem) { rem->sec=0; rem->nsec=0; }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    return posix_nanosleep(rq,rem) ? ERR(*runtime_errno()) : 0;
}
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
    time_t now=time(NULL);
#ifdef _WIN32
    tz->minuteswest=(int32_t)(-compat_utc_offset(now)/60); tz->dsttime=0;
#else
    struct tm local; localtime_r(&now,&local);
    tz->minuteswest=(int32_t)(-local.tm_gmtoff/60); tz->dsttime=0;
#endif
    return 0;
}
/* sceKernelConvertUtcToLocaltime(time, *local, *timesec{t,west_sec,dst_sec}, *dst_sec) */
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } GuestTimesec;
static ABI int32_t kernel_utc_to_local(int64_t utc,int64_t *local,GuestTimesec *st,uint64_t *dst) {
    time_t t=(time_t)utc; struct tm tm;
#ifdef _WIN32
    int64_t off=compat_utc_offset(t); int isdst=0;
#else
    localtime_r(&t,&tm); int64_t off=tm.tm_gmtoff; int isdst=tm.tm_isdst>0;
#endif
    (void)tm;
    int64_t dst_sec=isdst ? 3600 : 0, west=off-dst_sec;
    if (local) *local=utc+off;
    if (st) { st->t=utc; st->west_sec=(uint32_t)west; st->dst_sec=(uint32_t)dst_sec; }
    if (dst) *dst=(uint64_t)dst_sec;
    return 0;
}
/* sceKernelConvertLocaltimeToUtc(time, unused, *utc, *timezone, *dst_seconds) */
static ABI int32_t kernel_local_to_utc(int64_t local,int64_t unused,int64_t *utc,GuestTimezone *tz,int32_t *dst) {
    (void)unused;
    if (!tz) return ERR(22);
    kernel_gettimezone(tz);
    if (utc) *utc=local-((int64_t)tz->minuteswest+tz->dsttime)*60;
    if (dst) *dst=tz->dsttime*60;
    return 0;
}
/* Unwinder queries about guest modules: no module info is exported. */
static ABI int32_t kernel_module_info_unwind(uint64_t address,int32_t flags,void *info) {
    (void)address; (void)flags; (void)info; return ERR(3);
}
static ABI int32_t kernel_module_segment_info(void *info) { (void)info; return ERR(3); }
static ABI int32_t posix_gettimeofday(GuestTimeval *tv,GuestTimezone *tz) {
    struct timeval t;
    gettimeofday(&t,NULL);
    if (tv) { tv->sec=t.tv_sec; tv->usec=t.tv_usec; }
    if (tz) kernel_gettimezone(tz);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv) return ERR(22);
    return posix_gettimeofday(tv,NULL);
}
static ABI int64_t posix_time(int64_t *out) { int64_t t=(int64_t)time(NULL); if (out) *out=t; return t; }

/* ---- process ---- */
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { sched_yield(); return 0; }
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n",status);
    runtime_report();
    fflush(stdout);
    _exit(status);
}
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code,uint64_t argument) {
    fprintf(stderr,"STOP: guest raised debug exception code=0x%x argument=0x%llx\n",code,(unsigned long long)argument);
    runtime_report();
    exit(23);
}
/* The process was started without arguments other than its own name. */
static const char *guest_argv[]={"eboot.bin",NULL};
static ABI int32_t get_argc(void) { return 1; }
static ABI const char **get_argv(void) { return guest_argv; }
/* Only the game and its bundled modules exist; nothing was loaded at run time. */
static ABI int32_t stop_unload_module(int32_t handle,uint64_t args,const void *argp,uint32_t flags,const void *opt,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)opt; (void)result;
    printf("Runtime: sceKernelStopUnloadModule(%d): no such module\n",handle);
    return ERR(3); /* ESRCH */
}
static ABI int32_t module_info_from_addr(uint64_t address,int32_t flags,void *info) {
    (void)address;
    if (flags>=3) return ERR(22);
    if (!info) return ERR(14);
    return ERR(3); /* ESRCH: the runtime keeps no module records */
}
static ABI int32_t kernel_mlock(const void *address,uint64_t size) { (void)address; (void)size; return 0; }
/* libc's C++ exception unwinder. The game does not throw (no __cxa_throw import): unwinding
 * finds no frame data, and resuming a context is a stop, never a silent success. */
static ABI int32_t elf_phdr_match_addr(void *info,uint64_t address) { (void)info; (void)address; return 0; }
static ABI int32_t is_signal_return(uint64_t pc) { (void)pc; return 0; }
static ABI void pthread_cxa_finalize(void *dso) { (void)dso; }
static ABI __attribute__((noreturn)) void unwind_resume(void) {
    fputs("STOP: guest C++ exception unwinding (sigreturn/__Ux86_64_setcontext) is not supported\n",stderr);
    runtime_report();
    exit(23);
}
static ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace (not available)\n",stderr);
    return 0;
}
typedef struct { uint32_t bits[4]; } GuestSigset;
static _Thread_local GuestSigset signal_mask;
static uintptr_t handlers[128];
static ABI uintptr_t guest_signal(int sig,uintptr_t handler) {
    if (sig<=0 || sig>=128) { *runtime_errno()=22; return (uintptr_t)-1; } /* SIG_ERR */
    uintptr_t old=handlers[sig]; handlers[sig]=handler;
    printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n",sig);
    return old;
}
static ABI int32_t guest_sigprocmask(int how,const GuestSigset *set,GuestSigset *old) {
    if (old) *old=signal_mask;
    if (!set) return 0;
    for (int i=0;i<4;++i) {
        if (how==1) signal_mask.bits[i]|=set->bits[i];        /* SIG_BLOCK */
        else if (how==2) signal_mask.bits[i]&=~set->bits[i];  /* SIG_UNBLOCK */
        else if (how==3) signal_mask.bits[i]=set->bits[i];    /* SIG_SETMASK */
        else return fail_posix(EINVAL);
    }
    return 0;
}
static ABI int32_t guest_sigfillset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0xff,sizeof(*set)); return 0; }
static ABI int32_t guest_sigemptyset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0,sizeof(*set)); return 0; }
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *out) {
    if (!out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    memset(out,0,sizeof(*out));
#ifdef _WIN32
    int64_t user=0,kernel=0;
    compat_cpu_times(who,&user,&kernel);
    out->utime=(GuestTimeval){user/1000000,user%1000000};
    out->stime=(GuestTimeval){kernel/1000000,kernel%1000000};
#else
    struct rusage r;
    getrusage(who==0 ? RUSAGE_SELF : RUSAGE_THREAD,&r);
    out->utime=(GuestTimeval){r.ru_utime.tv_sec,r.ru_utime.tv_usec};
    out->stime=(GuestTimeval){r.ru_stime.tv_sec,r.ru_stime.tv_usec};
#endif
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *name,uint32_t namelen,void *old,uint64_t *oldlen,const void *new_value,uint64_t newlen) {
    (void)newlen;
    if (!name || namelen<2 || new_value) return fail_posix(EINVAL);
    if (name[0]==1 && name[1]==37) { /* kern.arandom */
        if (!old || !oldlen) return fail_posix(EINVAL);
#ifdef _WIN32
        if (compat_random(old,(size_t)*oldlen)) return fail_posix(EIO);
#else
        if (getrandom(old,(size_t)*oldlen,0)<0) return fail_posix(errno);
#endif
        return 0;
    }
    if (name[0]==6 && (name[1]==7 || name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value=name[1]==7 ? PAGE : 7;
        if (old) { if (*oldlen<4) return fail_posix(ENOMEM); memcpy(old,&value,4); }
        *oldlen=4; return 0;
    }
    fprintf(stderr,"STOP: unsupported sysctl mib");
    for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",name[i]);
    fputc('\n',stderr);
    exit(21);
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *once,void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            guest_cpu_call((uintptr_t)routine,0,NULL);
            __atomic_store_n(once,1,__ATOMIC_RELEASE);
            return 0;
        }
        sched_yield();
    }
}
static ABI int32_t posix_once(int32_t *once,void (ABI *routine)(void)) { return thread_once(once,routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used; KeyDestructor destructor; } keys[KEYS];
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *key_values[KEYS];
static ABI int32_t key_create(uint32_t *key,KeyDestructor destructor) {
    if (!key) return ERR(22);
    pthread_mutex_lock(&key_lock);
    for (uint32_t i=1;i<KEYS;++i) if (!keys[i].used) {
        keys[i].used=1; keys[i].destructor=destructor;
        pthread_mutex_unlock(&key_lock);
        *key=i; return 0;
    }
    pthread_mutex_unlock(&key_lock);
    return ERR(35);
}
static ABI int32_t key_delete(uint32_t key)
