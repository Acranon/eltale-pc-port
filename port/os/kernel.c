#include "common.h"
#include "port/fiber.h"
#include "port/os.h"
#include "port/p0/host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_OS_THREADS = 64, FIBER_STACK_BYTES = 0x40000,
       MAX_PENDING_EVENTS = 128, MAX_HOST_TIMERS = 16, COUNTS_PER_VI = 781250 };
/* initialize.c starts at 62.5 MHz, then applies the N64 3/4 count ratio. */
u64 osClockRate = UINT64_C(46875000);
s32 osViClock = VI_NTSC_CLOCK;
typedef enum ThreadState {
    THREAD_CREATED,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_RECV,
    THREAD_SEND,
    THREAD_STOPPED,
} ThreadState;

typedef struct HostThread {
    OSThread *n64;
    PortFiber *fiber;
    void (*entry)(void *);
    void *arg;
    OSMesgQueue *wait_queue;
    uint64_t ready_order;
    uint64_t wait_order;
    OSPri priority;
    OSId id;
    ThreadState state;
    unsigned started;
    unsigned ran;
} HostThread;

static HostThread threads[MAX_OS_THREADS];
static unsigned thread_count;
static unsigned started_count;
static unsigned ran_count;
static uint64_t order_counter;
static HostThread *current;
static PortFiber *host_fiber;
static int stopping;
static OSIntMask interrupt_mask;

typedef struct HostEvent {
    OSMesgQueue *queue;
    OSMesg message;
} HostEvent;
static HostEvent events[OS_NUM_EVENTS];
static OSMesgQueue *vi_queue;
static OSMesg vi_message;
static u32 vi_fields;
static uint64_t clock_count;
static uint64_t next_vi_tick;
static uint64_t vi_count;
static void *current_framebuffer;
static unsigned frame_limit;
static int fast_mode;
static unsigned frame_count;
static unsigned gfx_task_count;
static uint64_t last_gfx_hash;
static OSTask *loaded_task;

typedef struct PendingEvent {
    OSMesgQueue *queue;
    OSMesg message;
} PendingEvent;
static PendingEvent pending[MAX_PENDING_EVENTS];
static unsigned pending_first, pending_count;

typedef struct HostTimer {
    OSTimer *n64;
    OSMesgQueue *queue;
    OSMesg message;
    uint64_t deadline;
    uint64_t interval;
    int active;
} HostTimer;
static HostTimer timers[MAX_HOST_TIMERS];

static void post_event(OSMesgQueue *queue, OSMesg message) {
    if (!queue) return;
    if (pending_count == MAX_PENDING_EVENTS) Port_HostFatal("OS event queue full");
    pending[(pending_first + pending_count) % MAX_PENDING_EVENTS] =
        (PendingEvent){ queue, message };
    pending_count++;
}

void Port_OsPostPi(OSMesgQueue *queue, OSMesg message) {
    post_event(queue, message);
}

static void deliver_pending(void) {
    while (pending_count) {
        PendingEvent item = pending[pending_first];
        pending_first = (pending_first + 1) % MAX_PENDING_EVENTS;
        pending_count--;
        osSendMesg(item.queue, item.message, OS_MESG_NOBLOCK);
    }
}

static void advance_to_event(void) {
    uint64_t next = next_vi_tick;
    for (unsigned i = 0; i < MAX_HOST_TIMERS; i++) {
        if (timers[i].active && timers[i].deadline < next)
            next = timers[i].deadline;
    }
    if (next == UINT64_MAX) Port_HostFatal("OS idle with no virtual event");
    clock_count = next;
    if (!fast_mode) Port_HostPace(clock_count);
    if (clock_count == next_vi_tick) {
        vi_count++;
        next_vi_tick += COUNTS_PER_VI;
        if (vi_queue && vi_fields && vi_count % vi_fields == 0)
            post_event(vi_queue, vi_message);
    }
    for (unsigned i = 0; i < MAX_HOST_TIMERS; i++) {
        HostTimer *timer = &timers[i];
        if (!timer->active || timer->deadline > clock_count) continue;
        post_event(timer->queue, timer->message);
        if (timer->interval) timer->deadline += timer->interval;
        else timer->active = 0;
    }
}

static HostThread *find_thread(OSThread *n64) {
    for (unsigned i = 0; i < thread_count; i++) {
        if (threads[i].n64 == n64) return &threads[i];
    }
    return NULL;
}

