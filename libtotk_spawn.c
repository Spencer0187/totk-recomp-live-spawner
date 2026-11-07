#define _GNU_SOURCE
#include <errno.h>
#include <openssl/evp.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

// Experimental TotK PC Recomp v0.3.4 / guest TotK 1.4.3 ONLY.
// This first stage piggybacks the *next normal actor creation on a game thread*;
// it never calls game functions on the socket/background thread.
// Native address and original-byte guard established by REA + original NSO signature.
#define NATIVE_CREATE 0x2f05d60ull
#define NATIVE_PRESENT 0xf04d520ull
#define NATIVE_RT_CALL 0xf012f10ull
#define NATIVE_SVC 0xf0843f0ull
#define NATIVE_ACTOR_REGISTRY 0x2f0e760ull
#define GUEST_MAIN 0x7000000000ull
#define GUEST_ACTOR_CREATE (GUEST_MAIN + 0x599670ull)
#define PREFIX_LEN 14
#define SOCK_PATH_SIZE 108
#define MAX_NAME 95

// Recomp runtime/cpu.h: precisely aligned Cpu fields.
typedef struct __attribute__((aligned(16))) Cpu {
    uint64_t x[32], sp, pc, nzcv, tpidr, tpidrro, excl_addr, excl_val;
    void *thread;
    uint64_t fpcr, fpsr, excl_val_hi;
    uint8_t pad[512-344];
    __uint128_t v[32];
} Cpu;
_Static_assert(sizeof(Cpu)==1024, "Recomp CPU ABI mismatch");
_Static_assert(offsetof(Cpu,sp)==256, "CPU sp offset mismatch");
_Static_assert(offsetof(Cpu,thread)==312, "CPU thread offset mismatch");

typedef void (*GuestFn)(Cpu*);
static GuestFn original_create;
static GuestFn original_present;
static int use_present;
static atomic_uintptr_t actor_manager;
static atomic_ulong presents_seen, present_attempted;
static atomic_uintptr_t natural_thread_ptr, present_thread_ptr;
static atomic_ulong svc_natural_count, svc_natural_sleep, svc_natural_preempt, svc_attempted;
static int use_svc;
typedef void (*SvcFn)(Cpu*,uint32_t,uint64_t);
static SvcFn original_svc;
static GuestFn original_registry;
static int trace_registry;
static atomic_ulong traced_registry_calls;
static atomic_uintptr_t last_injected_job;
static atomic_uintptr_t last_natural_job;
static _Atomic uint32_t last_initial_status;

typedef uint64_t (*RtCall)(Cpu *c,uint64_t address,int args,...);
static RtCall engine_call;

static atomic_int request_state; // 0 free, 1 queued, 2 executing
static atomic_ulong natural_calls, attempted_calls;
static pthread_t server_thread;
static int server_fd=-1;
static char socket_path[SOCK_PATH_SIZE];
static __thread int in_hook;

typedef struct {char name[MAX_NAME+1]; float scale,offset[3];} Request;
static Request request;
// Game actor creation is asynchronous. The game retains pointers to the request
// after its enqueue function returns. Every request gets its own backing object;
// no new request can overwrite an earlier queued actor's memory. The tiny
// allocations are intentionally held until process exit (including in case a
// scene never consumes a particular job).
typedef struct SpawnBacking {
    _Alignas(16) uint8_t transform[512];
    char name[MAX_NAME+1];
    float position[3];
    uint64_t name_ptr;
    struct SpawnBacking *next;
} SpawnBacking;
static SpawnBacking *backing_head;
static atomic_ulong backing_count;
static int running;

static void note(const char *event) {fprintf(stderr,"[totk-live-spawn] %s\n",event);fflush(stderr);}

static int read_process_memory(uint64_t addr,void *output,size_t count){
    if (!addr || !output || count==0) return 0;
    struct iovec local={output,count},remote={(void*)(uintptr_t)addr,count};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)count;
}
static uint64_t read_u64(uint64_t addr){uint64_t v=0;return read_process_memory(addr,&v,8)?v:0;}

// Independently verified TotK 1.4.3 resident-player location chain, same as
// upstream Recomp runtime/frame_rate.c. Refuse spawns on title/loading screens.
static uint64_t player_actor(void) {
    uint64_t m=read_u64(GUEST_MAIN+0x3add238ull);
    uint64_t scene=m?read_u64(m+0x1e8ull):0;
    int32_t count=0;
    if (!scene || !read_process_memory(scene+0x50,&count,4) || count<14 || count>256)return 0;
    uint64_t comps=read_u64(scene+0x58);
    uint64_t resident=comps?read_u64(comps+13*8):0;
    if (!resident || !read_process_memory(resident+0x20,&count,4) || count<1 || count>64)return 0;
    uint64_t links=read_u64(resident+0x28);
    for (int i=0;links && i<count;i++) {
        uint64_t data=read_u64(links+(uint64_t)i*0x70+8);
        uint64_t actor=data?read_u64(data+0x40):0;
        uint64_t name=actor?read_u64(actor+0x218):0;
        char text[7];
        if (name && read_process_memory(name,text,7) && !memcmp(text,"Player",7))return actor;
    }
    return 0;
}
static bool player_position(float out[3]) {
    uint64_t actor=player_actor();
    return actor && read_process_memory(actor+0x2b4,out,12) &&
        isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}
