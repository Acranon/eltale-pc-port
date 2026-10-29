#include "common.h"
#include "port/os.h"

#include <stdio.h>
#include <stdlib.h>

static OSThread worker;
static OSMesgQueue vi_queue, timer_queue, pi_queue, sp_queue, dp_queue, ai_queue;
static OSMesg vi_buffer[2], timer_buffer[2], pi_buffer[2], sp_buffer[2], dp_buffer[2], ai_buffer[2];
static OSTimer timer;

uint32_t Port_NormalizeDlAddress(uint32_t word) { return word; }
uint32_t Port_HostNormalizeFrameAddress(uint32_t word) { return word; }
void Port_HostPace(uint64_t ticks) { (void)ticks; }
__attribute__((noreturn)) void Port_HostFatal(const char *message) {
    fprintf(stderr, "OS events test: %s\n", message);
    exit(1);
}

static void require_message(OSMesgQueue *queue, OSMesg expected) {
    OSMesg actual = NULL;
    if (osRecvMesg(queue, &actual, OS_MESG_BLOCK) != 0 || actual != expected)
        Port_HostFatal("wrong event message");
}

static void run_events(void *arg) {
    (void)arg;
    osCreateMesgQueue(&vi_queue, vi_buffer, 2);
    osCreateMesgQueue(&timer_queue, timer_buffer, 2);
    osCreateMesgQueue(&pi_queue, pi_buffer, 2);
    osCreateMesgQueue(&sp_queue, sp_buffer, 2);
    osCreateMesgQueue(&dp_queue, dp_buffer, 2);
    osCreateMesgQueue(&ai_queue, ai_buffer, 2);
    osViSetEvent(&vi_queue, (OSMesg)0x11, 1);
    osSetTimer(&timer, 390625, 0, &timer_queue, (OSMesg)0x22);
    require_message(&timer_queue, (OSMesg)0x22);
    if (osGetCount() != 390625 || osGetTime() != 390625)
        Port_HostFatal("timer fired at the wrong virtual time");
    require_message(&vi_queue, (OSMesg)0x11);
    if (osGetCount() != 781250)
        Port_HostFatal("VI fired at the wrong virtual time");

    Port_OsPostPi(&pi_queue, (OSMesg)0x33);
    require_message(&pi_queue, (OSMesg)0x33);

    osSetEventMesg(OS_EVENT_SP, &sp_queue, (OSMesg)0x44);
    osSetEventMesg(OS_EVENT_DP, &dp_queue, (OSMesg)0x55);
    Gfx commands[2] = { {{ 0xE7000000, 0 }}, {{ 0xB8000000, 0 }} };
    OSTask task = { 0 };
    task.t.type = M_GFXTASK;
    task.t.data_ptr = (u64 *)commands;
    task.t.data_size = sizeof commands;
    osSpTaskLoad(&task);
    osSpTaskStartGo(&task);
    require_message(&sp_queue, (OSMesg)0x44);
    require_message(&dp_queue, (OSMesg)0x55);
    if (Port_OsGfxTaskCount() != 1 || Port_OsFrameCount() != 0)
        Port_HostFatal("task completion count is wrong");
    osSetEventMesg(OS_EVENT_AI, &ai_queue, (OSMesg)0x66);
    if (osAiSetFrequency(22050) < 21000 ||
        osAiSetNextBuffer(NULL, 256) || osAiSetNextBuffer(NULL, 256) ||
        osAiSetNextBuffer(NULL, 256) != -1)
        Port_HostFatal("AI transfer queue is wrong");
    require_message(&ai_queue, (OSMesg)0x66);
    require_message(&ai_queue, (OSMesg)0x66);
    if (osAiGetLength() != 0 || osAiGetStatus() != 0)
        Port_HostFatal("AI transfer did not drain");
    Port_OsStop();
}

int main(void) {
    Port_OsInit();
    osCreateThread(&worker, 7, run_events, NULL, NULL, 5);
    osStartThread(&worker);
    Port_OsRun();
    puts("OS_EVENTS_OK");
    return 0;
}