static HostThread *best_ready(void) {
    HostThread *best = NULL;
    for (unsigned i = 0; i < thread_count; i++) {
        HostThread *t = &threads[i];
        if (t->state != THREAD_READY) continue;
        /* Boot's idle thread starts the main thread once. Its following
         * for(;;) must never spin while an event can advance virtual time. */
        if (t->id == 1 && t->ran) continue;
        if (!best || t->priority > best->priority ||
            (t->priority == best->priority && t->ready_order < best->ready_order))
            best = t;
    }
    return best;
}

static void switch_to_host(void) {
    if (!current) Port_HostFatal("OS switch without a running thread");
    port_fiber_switch(current->fiber, host_fiber);
}

static void maybe_preempt(void) {
    HostThread *best = best_ready();
    if (current && best && best->priority > current->priority) {
        current->state = THREAD_READY;
        current->n64->state = OS_STATE_RUNNABLE;
        current->ready_order = ++order_counter;
        switch_to_host();
    }
}

static void thread_entry(void *arg) {
    HostThread *t = arg;
    t->entry(t->arg);
    t->state = THREAD_STOPPED;
    t->n64->state = OS_STATE_STOPPED;
    switch_to_host();
    Port_HostFatal("stopped OS thread resumed");
}

void Port_OsInit(void) {
    memset(threads, 0, sizeof threads);
    memset(events, 0, sizeof events);
    thread_count = started_count = ran_count = 0;
    order_counter = 0;
    current = NULL;
    stopping = 0;
    interrupt_mask = OS_IM_NONE;
    vi_queue = NULL;
    vi_fields = 0;
    vi_message = NULL;
    clock_count = vi_count = 0;
    next_vi_tick = COUNTS_PER_VI;
    current_framebuffer = NULL;
    frame_limit = frame_count = gfx_task_count = 0;
    fast_mode = 1;
    last_gfx_hash = 0;
    loaded_task = NULL;
    pending_first = pending_count = 0;
    memset(timers, 0, sizeof timers);
    host_fiber = port_fiber_main();
    if (!port_fiber_fixed()) Port_HostFatal("OS fiber stacks are not at their fixed address");
}

void Port_OsRun(void) {
    if (!host_fiber) Port_HostFatal("OS scheduler was not initialized");
    if (!fast_mode) Port_HostPace(0);
    while (!stopping) {
        deliver_pending();
        HostThread *next = best_ready();
        if (!next) {
            if (!vi_queue) {
                int has_timer = 0;
                for (unsigned i = 0; i < MAX_HOST_TIMERS; i++)
                    has_timer |= timers[i].active;
                if (!has_timer) Port_HostFatal("OS idle with no virtual event scheduled");
            }
            advance_to_event();
            continue;
        }
        current = next;
        next->state = THREAD_RUNNING;
        next->n64->state = OS_STATE_RUNNING;
        if (!next->ran) {
            next->ran = 1;
            ran_count++;
        }
        port_fiber_switch(host_fiber, next->fiber);
        current = NULL;
    }
}

void Port_OsStop(void) {
    stopping = 1;
    if (!current) Port_HostFatal("OS stop outside a game thread");
    switch_to_host();
    Port_HostFatal("OS checkpoint thread resumed");
}

unsigned Port_OsThreadCount(void) { return thread_count; }
unsigned Port_OsStartedCount(void) { return started_count; }
unsigned Port_OsRunCount(void) { return ran_count; }
unsigned Port_OsFrameCount(void) { return frame_count; }
unsigned Port_OsGfxTaskCount(void) { return gfx_task_count; }
uint64_t Port_OsLastGfxHash(void) { return last_gfx_hash; }
uint64_t Port_OsViCount(void) { return vi_count; }
void Port_OsSetFrameLimit(unsigned frames) { frame_limit = frames; }
void Port_OsSetFast(int fast) { fast_mode = fast != 0; }

void osCreateThread(OSThread *n64, OSId id, void (*entry)(void *), void *arg,
                    void *stack, OSPri priority) {
    (void)stack;
    if (thread_count == MAX_OS_THREADS) Port_HostFatal("too many OS threads");
    if (find_thread(n64)) Port_HostFatal("OS thread object created twice");
    HostThread *t = &threads[thread_count++];
    *t = (HostThread){ .n64 = n64, .entry = entry, .arg = arg,
                       .priority = priority, .id = id, .state = THREAD_CREATED };
    t->fiber = port_fiber_create(FIBER_STACK_BYTES, thread_entry, t);
    n64->id = id;
    n64->priority = priority;
    n64->state = OS_STATE_STOPPED;
}