static bool player_pose(float pos[3],float forward[2],float right[2]) {
    uint64_t actor=player_actor();
    float rot[9];
    if (!actor || !read_process_memory(actor+0x2b4,pos,12) ||
        !read_process_memory(actor+0x2c0,rot,sizeof(rot)))return false;
    // TotK player local +Z axis is the direction of forward movement.
    // Verified with in-world W/A movement: matrix [0,2]=+0x2c8,
    // matrix [2,2]=+0x2e0. The matrix is row-major 3x3.
    float fx=rot[2], fz=rot[8];
    float length=hypotf(fx,fz);
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2]) ||
        !isfinite(length) || length<0.7f || length>1.4f)return false;
    forward[0]=fx/length; forward[1]=fz/length;
    // Local left/right axis (perpendicular to forward on ground plane).
    right[0]=forward[1];right[1]=-forward[0];
    return true;
}

static void spawn_hook(Cpu *c) {
    if (in_hook) { original_create(c); return; }
    in_hook=1;
    atomic_fetch_add_explicit(&natural_calls,1,memory_order_relaxed);
    if(c->thread) {
        uintptr_t zero=0;
        if(atomic_compare_exchange_strong(&natural_thread_ptr,&zero,(uintptr_t)c->thread))
            fprintf(stderr,"[totk-live-spawn] NATURAL actor caller guest_thread=%p cpu=%p\n",c->thread,(void*)c);
    }
    Cpu before=*c;
    if (before.x[0] && read_u64(before.x[0]))
        atomic_store_explicit(&actor_manager,(uintptr_t)before.x[0],memory_order_release);
    if (use_present || use_svc) {
        static atomic_uint trace_budget;
        bool do_trace = getenv("TOTK_LIVE_SPAWN_TRACE_NATURAL") != NULL &&
                        atomic_fetch_add_explicit(&trace_budget,1,memory_order_relaxed)<24;
        char actorname[96]={0};
        float params[16]={0};
        uint64_t stk[3]={0};
        if(do_trace) {
            uint64_t nameptr=read_u64(before.x[1]);
            if(nameptr)read_process_memory(nameptr,actorname,sizeof(actorname)-1);
            read_process_memory(before.x[2],params,sizeof(params));
            read_process_memory(before.sp,stk,sizeof(stk));
            fprintf(stderr,"[totk-live-spawn] NATURAL-IN name=%s manager=%#llx x2=%#llx x3=%#llx x4=%#llx flags=%#llx stack=%#llx,%#llx,%#llx\n",
                    actorname,(unsigned long long)before.x[0],(unsigned long long)before.x[2],
                    (unsigned long long)before.x[3],(unsigned long long)before.x[4],
                    (unsigned long long)before.x[5],(unsigned long long)stk[0],
                    (unsigned long long)stk[1],(unsigned long long)stk[2]);
            fprintf(stderr,"[totk-live-spawn] NATURAL-PARAMS name=%s %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
                    actorname,params[0],params[1],params[2],params[3],
                    params[4],params[5],params[6],params[7],
                    params[8],params[9],params[10],params[11],
                    params[12],params[13],params[14],params[15]);
        }
        original_create(c);
        if(do_trace)fprintf(stderr,"[totk-live-spawn] NATURAL-OUT name=%s retval=%#llx\n",
                            actorname,(unsigned long long)c->x[0]);
        in_hook=0;
        return;
    }
    if (atomic_load_explicit(&request_state,memory_order_acquire)!=1) {
        original_create(c);
        in_hook=0;
        return;
    }
    float player[3]={0};
    // Default: only execute after a loaded world. The explicit isolated-test
    // override below allows us to investigate whether the menu scene supports
    // spawning too; never enable it in a normal launcher by default.
    bool title_test = getenv("TOTK_LIVE_SPAWN_ALLOW_TITLE_TEST") != NULL;
    bool has_player = player_position(player);
    if (!title_test && (!has_player || fabsf(player[0])+fabsf(player[2])<10.0f)) {
        original_create(c);in_hook=0;return;
    }
    enum { PARAM_BYTES=0x200 };
    uint8_t original_shape[PARAM_BYTES];
    uint64_t stack_args[3];
    if (!read_process_memory(before.x[2],original_shape,PARAM_BYTES) ||
        !read_process_memory(before.sp,stack_args,sizeof(stack_args)) ||
        (before.sp&15)) {
        original_create(c);in_hook=0;return;
    }
    int target=1;
    if (!atomic_compare_exchange_strong_explicit(&request_state,&target,2,
            memory_order_acq_rel,memory_order_acquire)) {
        original_create(c);in_hook=0;return;
    }
    // Inject BEFORE the normal actor is created, while all caller-supplied
    // actor initialization structures and backing pointers remain valid.
    Request wanted=request;
    char name[MAX_NAME+1];
    memcpy(name,wanted.name,sizeof(name));
    uint64_t string_ptr=(uint64_t)(uintptr_t)name;
    _Alignas(16) uint8_t transform[PARAM_BYTES];
    memcpy(transform,original_shape,sizeof(transform));
    // Relocate self-references into the cloned initialization structure.
    for(size_t i=0;i+8<=sizeof(transform);i+=8) {
        uint64_t value;
        memcpy(&value,transform+i,8);
        if(value>=before.x[2] && value-before.x[2]<sizeof(transform)) {
            value=(uint64_t)(uintptr_t)transform+(value-before.x[2]);
            memcpy(transform+i,&value,8);
        }
    }
    float destination[3];
    for(int i=0;i<3;i++)destination[i]=player[i]+wanted.offset[i];
    memcpy(transform,destination,12);
    for(int i=0;i<3;i++)memcpy(transform+48+i*4,&wanted.scale,4);
    uint64_t injected_stack[3]={0,(uint64_t)(uintptr_t)destination,0};
    memcpy((void*)(uintptr_t)before.sp,injected_stack,sizeof(injected_stack));
    *c=before;
    c->x[0]=before.x[0];
    c->x[1]=(uint64_t)(uintptr_t)&string_ptr;
    c->x[2]=(uint64_t)(uintptr_t)transform;
    c->x[3]=0;c->x[4]=1;c->x[5]=0;c->x[6]=0;c->x[7]=0;
    c->sp=before.sp;c->x[30]=before.x[30];
    atomic_fetch_add_explicit(&attempted_calls,1,memory_order_relaxed);
    fprintf(stderr,"[totk-live-spawn] attempting %s at (%.3f,%.3f,%.3f) scale %.3f on existing game actor creation thread\n",
            name,destination[0],destination[1],destination[2],wanted.scale);
    fflush(stderr);
    original_create(c);
    fprintf(stderr,"[totk-live-spawn] creation function returned x0=%#llx (not proof that actor is alive)\n",
            (unsigned long long)c->x[0]);
    fflush(stderr);
    // Restore original caller input frame and game CPU state before the game
    // processes its natural actor creation. Never discard the natural spawn.
    memcpy((void*)(uintptr_t)before.sp,stack_args,sizeof(stack_args));
    *c=before;
    atomic_store_explicit(&request_state,0,memory_order_release);
    original_create(c);
    in_hook=0;
}

