#include "common.h"
#include "port/os.h"

#include <stdio.h>
#include <stdlib.h>

static OSThread low_thread, high_thread, peer_a_thread, peer_b_thread, sender_thread;
static OSMesgQueue queue;
static OSMesg messages[1];
static OSMesgQueue jam_queue;
static OSMesg jam_messages[2];
static int sequence[12];
static int step;

__attribute__((noreturn)) void Port_HostFatal(const char *message) {
    fprintf(stderr, "OS scheduler test: %s\n", message);
    exit(1);
}

static void high(void *arg) {
    (void)arg;
    sequence[step++] = 2;
    OSMesg message = NULL;
    if (osRecvMesg(&queue, &message, OS_MESG_BLOCK) != 0 || message != (OSMesg)0x1234)
        Port_HostFatal("high-priority receiver got the wrong message");
    sequence[step++] = 4;
    osStopThread(NULL);
    Port_HostFatal("stopped high-priority thread resumed");
}

static void peer_a(void *arg) {
    (void)arg;
    sequence[step++] = 6;
    osStopThread(NULL);
    Port_HostFatal("stopped peer A resumed");
}

static void peer_b(void *arg) {
    (void)arg;
    sequence[step++] = 7;
    osStopThread(NULL);
    Port_HostFatal("stopped peer B resumed");
}

static void blocked_sender(void *arg) {
    (void)arg;
    sequence[step++] = 9;
    if (osSendMesg(&queue, (OSMesg)0x22, OS_MESG_BLOCK) != 0)
        Port_HostFatal("blocked sender failed");
    sequence[step++] = 11;
    osStopThread(NULL);
    Port_HostFatal("stopped sender resumed");
}

static void low(void *arg) {
    (void)arg;
    sequence[step++] = 1;
    osCreateMesgQueue(&queue, messages, 1);
    osCreateThread(&high_thread, 2, high, NULL, NULL, 10);
    osStartThread(&high_thread); /* preempts, then high blocks on the queue */
    sequence[step++] = 3;
    if (osSendMesg(&queue, (OSMesg)0x1234, OS_MESG_BLOCK) != 0)
        Port_HostFatal("send failed");
    sequence[step++] = 5;
    osCreateThread(&peer_a_thread, 3, peer_a, NULL, NULL, 5);
    osCreateThread(&peer_b_thread, 4, peer_b, NULL, NULL, 5);
    osStartThread(&peer_a_thread);
    osStartThread(&peer_b_thread);
    osYieldThread(); /* equal priorities run in start order */
    sequence[step++] = 8;
    if (osGetThreadPri(NULL) != 5) Port_HostFatal("wrong current priority");
    osSetThreadPri(NULL, 6);
    if (osGetThreadPri(NULL) != 6) Port_HostFatal("priority change failed");
    osSetThreadPri(NULL, 5);

    if (osSendMesg(&queue, (OSMesg)0x11, OS_MESG_NOBLOCK) != 0 ||
        osSendMesg(&queue, (OSMesg)0x33, OS_MESG_NOBLOCK) != -1)
        Port_HostFatal("full queue nonblocking result is wrong");
    osCreateThread(&sender_thread, 6, blocked_sender, NULL, NULL, 10);
    osStartThread(&sender_thread); /* sender blocks on the full queue */
    sequence[step++] = 10;
    OSMesg message = NULL;
    if (osRecvMesg(&queue, &message, OS_MESG_BLOCK) != 0 || message != (OSMesg)0x11)
        Port_HostFatal("first full-queue message is wrong");
    sequence[step++] = 12;
    if (osRecvMesg(&queue, &message, OS_MESG_BLOCK) != 0 || message != (OSMesg)0x22)
        Port_HostFatal("blocked sender message is wrong");

    osCreateMesgQueue(&jam_queue, jam_messages, 2);
    osSendMesg(&jam_queue, (OSMesg)1, OS_MESG_NOBLOCK);
    osJamMesg(&jam_queue, (OSMesg)2, OS_MESG_NOBLOCK);
    if (osRecvMesg(&jam_queue, &message, OS_MESG_NOBLOCK) != 0 || message != (OSMesg)2 ||
        osRecvMesg(&jam_queue, &message, OS_MESG_NOBLOCK) != 0 || message != (OSMesg)1 ||
        osRecvMesg(&jam_queue, &message, OS_MESG_NOBLOCK) != -1)
        Port_HostFatal("jam/FIFO queue order is wrong");
    Port_OsStop();
}

int main(void) {
    Port_OsInit();
    osCreateThread(&low_thread, 5, low, NULL, NULL, 5);
    osStartThread(&low_thread);
    Port_OsRun();
    const int want[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    if (step != 12) Port_HostFatal("wrong number of scheduling steps");
    for (int i = 0; i < 12; i++) {
        if (sequence[i] != want[i]) Port_HostFatal("wrong priority/FIFO order");
    }
    if (Port_OsThreadCount() != 5 || Port_OsStartedCount() != 5 || Port_OsRunCount() != 5)
        Port_HostFatal("thread counts are wrong");
    puts("OS_SCHEDULER_OK");
    return 0;
}
