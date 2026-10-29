#include "common.h"
#include "port/fiber.h"
#include "port/os.h"
#include "port/p0/host.h"

#include <stdint.h>
#include <string.h>

enum { MAX_OS_THREADS = 64, FIBER_STACK_BYTES = 0x40000 };
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
    host_fiber = port_fiber_main();
    if (!port_fiber_fixed()) Port_HostFatal("OS fiber stacks are not at their fixed address");
}

void Port_OsRun(void) {
    if (!host_fiber) Port_HostFatal("OS scheduler was not initialized");
    while (!stopping) {
        HostThread *next = best_ready();
        if (!next) Port_HostFatal("OS idle with no virtual event scheduled");
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