// An opt-in per-frame dispatch path: the game calls nvnQueuePresentTexture
// from a guest thread with a live Cpu, and the existing Recomp runtime itself
// calls rt_call from this rendering path (resolution.c). Never use a socket
// listener's host Cpu to invoke guest functions.
// Common guest-call builder, invoked only on a real guest game thread.
// Inputs/outputs and temporary guest-stack slots are fully restored.
static void dispatch_from_cpu(Cpu *c, const char *origin) {
    if (atomic_load_explicit(&request_state,memory_order_acquire)!=1)return;
    uintptr_t manager=atomic_load_explicit(&actor_manager,memory_order_acquire);
    if (!manager || !read_u64(manager)) return;
    float player[3]={0};
    const bool title_test = getenv("TOTK_LIVE_SPAWN_ALLOW_TITLE_TEST")!=NULL;
    bool has_player=player_position(player);
    if(!title_test && (!has_player || fabsf(player[0])+fabsf(player[2])<10.0f))return;
    if(!c->sp || (c->sp&15))return;
    uint64_t callsp=c->sp-0x60;
    uint64_t saved_args[3]={0};
    if(!read_process_memory(callsp,saved_args,sizeof(saved_args)))return;
    int expect=1;
    if(!atomic_compare_exchange_strong_explicit(&request_state,&expect,2,
            memory_order_acq_rel,memory_order_acquire))return;
    Request wanted=request;
    SpawnBacking *backing=calloc(1,sizeof *backing);
    if(!backing){
        note("allocation failed for queued actor request");
        atomic_store_explicit(&request_state,0,memory_order_release);
        return;
    }
    backing->next=backing_head;
    backing_head=backing;
    atomic_fetch_add_explicit(&backing_count,1,memory_order_relaxed);
    char *name=backing->name;
    memcpy(name,wanted.name,sizeof(backing->name));
    backing->name_ptr=(uint64_t)(uintptr_t)name;
    uint8_t *transform=backing->transform;
    float *position=backing->position;
    for(int i=0;i<3;i++)position[i]=player[i]+wanted.offset[i];
    // UltraCam FUN_70000257bc: a 0x... stack actor transform with
    // identity rotation (float at offsets 12,28,44) and XYZ scale at 48-56.
    memcpy(transform,position,sizeof(backing->position));
    const float one=1.0f;
    for(int i=0;i<3;i++)memcpy(transform+12+i*16,&one,4);
    for(int i=0;i<3;i++)memcpy(transform+48+i*4,&wanted.scale,4);
    const char *empty="";
    memcpy(transform+64,&empty,sizeof(empty));
    // Exact empty-blackboard object graph recovered from UltraCam 1.4.3:
    //   local_1958 (matrix+0x58) -> local_1918 (matrix+0x98)
    //   local_1918[0] = 0x20; local_1910 (matrix+0xa0) -> local_1900
    //   local_1900 is at matrix+0xb0 and is a zero-initialized short buffer.
    // Zeroing just the root and passing a null child pointer was not faithful.
    const uint32_t blackboard_capacity=0x20;
    memcpy(transform+0x98,&blackboard_capacity,4);
    uint64_t bbroot=(uint64_t)(uintptr_t)(transform+0x98);
    uint64_t bbdata=(uint64_t)(uintptr_t)(transform+0xb0);
    memcpy(transform+0x58,&bbroot,8);
    memcpy(transform+0xa0,&bbdata,8);
    // Verified live with a metal rectangle physically falling on Link:
    // the UltraCam local_1920 flag mask 0x05 is necessary for the tested successful
    // visible actor creation. Keep enabled by default; explicit 0 is a
    // diagnostic regression switch for comparison with the broken path.
    const char *extra_flags=getenv("TOTK_LIVE_SPAWN_ULTRACAM_FLAGS");
    transform[0x90]=(extra_flags && !strcmp(extra_flags,"0")) ? 0 : 5;
    uint64_t call_args[3]={0,(uint64_t)(uintptr_t)position,0};
    memcpy((void*)(uintptr_t)callsp,call_args,sizeof(call_args));
    Cpu original=*c;
    c->sp=callsp;
    if(origin && !strcmp(origin,"svc"))atomic_fetch_add_explicit(&svc_attempted,1,memory_order_relaxed);
    else atomic_fetch_add_explicit(&present_attempted,1,memory_order_relaxed);
    fprintf(stderr,"[totk-live-spawn] %s request %s (%.2f,%.2f,%.2f), flags=%#x, backing=%p, manager=%#llx\n",
            origin,name,position[0],position[1],position[2],transform[0x90],
            (void*)backing,(unsigned long long)manager);
    fflush(stderr);
    uint64_t result=engine_call(c,GUEST_ACTOR_CREATE,8,(uint64_t)manager,
        (uint64_t)(uintptr_t)&backing->name_ptr,(uint64_t)(uintptr_t)transform,0ull,1ull,0ull,0ull,0ull);
    fprintf(stderr,"[totk-live-spawn] %s actor factory returned %#llx (object lifetime unverified)\n",
            origin,(unsigned long long)result);
    fflush(stderr);
    memcpy((void*)(uintptr_t)callsp,saved_args,sizeof(saved_args));
    *c=original;
    atomic_store_explicit(&request_state,0,memory_order_release);
}