void osStartThread(OSThread *n64) {
    HostThread *t = find_thread(n64);
    if (!t) Port_HostFatal("starting an unknown OS thread");
    if (t->state != THREAD_CREATED && t->state != THREAD_STOPPED) return;
    if (t->state == THREAD_STOPPED && t->started)
        Port_HostFatal("restarting a stopped OS thread is not implemented");
    t->state = THREAD_READY;
    t->ready_order = ++order_counter;
    n64->state = OS_STATE_RUNNABLE;
    t->started = 1;
    started_count++;
    maybe_preempt();
}

void osStopThread(OSThread *n64) {
    HostThread *t = n64 ? find_thread(n64) : current;
    if (!t) Port_HostFatal("stopping an unknown OS thread");
    t->state = THREAD_STOPPED;
    t->n64->state = OS_STATE_STOPPED;
    if (t == current) switch_to_host();
}

void osYieldThread(void) {
    if (!current) Port_HostFatal("yield outside an OS thread");
    current->state = THREAD_READY;
    current->n64->state = OS_STATE_RUNNABLE;
    current->ready_order = ++order_counter;
    switch_to_host();
}

void osSetThreadPri(OSThread *n64, OSPri priority) {
    HostThread *t = n64 ? find_thread(n64) : current;
    if (!t) Port_HostFatal("changing priority of an unknown OS thread");
    t->priority = priority;
    t->n64->priority = priority;
    maybe_preempt();
}

OSPri osGetThreadPri(OSThread *n64) {
    HostThread *t = n64 ? find_thread(n64) : current;
    if (!t) Port_HostFatal("reading priority of an unknown OS thread");
    return t->priority;
}

OSId osGetThreadId(OSThread *n64) {
    HostThread *t = n64 ? find_thread(n64) : current;
    if (!t) Port_HostFatal("reading id of an unknown OS thread");
    return t->id;
}

static void wake_one(OSMesgQueue *queue, ThreadState state) {
    HostThread *best = NULL;
    for (unsigned i = 0; i < thread_count; i++) {
        HostThread *t = &threads[i];
        if (t->state != state || t->wait_queue != queue) continue;
        if (!best || t->priority > best->priority ||
            (t->priority == best->priority && t->wait_order < best->wait_order))
            best = t;
    }
    if (best) {
        best->state = THREAD_READY;
        best->n64->state = OS_STATE_RUNNABLE;
        best->wait_queue = NULL;
        best->ready_order = ++order_counter;
    }
}

void osCreateMesgQueue(OSMesgQueue *queue, OSMesg *buffer, s32 capacity) {
    if (capacity <= 0) Port_HostFatal("invalid OS message queue capacity");
    queue->mtqueue = NULL;
    queue->fullqueue = NULL;
    queue->validCount = 0;
    queue->first = 0;
    queue->msgCount = capacity;
    queue->msg = buffer;
}

static s32 send_message(OSMesgQueue *queue, OSMesg message, s32 flag, int jam) {
    while (queue->validCount == queue->msgCount) {
        if (flag == OS_MESG_NOBLOCK) return -1;
        if (!current) Port_HostFatal("blocking OS send outside a thread");
        current->state = THREAD_SEND;
        current->n64->state = OS_STATE_WAITING;
        current->wait_queue = queue;
        current->wait_order = ++order_counter;
        switch_to_host();
    }
    if (jam) {
        queue->first = (queue->first + queue->msgCount - 1) % queue->msgCount;
        queue->msg[queue->first] = message;
    } else {
        queue->msg[(queue->first + queue->validCount) % queue->msgCount] = message;
    }
    queue->validCount++;
    wake_one(queue, THREAD_RECV);
    maybe_preempt();
    return 0;
}

s32 osSendMesg(OSMesgQueue *queue, OSMesg message, s32 flag) {
    return send_message(queue, message, flag, 0);
}

s32 osJamMesg(OSMesgQueue *queue, OSMesg message, s32 flag) {
    return send_message(queue, message, flag, 1);
}

s32 osRecvMesg(OSMesgQueue *queue, OSMesg *out, s32 flag) {
    while (queue->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) return -1;
        if (!current) Port_HostFatal("blocking OS receive outside a thread");
        current->state = THREAD_RECV;
        current->n64->state = OS_STATE_WAITING;
        current->wait_queue = queue;
        current->wait_order = ++order_counter;
        switch_to_host();
    }
    if (out) *out = queue->msg[queue->first];
    queue->first = (queue->first + 1) % queue->msgCount;
    queue->validCount--;
    wake_one(queue, THREAD_SEND);
    maybe_preempt();
    return 0;
}

void osSetEventMesg(OSEvent event, OSMesgQueue *queue, OSMesg message) {
    if ((unsigned)event >= OS_NUM_EVENTS) Port_HostFatal("invalid OS event");
    events[event] = (HostEvent){ queue, message };
}

