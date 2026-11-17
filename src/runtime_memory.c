/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Windows: a pagefile-backed section plays the memfd's part, and the PS4 user range is
 * reserved as one placeholder that views of the section replace (VirtualAlloc2/MapViewOfFile3,
 * as shadPS4 does); see the "host mappings" section below.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include "guest_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. GOW3_DMEM_MB adjusts it
 * (lower values like 4096-4608 MiB help avoid Android lmkd kills on 12 GB phones). */
static uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("GOW3_DMEM_MB");
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < 3072 || mb > 16384) mb = 5056;
        size = mb * 1024 * 1024;
    }
    return size;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#ifdef _WIN32
#define USER_MAX UINT64_C(0xfc00000000)
#else
#define USER_MAX UINT64_C(0x7f00000000)
#endif
#define LIMIT 4096
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
static void write_lock(void) {
    if (!exclusive_depth++) { pthread_rwlock_wrlock(&lock); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL); }
}
static void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); pthread_rwlock_unlock(&lock); }
}
static void read_lock(void) { if (!exclusive_depth) pthread_rwlock_rdlock(&lock); }
static void read_unlock(void) { if (!exclusive_depth) pthread_rwlock_unlock(&lock); }
static Block blocks[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
#ifndef _WIN32
static int pool_fd=-1;
#endif
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (gow3gpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook pending[64]; static size_t pending_count;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
#ifdef _WIN32
/* ---- host mappings (Windows) ---- */
typedef struct { uintptr_t start, end; uint64_t phys; int mapped; } HostRegion;
typedef struct { uintptr_t start, end; DWORD protect; } ProtRun;
static HostRegion *regions; static size_t region_count, region_capacity;
static HANDLE section;
static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void win_fail(const char *what, uintptr_t address, uint64_t size) {
    fprintf(stderr,"STOP: %s failed at %p (+0x%" PRIx64 "), error %lu\n",what,(void *)address,size,GetLastError());
    exit(21);
}
static size_t region_index(uintptr_t a) {
    size_t lo=0, hi=region_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (regions[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static void region_insert(size_t at, HostRegion r) {
    if (region_count==region_capacity) {
        size_t capacity=region_capacity ? region_capacity*2 : 256;
        HostRegion *next=realloc(regions,capacity*sizeof(*regions));
        if (!next) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
        regions=next; region_capacity=capacity;
    }
    memmove(regions+at+1,regions+at,(region_count-at)*sizeof(*regions));
    regions[at]=r; ++region_count;
}
static void region_erase(size_t at, size_t n) {
    memmove(regions+at,regions+at+n,(region_count-at-n)*sizeof(*regions)); region_count-=n;
}
void runtime_memory_reserve(void) {
    if (region_count) return;
    if (!VirtualAlloc2(NULL,(void *)USER_MIN,USER_MAX-USER_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        win_fail("reserving the PS4 address range (link with --disable-high-entropy-va)",USER_MIN,USER_MAX-USER_MIN);
    region_insert(0,(HostRegion){USER_MIN,USER_MAX,0,0});
}
static void view_map(const HostRegion *r, DWORD protect) {
    DWORD initial=protect==PAGE_NOACCESS ? PAGE_READWRITE : protect, old;
    if (!MapViewOfFile3(section,GetCurrentProcess(),(void *)r->start,r->phys,r->end-r->start,MEM_REPLACE_PLACEHOLDER,initial,NULL,0))
        win_fail("MapViewOfFile3",r->start,r->end-r->start);
    if (initial!=protect && !VirtualProtect((void *)r->start,r->end-r->start,protect,&old))
        win_fail("VirtualProtect",r->start,r->end-r->start);
}
static size_t save_protections(uintptr_t start, uintptr_t end, ProtRun **runs, size_t count, size_t *capacity) {
    for (uintptr_t at=start; at<end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at,&m,sizeof(m))) win_fail("VirtualQuery",at,0);
        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;
        if (next>end) next=end;
        if (count==*capacity) { *capacity=*capacity ? *capacity*2 : 16; *runs=realloc(*runs,*capacity*sizeof(**runs)); }
        (*runs)[count++]=(ProtRun){at,next,m.Protect};
        at=next;
    }
    return count;
}
static size_t carve_host(uintptr_t a, uintptr_t b) {
    size_t first=region_index(a), last=first;
    if (first==region_count || regions[first].start>a) win_fail("guest range outside the reservation",a,b-a);
    while (last<region_count && regions[last].start<b) ++last;
    if (regions[last-1].end<b) win_fail("guest range outside the reservation",a,b-a);
    uintptr_t lo=regions[first].start, hi=regions[last-1].end;
    HostRegion left=regions[first], right=regions[last-1];
    int keep_left=left.mapped && lo<a, keep_right=right.mapped && b<hi;
    if (first==last-1 && !left.mapped && lo==a && hi==b) return first;
    ProtRun *runs=NULL; size_t nruns=0, capacity=0, left_runs=0;
    if (keep_left) left_runs=nruns=save_protections(lo,a,&runs,nruns,&capacity);
    if (keep_right) nruns=save_protections(b,hi,&runs,nruns,&capacity);
    for (size_t i=first;i<last;++i) if (regions[i].mapped) {
        if (!UnmapViewOfFile2(GetCurrentProcess(),(void *)regions[i].start,MEM_PRESERVE_PLACEHOLDER))
            win_fail("UnmapViewOfFile2",regions[i].start,regions[i].end-regions[i].start);
    }
    if (last-first>1 && !VirtualFree((void *)lo,hi-lo,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        win_fail("coalescing placeholders",lo,hi-lo);
    region_erase(first,last-first);
    region_insert(first,(HostRegion){lo,hi,0,0});
    size_t at=first;
    if (lo<a) {
        if (!VirtualFree((void *)lo,a-lo,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",lo,a-lo);
        regions[at].end=a;
        region_insert(at+1,(HostRegion){a,hi,0,0});
        if (keep_left) {
            regions[at].phys=left.phys; regions[at].mapped=1;
            view_map(&regions[at],PAGE_READWRITE);
        }
        ++at;
    }
    if (b<hi) {
        if (!VirtualFree((void *)a,b-a,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",a,b-a);
        regions[at].end=b;
        region_insert(at+1,(HostRegion){b,hi,0,0});
        if (keep_right) {
            regions[at+1].phys=right.phys+(b-right.start); regions[at+1].mapped=1;
            view_map(&regions[at+1],PAGE_READWRITE);
        }
    }
    for (size_t i=0;i<nruns;++i) {
        DWORD old;
        if (runs[i].protect!=PAGE_READWRITE && !VirtualProtect((void *)runs[i].start,runs[i].end-runs[i].start,runs[i].protect,&old))
            win_fail("restoring page protection",runs[i].start,runs[i].end-runs[i].start);
    }
    (void)left_runs;
    free(runs);
    return at;
}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    size_t i=carve_host(address,address+size);
    if (reserved) return 0;
    regions[i].phys=phys; regions[i].mapped=1;
    view_map(&regions[i],win_prot(prot));
    return 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { carve_host(address,address+size); return 0; }
static int host_protect(uintptr_t address, uint64_t size, int prot) {
    DWORD protect=win_prot(prot), old;
    uintptr_t end=address+size;
    for (size_t i=region_index(address); i<region_count && regions[i].start<end; ++i) {
        if (!regions[i].mapped) continue;
        uintptr_t a=regions[i].start>address ? regions[i].start : address, b=regions[i].end<end ? regions[i].end : end;
        if (!VirtualProtect((void *)a,b-a,protect,&old)) return -1;
    }
    return 0;
}
static void host_discard(uint64_t phys, uint64_t size) { memset(backing_base+phys,0,size); }
static int pool(void) {
    if (backing_base) return 0;
    runtime_memory_reserve();
    uint64_t bytes=POOL_SIZE+FLEX_SPAN;
    section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_COMMIT,bytes,NULL,NULL,0);
    if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }
    backing_base=MapViewOfFile3(section,GetCurrentProcess(),NULL,0,bytes,0,PAGE_READWRITE,NULL,0);
    return backing_base ? 0 : -1;
}
#else
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    pool_fd=memfd_create("gow3-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
}
void runtime_memory_reserve(void) {}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    void *mapped = !reserved
        ? mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE,-1,0);
    return mapped==MAP_FAILED ? -1 : 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { return munmap((void *)address,size); }
static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }
static void host_discard(uint64_t phys, uint64_t size) {
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
#endif
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); exit(21); }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
    host_discard(phys,size);
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
    }
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count; return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT) right.phys+=a-vmas[i].start;
    vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
    if (kind==KIND_RESERVED) {
        uint64_t limit=USER_MAX;
        if (address<limit && address+size>limit) {
            uint64_t fit=(limit-address) & ~(uint64_t)(PAGE-1);
            fprintf(stderr,"Runtime: reservation at %p clamped from %" PRIu64 " to %" PRIu64 " bytes (host VA limit)\n",
                    (void *)address,size,fit);
            size=fit;
        }
    }
    fprintf(stderr,"Runtime: place kind=%d address=0x%" PRIxPTR " size=0x%" PRIx64 " before host_map\n",kind,address,size);
    fflush(stderr);
    if (host_map(address,size,host_prot(prot),kind==KIND_RESERVED,phys)) {
        int host_errno=errno;
        fprintf(stderr,"Runtime: HOST MMAP FAILED addr=%p size=%" PRIu64 " phys=0x%" PRIx64 " kind=%d errno=%d (%s)\n",
                (void *)address,size,phys,kind,host_errno,strerror(host_errno));
        fflush(stderr);
        if (kind!=KIND_RESERVED && !(flags & MAP_FIXED_FLAG)) return NO_MEMORY;
        uintptr_t moved=0, from=USER_MIN;
        uint64_t step=alignment > UINT64_C(0x1000000) ? alignment : UINT64_C(0x1000000);
        for (int attempt=0; attempt<256; ++attempt) {
            moved=find_free(from,size,alignment);
            if (!moved) break;
            if (!host_map(moved,size,host_prot(prot),kind==KIND_RESERVED,phys)) break;
            from=moved+step; moved=0;
        }
        if (!moved) {
            fprintf(stderr,"Runtime: relocation of %s %p FAILED (errno=%d)\n",kind==KIND_RESERVED?"reservation":"fixed map",(void *)address,errno);
            fflush(stderr);
            return NO_MEMORY;
        }
        fprintf(stderr,"Runtime: %s %p relocated to %p (size=%" PRIu64 ")\n",kind==KIND_RESERVED?"reservation":"fixed map",(void *)address,(void *)moved,size);
        fflush(stderr);
        address=moved;
    }
    if (overlaps(address,address+size,1)) {
        for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size;