static void present_hook(Cpu *c) {
    original_present(c);
    if(c->thread) {
        uintptr_t zero=0;
        if(atomic_compare_exchange_strong(&present_thread_ptr,&zero,(uintptr_t)c->thread))
            fprintf(stderr,"[totk-live-spawn] PRESENT actor dispatcher guest_thread=%p cpu=%p\n",c->thread,(void*)c);
    }
    atomic_fetch_add_explicit(&presents_seen,1,memory_order_relaxed);
    if (use_present && !use_svc)dispatch_from_cpu(c,"present");
}

static void svc_hook(Cpu *c,uint32_t imm,uint64_t pc) {
    original_svc(c,imm,pc);
    if(!c || !c->thread || c->thread!=(void*)atomic_load(&natural_thread_ptr))return;
    atomic_fetch_add_explicit(&svc_natural_count,1,memory_order_relaxed);
    if(imm==0x0B)atomic_fetch_add_explicit(&svc_natural_sleep,1,memory_order_relaxed);
    if(imm==0x36)atomic_fetch_add_explicit(&svc_natural_preempt,1,memory_order_relaxed);
    // After the actor-main thread's own SleepThread returns, we can invoke
    // the factory without touching render-thread state or waiting for an
    // unrelated natural actor creation.
    if(use_svc && imm==0x0B)dispatch_from_cpu(c,"svc");
}

