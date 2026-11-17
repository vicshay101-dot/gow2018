/* Loader for the game's x86-64 image.
 *
 * The CPU side is architecture-independent: on x86-64 the guest code runs on the host CPU
 * (src/runtime_cpu.c), elsewhere it runs under FEXCore (cpu/, libgow3cpu.so). Everything that
 * crosses between host and guest goes through the boundary in src/guest_cpu.h, so this file has
 * no x86-64 machine-code assumptions of its own. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include "runtime.h"
#include "guest_cpu.h"
#include "orb_hook.h"
#include "cheat_hook.h"
#include "actor_hook.h"
#include "damage_hook.h"
#include "gpu/gow3gpu.h"
#if !defined(__GNUC__)
#error This loader requires GCC or Clang.
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#ifdef __ANDROID__
#include <unwind.h>
#include <sys/prctl.h>
#else
#include <execinfo.h>
#endif
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#if defined(__ANDROID__) && !defined(_WIN32)
typedef struct { void **frames; int count, max; } UnwindState;
static _Unwind_Reason_Code unwind_step(struct _Unwind_Context *context, void *argument) {
    UnwindState *state = argument;
    uintptr_t pc = _Unwind_GetIP(context);
    if (!pc) return _URC_END_OF_STACK;
    if (state->count >= state->max) return _URC_END_OF_STACK;
    state->frames[state->count++] = (void *)pc;
    return _URC_NO_REASON;
}
static int backtrace(void **frames, int max) {
    UnwindState state = {frames, 0, max};
    _Unwind_Backtrace(unwind_step, &state);
    return state.count;
}
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           guest_cpu_return_address((uintptr_t)__builtin_return_address(0)) - (uintptr_t)image, argument);
    uintptr_t caller=guest_cpu_return_address((uintptr_t)__builtin_return_address(0))-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#if defined(GUEST_CPU_NATIVE) && !defined(_WIN32)
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack.
 * x86-64 hosts only: elsewhere (libgow3cpu.so) guest calls go through guest_cpu_call, which
 * switches to the guest stack itself. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    if (gpu_enabled) gow3gpu_dump_guest_writes(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && gow3gpu_handle_fault(info,(void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in gow3_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)gow3_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* GOW3_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
#ifndef _WIN32
int runtime_watch_handle_fault(int sig,siginfo_t *info,void *context);
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking gets first chance to resolve its own protected-page faults. */
    if (gpu_enabled && sig == SIGSEGV && gow3gpu_handle_fault(context, info->si_addr)) return;
    /* Diagnostic watch on the A page (only active when GOW3_WATCH_A=1). */
    if (runtime_watch_handle_fault(sig,info,context)) return;
    /* Faults the guest CPU itself raises (JIT bookkeeping) are handled and resumed. */
    if (guest_cpu_handle_fault(sig, info, context)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    GuestRegs regs;
    const int in_guest = guest_cpu_signal_regs(context, &regs);
    uintptr_t rip = in_guest == GUEST_CPU_IN_GUEST ? (uintptr_t)regs.rip : guest_cpu_host_pc(context);
    char line[1024];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    /* Extra crash context: thread, guest/host state, last guest->host call, and the mapping that holds the PC. */
    {
        char tname[32]="?";
#ifdef __ANDROID__
        prctl(PR_GET_NAME,(unsigned long)tname,0,0,0);
#endif
        snprintf(line,sizeof(line),"  thread='%s' tid=%ld in_guest=%d image=%p last_guest_call_ret=0x%lx (image+0x%lx) guest_sp=0x%lx\n",
                 tname,(long)gettid(),in_guest,(void *)image,(unsigned long)guest_cpu_return_address(0),
                 (unsigned long)(guest_cpu_return_address(0)-(uintptr_t)image),(unsigned long)guest_cpu_stack_pointer());
        { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
        if (in_guest) {
            snprintf(line,sizeof(line),"  guest rip=0x%lx (image+0x%lx) rsp=0x%lx rbp=0x%lx rax=0x%lx rcx=0x%lx rdx=0x%lx rbx=0x%lx rsi=0x%lx rdi=0x%lx\n",
                     (unsigned long)regs.rip,(unsigned long)(regs.rip-(uintptr_t)image),(unsigned long)regs.gpr[GUEST_RSP],(unsigned long)regs.gpr[GUEST_RBP],
                     (unsigned long)regs.gpr[GUEST_RAX],(unsigned long)regs.gpr[GUEST_RCX],(unsigned long)regs.gpr[GUEST_RDX],(unsigned long)regs.gpr[GUEST_RBX],
                     (unsigned long)regs.gpr[GUEST_RSI],(unsigned long)regs.gpr[GUEST_RDI]);
            { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
            snprintf(line,sizeof(line),"  guest r8=0x%lx r9=0x%lx r10=0x%lx r11=0x%lx r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
                     (unsigned long)regs.gpr[GUEST_R8],(unsigned long)regs.gpr[GUEST_R9],(unsigned long)regs.gpr[GUEST_R10],(unsigned long)regs.gpr[GUEST_R11],
                     (unsigned long)regs.gpr[GUEST_R12],(unsigned long)regs.gpr[GUEST_R13],(unsigned long)regs.gpr[GUEST_R14],(unsigned long)regs.gpr[GUEST_R15]);
            { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
        }
#if defined(__aarch64__)
        {
            const ucontext_t *uc=(const ucontext_t *)context;
            snprintf(line,sizeof(line),"  host pc=0x%lx sp=0x%lx fault_addr=0x%lx\n",(unsigned long)uc->uc_mcontext.pc,(unsigned long)uc->uc_mcontext.sp,(unsigned long)uc->uc_mcontext.fault_address);
            { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
            for (int i=0;i<31;i+=4) {
                snprintf(line,sizeof(line),"  x%d=0x%lx x%d=0x%lx x%d=0x%lx x%d=0x%lx\n",i,(unsigned long)uc->uc_mcontext.regs[i],
                         i+1,(unsigned long)uc->uc_mcontext.regs[i+1],i+2,(unsigned long)uc->uc_mcontext.regs[i+2],
                         i+3<31?i+3:30,(unsigned long)uc->uc_mcontext.regs[i+3<31?i+3:30]);
                { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
            }
        }
#endif
        {
            /* Safe reads (process_vm_readv) so a bad pointer cannot fault again. */
            #define SAFE_READ(dst,addr,len) ({ struct iovec l_={(dst),(len)}, r_={(void *)(uintptr_t)(addr),(len)}; process_vm_readv(getpid(),&l_,1,&r_,1,0)==(ssize_t)(len); })
            static char maps[1<<20]; size_t used=0; int mfd=open("/proc/self/maps",O_RDONLY);
            if (mfd>=0) { ssize_t n; while (used<sizeof(maps)-1 && (n=read(mfd,maps+used,sizeof(maps)-1-used))>0) used+=(size_t)n; close(mfd); }
            maps[used]=0;
            uintptr_t probes[3]={rip,0,0};
#if defined(__aarch64__)
            probes[1]=((const ucontext_t *)context)->uc_mcontext.regs[30];
            probes[2]=((const ucontext_t *)context)->uc_mcontext.regs[28];
#endif
            for (int k=0;k<3;++k) {
                if (!probes[k]) continue;
                char *ln=maps;
                while (ln && *ln) {
                    char *nl=strchr(ln,'\n'); if (nl) *nl=0;
                    unsigned long lo=0,hi=0;
                    if (sscanf(ln,"%lx-%lx",&lo,&hi)==2 && probes[k]>=lo && probes[k]<hi) {
                        snprintf(line,sizeof(line),"  %s mapping: %s (offset in mapping 0x%lx)\n",k==0?"pc":k==1?"lr":"x28",ln,(unsigned long)(probes[k]-lo));
                        { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
                        break;
                    }
                    if (nl) *nl='\n';
                    ln=nl?nl+1:NULL;
                }
            }
            unsigned char code[64];
            if (SAFE_READ(code,rip-32,sizeof(code))) {
                int o=snprintf(line,sizeof(line),"  code around pc (pc-32..pc+32):");
                for (size_t i=0;i<sizeof(code) && o<(int)sizeof(line)-4;++i) o+=snprintf(line+o,sizeof(line)-(size_t)o," %02x",code[i]);
                snprintf(line+o,sizeof(line)-(size_t)o,"\n"); { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
            }
            /* Guest stack: return addresses (image offsets) show who called what. */
            uintptr_t gsp=in_guest ? (uintptr_t)regs.gpr[GUEST_RSP] : 0;
            uint64_t stk[48];
            if (gsp && SAFE_READ(stk,gsp,sizeof(stk))) {
                for (int i=0;i<48;i+=4) {
                    snprintf(line,sizeof(line),"  gstack+0x%x: %lx %lx %lx %lx\n",i*8,(unsigned long)stk[i],(unsigned long)stk[i+1],(unsigned long)stk[i+2],(unsigned long)stk[i+3]);
                    { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
                }
                for (int i=0;i<48;++i)
                    if (stk[i]>=(uintptr_t)image && stk[i]<(uintptr_t)image+0x3000000) {
                        snprintf(line,sizeof(line),"  gstack ret candidate [%d]: image+0x%lx\n",i,(unsigned long)(stk[i]-(uintptr_t)image));
                        { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
                    }
            }
            /* Guest code bytes (x86-64) around the likely crash site and the stack return addresses, to disassemble offline. */
            {
                uintptr_t sites[10]; int ns_=0;
#if defined(__aarch64__)
                sites[ns_++]=((const ucontext_t *)context)->uc_mcontext.regs[20];
#endif
                if (gsp && SAFE_READ(stk,gsp,sizeof(stk)))
                    for (int i=0;i<48 && ns_<10;++i)
                        if (stk[i]>=(uintptr_t)image && stk[i]<(uintptr_t)image+0x3000000) sites[ns_++]=stk[i];
                for (int k=0;k<ns_;++k) {
                    unsigned char b[96];
                    if (!SAFE_READ(b,sites[k]-64,sizeof(b))) continue;
                    int o=snprintf(line,sizeof(line),"  guest code image+0x%lx (-64..+32):",(unsigned long)(sites[k]-(uintptr_t)image));
                    for (size_t i=0;i<sizeof(b) && o<(int)sizeof(line)-4;++i) o+=snprintf(line+o,sizeof(line)-(size_t)o,"%02x",b[i]);
                    snprintf(line+o,sizeof(line)-(size_t)o,"\n"); { ssize_t w_=write(2,line,strlen(line)); (void)w_; }
                }
            }
#if defined(__aarch64__)
            /* FEX keeps its CPU state (guest RIP first, then the 16 GPRs) at the pointer in x28/x0. */
            const uintptr_t st[2]={((const ucontext_t *)context)->uc_mcontext.regs[28],((const ucontext_t *)context)->uc_mcontext.regs[0]};
            for (int k=0;k<2;++k) {
                uint64_t q[20];
      