OSIntMask osSetIntMask(OSIntMask mask) {
    OSIntMask old = interrupt_mask;
    interrupt_mask = mask;
    return old;
}

void osViSetEvent(OSMesgQueue *queue, OSMesg message, u32 fields) {
    vi_queue = queue;
    vi_message = message;
    vi_fields = fields;
}

void *osViGetCurrentFramebuffer(void) { return current_framebuffer; }

void osViSwapBuffer(void *framebuffer) {
    current_framebuffer = framebuffer;
    /* Graphics completion may happen before the next retrace. The host has
     * no physical VI, so consume one virtual retrace per presented frame. */
    uint64_t target_vi = vi_count + 1;
    while (vi_count < target_vi) advance_to_event();
    frame_count++;
    if (frame_limit && frame_count >= frame_limit) Port_OsStop();
    osYieldThread();
}

u32 osGetCount(void) { return (u32)clock_count; }
OSTime osGetTime(void) { return (OSTime)clock_count; }

int osSetTimer(OSTimer *n64, OSTime countdown, OSTime interval,
               OSMesgQueue *queue, OSMesg message) {
    HostTimer *timer = NULL;
    for (unsigned i = 0; i < MAX_HOST_TIMERS; i++) {
        if (timers[i].n64 == n64 || !timers[i].active) {
            timer = &timers[i];
            break;
        }
    }
    if (!timer) Port_HostFatal("too many OS timers");
    *timer = (HostTimer){ n64, queue, message, clock_count + (uint64_t)countdown,
                          (uint64_t)interval, 1 };
    return 0;
}

int osStopTimer(OSTimer *n64) {
    for (unsigned i = 0; i < MAX_HOST_TIMERS; i++) {
        if (timers[i].n64 == n64 && timers[i].active) {
            timers[i].active = 0;
            return 0;
        }
    }
    return -1;
}

void osSpTaskLoad(OSTask *task) { loaded_task = task; }

static u32 normalized_second_word(const Gfx *command) {
    u8 opcode = (u8)(command->words.w0 >> 24);
    switch (opcode) {
        case (u8)G_MOVEWORD:
            if ((command->words.w0 & 0xff) != G_MW_SEGMENT) return command->words.w1;
            /* fall through */
        case (u8)G_DL:
        case (u8)G_VTX:
        case (u8)G_MTX:
        case (u8)G_MOVEMEM:
        case (u8)G_SETTIMG:
        case (u8)G_SETCIMG:
        case (u8)G_SETZIMG:
            return Port_NormalizeDlAddress(Port_HostNormalizeFrameAddress(command->words.w1));
        default:
            return command->words.w1;
    }
}

static uint64_t hash_display_list(const Gfx *dl, unsigned words) {
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    for (unsigned i = 0; i < words; i++) {
        u32 pair[2] = { dl[i].words.w0, normalized_second_word(&dl[i]) };
        for (unsigned j = 0; j < 2; j++) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                hash ^= (pair[j] >> shift) & 0xFFu;
                hash *= UINT64_C(0x100000001B3);
            }
        }
    }
    return hash;
}

void osSpTaskStartGo(OSTask *task) {
    if (task != loaded_task) Port_HostFatal("starting an unloaded RSP task");
    loaded_task = NULL;
    if (task->t.type == M_GFXTASK) {
        unsigned words = task->t.data_size / sizeof(Gfx);
        if (!task->t.data_ptr || !words || words > 10000)
            Port_HostFatal("invalid graphics task display list");
        last_gfx_hash = hash_display_list((const Gfx *)task->t.data_ptr, words);
        gfx_task_count++;
        if (getenv("ELTALE_DUMP_DL") && gfx_task_count == 3) {
            const Gfx *dl = (const Gfx *)task->t.data_ptr;
            for (unsigned i = 0; i < words; i++)
                fprintf(stderr, "DL3[%u]=%08x:%08x norm=%08x\n", i,
                        dl[i].words.w0, dl[i].words.w1,
                        normalized_second_word(&dl[i]));
        }
        printf("P1_GFX_TASK frame=%u words=%u hash=%016llx\n", gfx_task_count,
               words, (unsigned long long)last_gfx_hash);
        post_event(events[OS_EVENT_SP].queue, events[OS_EVENT_SP].message);
        post_event(events[OS_EVENT_DP].queue, events[OS_EVENT_DP].message);
    } else if (task->t.type == M_AUDTASK) {
        post_event(events[OS_EVENT_SP].queue, events[OS_EVENT_SP].message);
    } else {
        Port_HostFatal("unknown RSP task type");
    }
}