// Read-only instrumentation for f_700059a380, which records actor state.
// The trampoline relocates its initial RIP-relative LEA into a MOVABS.
static void registry_hook(Cpu *c){
    const int state=atomic_load_explicit(&request_state,memory_order_relaxed);
    int trace=(state==2 || (trace_registry && atomic_fetch_add(&traced_registry_calls,1)<3));
    uint64_t regptr=c->x[0],actor=c->x[1],category=c->x[2];
    int32_t before[4]={-1,-1,-1,-1},after[4]={-1,-1,-1,-1};
    if(trace && regptr){
        for(int i=0;i<3;i++)read_process_memory(regptr+0x50+0x18*i+0x10,&before[i],4);
        read_process_memory(regptr+0xbc,&before[3],4);
    }
    original_registry(c);
    if(state!=2 && actor){
        uintptr_t zero=0;
        atomic_compare_exchange_strong(&last_natural_job,&zero,(uintptr_t)actor);
    }
    if(state==2 && actor){
        int32_t status=0;read_process_memory(actor+0x138,&status,4);
        atomic_store_explicit(&last_injected_job,(uintptr_t)actor,memory_order_release);
        atomic_store_explicit(&last_initial_status,(uint32_t)status,memory_order_release);
        fprintf(stderr,"[totk-live-spawn] INJECTED_JOB ptr=%#llx header=%#llx category=%llu status=%#x data=%#llx\n",
          (unsigned long long)actor,(unsigned long long)read_u64(actor),
          (unsigned long long)category,(uint32_t)status,
          (unsigned long long)read_u64(actor+0x10));
    }
    if(trace && regptr){
        for(int i=0;i<3;i++)read_process_memory(regptr+0x50+0x18*i+0x10,&after[i],4);
        read_process_memory(regptr+0xbc,&after[3],4);
        fprintf(stderr,"[totk-live-spawn] REGISTRY_CALL injected=%d category=%llu actor=%#llx result=%#llx before=%d,%d,%d,%d after=%d,%d,%d,%d\n",
                state==2,(unsigned long long)category,(unsigned long long)actor,
                (unsigned long long)c->x[0],before[0],before[1],before[2],before[3],
                after[0],after[1],after[2],after[3]);
        fflush(stderr);
    }
}

static void jump_abs(unsigned char data[12],void *address){
    data[0]=0x48;data[1]=0xb8;
    memcpy(data+2,&address,8);
    data[10]=0xff;data[11]=0xe0;
}
static int correct_game_binary(const char *path) {
    static const unsigned char expected[32]={
        0xff,0x7f,0x08,0xf3,0xd9,0x7b,0xae,0xf7,
        0x3c,0x9e,0xcb,0x22,0x00,0x42,0x72,0x21,
        0xf8,0x26,0x1e,0x5e,0x53,0x58,0x2c,0xe2,
        0x2e,0x26,0xf4,0xd4,0x24,0x81,0x68,0x18
    };
    FILE *f=fopen(path,"rb");
    if(!f) return 0;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    if(!ctx) {fclose(f);return 0;}
    int good=EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)==1;
    unsigned char buf[65536];size_t n;
    while(good && (n=fread(buf,1,sizeof(buf),f))>0)
        good=EVP_DigestUpdate(ctx,buf,n)==1;
    if(ferror(f))good=0;
    unsigned char digest[32];unsigned int length=0;
    if(good)good=EVP_DigestFinal_ex(ctx,digest,&length)==1;
    EVP_MD_CTX_free(ctx);fclose(f);
    return good && length==32 && memcmp(digest,expected,32)==0;
}

static int install_hook(void){
    void *target=(void*)(uintptr_t)NATIVE_CREATE;
    // Exact f_7000599670 prologue of the user's installed ELF. The 14
    // instructions contain no PC-relative references, and end at an instruction
    // boundary before an RIP-relative LEA.
    const unsigned char expected[PREFIX_LEN]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x48,0x89,0xfb};
    if (memcmp(target,expected,PREFIX_LEN)) {note("unsupported executable: actor function signature mismatch; hook disabled");return -1;}
    const long pagesize=sysconf(_SC_PAGESIZE);
    if (pagesize<=0)return -1;
    uint8_t *tramp=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (tramp==MAP_FAILED) {perror("mmap trampoline");return -1;}
    memcpy(tramp,expected,PREFIX_LEN);
    unsigned char jmp[12];jump_abs(jmp,(void*)((uintptr_t)target+PREFIX_LEN));
    memcpy(tramp+PREFIX_LEN,jmp,sizeof(jmp));
    if (mprotect(tramp,4096,PROT_READ|PROT_EXEC)) {perror("mprotect trampoline");munmap(tramp,4096);return -1;}
    uintptr_t base=(uintptr_t)target&~((uintptr_t)pagesize-1);
    if (mprotect((void*)base,pagesize,PROT_READ|PROT_WRITE|PROT_EXEC)) {
        perror("mprotect game text");munmap(tramp,4096);return -1;
    }
    original_create=(GuestFn)tramp;
    jump_abs(jmp,(void*)spawn_hook);
    memcpy(target,jmp,sizeof(jmp));
    memset((unsigned char*)target+12,0x90,PREFIX_LEN-12);
    __builtin___clear_cache(target,(char*)target+PREFIX_LEN);
    if (mprotect((void*)base,pagesize,PROT_READ|PROT_EXEC)) {perror("mprotect restore game text");}
    note("hook installed for native f_7000599670 (guest main+0x599670)");
    return 0;
}

static int install_present_hook(void){
    const unsigned char expected[16]={
        0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x83,0xec,0x10,0x49,0x89,0xfe
    };
    const uintptr_t address=NATIVE_PRESENT;
    void *site=(void*)address;
    if(memcmp(site,expected,sizeof(expected))) {
        note("present callback machine-code mismatch; per-frame dispatch disabled");return -1;
    }
    unsigned char *tramp=mmap(NULL,4096,PROT_READ|PROT_WRITE,
           MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(tramp==MAP_FAILED){note("present trampoline allocation failed");return -1;}
    memcpy(tramp,expected,sizeof(expected));
    unsigned char jump[12];
    jump_abs(jump,(void*)(address+sizeof(expected)));
    memcpy(tramp+sizeof(expected),jump,sizeof(jump));
    if(mprotect(tramp,4096,PROT_READ|PROT_EXEC)){
        munmap(tramp,4096);note("present trampoline executable marking failed");return -1;
    }
    uintptr_t pg=address&~((uintptr_t)sysconf(_SC_PAGESIZE)-1);
    if(mprotect((void*)pg,4096,PROT_READ|PROT_WRITE|PROT_EXEC)){
        munmap(tramp,4096);note("could not patch present callback");return -1;
    }
    original_present=(GuestFn)tramp;
    engine_call=(RtCall)(uintptr_t)NATIVE_RT_CALL;
    jump_abs(jump,(void*)present_hook);
    memcpy(site,jump,sizeof(jump));
    memset((unsigned char*)site+12,0x90,sizeof(expected)-12);
    __builtin___clear_cache(site,(unsigned char*)site+sizeof(expected));
    mprotect((void*)pg,4096,PROT_READ|PROT_EXEC);
    note("opt-in per-frame actor dispatch installed at nvnp_nvnQueuePresentTexture");
    return 0;
}

static int install_svc_hook(void){
    static const unsigned char expected[15]={
        0x55,0x41,0x57,0x41,0x56,0x53,0x50,0x49,
        0x89,0xd6,0x89,0xf5,0x49,0x89,0xff
    };
    void *target=(void*)(uintptr_t)NATIVE_SVC;
    if(memcmp(target,expected,sizeof(expected))){note("SVC prologue mismatch; hook disabled");return -1;}
    unsigned char *tramp=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(tramp==MAP_FAILED)return -1;
    memcpy(tramp,expected,sizeof(expected));
    unsigned char jmp[12];jump_abs(jmp,(void*)((uintptr_t)target+sizeof(expected)));
    memcpy(tramp+sizeof(expected),jmp,sizeof(jmp));
    if(mprotect(tramp,4096,PROT_READ|PROT_EXEC)){munmap(tramp,4096);return -1;}
    uintptr_t page=(uintptr_t)target&~((uintptr_t)sysconf(_SC_PAGESIZE)-1);
    if(mprotect((void*)page,4096,PROT_READ|PROT_WRITE|PROT_EXEC)){
        munmap(tramp,4096);return -1;
    }
    original_svc=(SvcFn)tramp;
    engine_call=(RtCall)(uintptr_t)NATIVE_RT_CALL;
    jump_abs(jmp,(void*)svc_hook);
    memcpy(target,jmp,sizeof(jmp));
    memset((unsigned char*)target+sizeof(jmp),0x90,sizeof(expected)-sizeof(jmp));
    __builtin___clear_cache(target,(unsigned char*)target+sizeof(expected));
    mprotect((void*)page,4096,PROT_READ|PROT_EXEC);
    note("same-game-thread SVC dispatch installed; SleepThread checkpoints active");
    return 0;
}

// Actor vector that f_700059a380 registers into, confirmed in lifted
// TotK 1.4.3 game code. Read-only: this does not mutate game state.
static int actor_registry_read(uint64_t *registry_out, int32_t counts_out[4]) {
    uint64_t global=read_u64(GUEST_MAIN+0x3ad6d80ull);
    uint64_t registry=global?read_u64(global+0x430ull):0;
    if(!registry)return 0;
    // The registry implements three subarrays, +0x50 plus 0x18 per category;
    // f_700059a380 inserts at category subarray+0x10, NOT always at +0xbc.
    for(int i=0;i<3;i++) {
        int32_t n=-1;
        if(!read_process_memory(registry+0x50+0x18*i+0x10,&n,4) || n<0 || n>1000000)return 0;
        counts_out[i]=n;
    }
    int32_t n=-1;
    if(!read_process_memory(registry+0xbc,&n,4) || n<0 || n>1000000)return 0;
    counts_out[3]=n;
    *registry_out=registry;
    return 1;
}

static int install_registry_hook(void){
    // f_700059a380: push r15;push r14;push rbx;mov rdi,rbx;
    // lea 0xcb17d09(%rip),rax (loads fixed global g_fnprof at 0xfa26478).
    static const uint8_t expected[15]={
        0x41,0x57,0x41,0x56,0x53,0x48,0x89,0xfb,
        0x48,0x8d,0x05,0x09,0x7d,0xb1,0x0c
    };
    void *site=(void*)(uintptr_t)NATIVE_ACTOR_REGISTRY;
    if(memcmp(site,expected,sizeof(expected))) {
        note("actor-registration hook signature mismatch; registry tracing disabled");return -1;
    }
    uint8_t *tramp=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(tramp==MAP_FAILED)return -1;
    memcpy(tramp,expected,8);
    tramp[8]=0x48;tramp[9]=0xb8;
    uint64_t lea_abs=0xfa26478ull;
    memcpy(tramp+10,&lea_abs,8);
    uint8_t jump[12];jump_abs(jump,(void*)(uintptr_t)(NATIVE_ACTOR_REGISTRY+15));
    memcpy(tramp+18,jump,sizeof(jump));
    if(mprotect(tramp,4096,PROT_READ|PROT_EXEC)){munmap(tramp,4096);return -1;}
    uintptr_t page=(uintptr_t)site&~((uintptr_t)sysconf(_SC_PAGESIZE)-1);
    if(mprotect((void*)page,4096,PROT_READ|PROT_WRITE|PROT_EXEC)){
        munmap(tramp,4096);return -1;
    }
    original_registry=(GuestFn)tramp;
    jump_abs(jump,(void*)registry_hook);
    memcpy(site,jump,sizeof(jump));
    memset((uint8_t*)site+12,0x90,3);
    __builtin___clear_cache(site,(uint8_t*)site+15);
    mprotect((void*)page,4096,PROT_READ|PROT_EXEC);
    note("actor registration hook installed for request lifecycle tracing");
    return 0;
}

static void reply(int fd,const char *s){(void)write(fd,s,strlen(s));}
static void *serve(void *arg){
    (void)arg;
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    snprintf(addr.sun_path,sizeof(addr.sun_path),"%s",socket_path);
    server_fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(server_fd<0){perror("socket");return NULL;}
    if(bind(server_fd,(struct sockaddr*)&addr,sizeof(addr))){perror("bind");close(server_fd);server_fd=-1;return NULL;}
    chmod(socket_path,0600);
    if(listen(server_fd,4)){perror("listen");return NULL;}
    fprintf(stderr,"[totk-live-spawn] Unix socket listening: %s\n",socket_path);fflush(stderr);
    while(running){
        int fd=accept(server_fd,NULL,NULL);
        if (fd<0){if(errno==EINTR)continue;break;}
        struct ucred cred;socklen_t len=sizeof(cred);
        if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&len) || cred.uid!=getuid()){
            reply(fd,"ERR permission\n");close(fd);continue;
        }
        char text[256]={0};ssize_t n=read(fd,text,sizeof(text)-1);
        if(n<=0){close(fd);continue;}
        if(!strncmp(text,"status",6)){
            float where[3]={0};
            bool hasplayer=player_position(where);
            char msg[512];
            uintptr_t lastjob=atomic_load_explicit(&last_injected_job,memory_order_acquire);
            uint32_t currentstatus=0xffffffffu;
            int jobread=lastjob&&read_process_memory(lastjob+0x138,&currentstatus,4);
            uintptr_t naturaljob=atomic_load_explicit(&last_natural_job,memory_order_acquire);
            uint32_t naturalstatus=0xffffffffu;
            int naturalread=naturaljob&&read_process_memory(naturaljob+0x138,&naturalstatus,4);
            uint64_t actor_registry=0;
            int32_t actor_counts[4]={-1,-1,-1,-1};
            actor_registry_read(&actor_registry,actor_counts);
            snprintf(msg,sizeof(msg),"READY native_create=%lu attempted=%lu pending=%d player=%s(%.2f,%.2f,%.2f) presents=%lu perframe_attempts=%lu svc_attempts=%lu manager=%#llx mode=%s native_guest_thread=%#llx present_guest_thread=%#llx svcs_on_actor_thread=%lu svc_sleep=%lu svc_preempt=%lu registry=%#llx registries=%d,%d,%d,%d lastjob=%#llx status=%#x originalstatus=%#x readable=%d naturaljob=%#llx naturalstatus=%#x naturalread=%d\n",
                atomic_load(&natural_calls),atomic_load(&attempted_calls),atomic_load(&request_state),
                hasplayer?"found":"missing",where[0],where[1],where[2],
                atomic_load(&presents_seen),atomic_load(&present_attempted),atomic_load(&svc_attempted),
                (unsigned long long)atomic_load(&actor_manager),use_svc?"svc":use_present?"perframe":"natural",
                (unsigned long long)atomic_load(&natural_thread_ptr),
                (unsigned long long)atomic_load(&present_thread_ptr),
                atomic_load(&svc_natural_count),atomic_load(&svc_natural_sleep),
                atomic_load(&svc_natural_preempt),
                (unsigned long long)actor_registry,actor_counts[0],actor_counts[1],actor_counts[2],actor_counts[3],
                (unsigned long long)lastjob,currentstatus,atomic_load(&last_initial_status),jobread,
                (unsigned long long)naturaljob,naturalstatus,naturalread);
            reply(fd,msg);
    } else if(!strncmp(text,"pose",4)){
            float pos[3],forward[2],right[2];
            if(!player_pose(pos,forward,right) ||
                fabsf(pos[0])+fabsf(pos[2])<10.0f){
                reply(fd,"ERR player pose unavailable; load a save first\n");
            } else {
                char msg[220];
                snprintf(msg,sizeof(msg),"POSE %.8g %.8g %.8g %.8g %.8g %.8g %.8g\n",
                  pos[0],pos[1],pos[2],forward[0],forward[1],right[0],right[1]);
                reply(fd,msg);
            }
    } else if(!strncmp(text,"spawn ",6)){
            Request r={0};char trailing=0;
            int items=sscanf(text+6,"%95s %f %f %f %f %c",r.name,&r.scale,&r.offset[0],&r.offset[1],&r.offset[2],&trailing);
            bool valid=items==5 && isfinite(r.scale) && r.scale>=.05f && r.scale<=5.0f;
            for(char*p=r.name;valid && *p;p++){
                if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'))valid=false;
            }
            for(int i=0;i<3;i++)if(!isfinite(r.offset[i])||fabsf(r.offset[i])>100.0f)valid=false;
            if(!valid)reply(fd,"ERR syntax: spawn ActorName Scale offsetX offsetY offsetZ\n");
            else if(atomic_load(&request_state)!=0)reply(fd,"ERR another actor request is pending\n");
            else {
                request=r;
                atomic_store_explicit(&request_state,1,memory_order_release);
                if(use_svc)reply(fd,"QUEUED; will execute at the next actor-thread checkpoint\n");
                else if(use_present)reply(fd,"QUEUED; will execute at the next render present (experimental)\n");
                else reply(fd,"QUEUED; waiting for a natural actor creation (fallback)\n");
            }
        } else if(!strncmp(text,"cancel",6)){
            int expected=1;
            if(atomic_compare_exchange_strong(&request_state,&expected,0))reply(fd,"CANCELLED\n");
            else reply(fd,"ERR none queued or already executing\n");
        } else reply(fd,"ERR unknown command\n");
        close(fd);
    }
    return NULL;
}

__attribute__((constructor))static void init(void){
    // Never patch unrelated programs that inherit LD_PRELOAD from launch.sh.
    char exe[256];ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    if(n<=0)return;
    exe[n]=0;
    const char*base=strrchr(exe,'/');base=base?base+1:exe;
    if(strcmp(base,"totk")||!getenv("TOTK_LIVE_SPAWN")||strcmp(getenv("TOTK_LIVE_SPAWN"),"1"))return;
    if(!correct_game_binary(exe)) {note("game binary SHA-256 mismatch; refusing hook");return;}
    if(install_hook())return;
    if(getenv("TOTK_LIVE_SPAWN_REG_TRACE")){
        trace_registry=1;
        install_registry_hook();
    }
    if(getenv("TOTK_LIVE_SPAWN_SVC") && strcmp(getenv("TOTK_LIVE_SPAWN_SVC"),"0") == 0) {
        // Disabled by explicit 0. All other nonempty values enable diagnostic.
    } else if(getenv("TOTK_LIVE_SPAWN_SVC")) {
        if(install_svc_hook()==0)use_svc=1;
    }
    if(getenv("TOTK_LIVE_SPAWN_PRESENT") && strcmp(getenv("TOTK_LIVE_SPAWN_PRESENT"),"0")) {
        if(install_present_hook()==0)use_present=1;
    }
    const char *runtime=getenv("XDG_RUNTIME_DIR");
    if(!runtime||!runtime[0])runtime="/tmp";
    snprintf(socket_path,sizeof(socket_path),"%s/totk-live-spawn-%u.sock",runtime,(unsigned)getuid());
    // Avoid interfering with another running instance that already owns socket.
    if(access(socket_path,F_OK)==0){
        int test=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(test>=0){struct sockaddr_un a={.sun_family=AF_UNIX};snprintf(a.sun_path,sizeof(a.sun_path),"%s",socket_path);
            if(connect(test,(struct sockaddr*)&a,sizeof(a))==0){note("another server is active; socket disabled");close(test);return;}
            close(test);
        }
        unlink(socket_path);
    }
    running=1;
    if(pthread_create(&server_thread,NULL,serve,NULL))note("failed to start socket listener");
}
__attribute__((destructor))static void fini(void){
    if(!running)return;
    running=0;
    if(server_fd>=0)close(server_fd);
    if(socket_path[0])unlink(socket_path);
}
