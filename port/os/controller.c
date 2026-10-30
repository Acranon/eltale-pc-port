#include "common.h"
#include "port/controller.h"
#include "port/os.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_INPUT_ROWS = 4096 };
typedef struct InputRow {
    uint64_t vi;
    u16 buttons;
    s8 stick_x;
    s8 stick_y;
} InputRow;
static InputRow rows[MAX_INPUT_ROWS];
static unsigned row_count, next_row;
/* `repeat FROM PERIOD`: after the last row, the rows from VI FROM on play again every PERIOD VIs */
static unsigned repeat_first;
static uint64_t repeat_period, repeat_offset;
static OSContPad current_pad;
static u8 channels = 4;

int Port_ControllerLoadScript(const char *path) {
    row_count = next_row = 0;
    repeat_period = repeat_offset = 0;
    memset(&current_pad, 0, sizeof current_pad);
    if (!path) return 0;
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return -1;
    }
    char line[256];
    unsigned lineno = 0;
    while (fgets(line, sizeof line, file)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        unsigned long long from, period;
        if (sscanf(line, "repeat %llu %llu", &from, &period) == 2) {
            repeat_first = 0;
            while (repeat_first < row_count && rows[repeat_first].vi < from) repeat_first++;
            if (repeat_first == row_count || !period || from + period <= rows[row_count - 1].vi) {
                fprintf(stderr, "input script %s:%u: repeat must follow its rows and span them\n", path, lineno);
                fclose(file);
                return -1;
            }
            repeat_period = period;
            continue;
        }
        unsigned long long vi;
        int buttons;
        int x, y;
        if (sscanf(line, "%llu %i %d %d", &vi, &buttons, &x, &y) != 4 ||
            row_count == MAX_INPUT_ROWS || buttons < 0 || buttons > 0xFFFF ||
            x < -128 || x > 127 || y < -128 || y > 127 ||
            (row_count && vi <= rows[row_count - 1].vi)) {
            fprintf(stderr, "input script %s:%u: bad or unordered row\n", path, lineno);
            fclose(file);
            return -1;
        }
        rows[row_count++] = (InputRow){ (uint64_t)vi, (u16)buttons, (s8)x, (s8)y };
    }
    fclose(file);
    return 0;
}

/* Set by the host when a window gives live input (port/gfx/rt64_bridge.c). Used when no script
 * was loaded. */
void (*Port_LivePad)(u16 *buttons, s8 *stick_x, s8 *stick_y);

static void update_pad(void) {
    if (Port_LivePad && row_count == 0) {
        u16 before = current_pad.button;
        Port_LivePad(&current_pad.button, &current_pad.stick_x, &current_pad.stick_y);
        if (current_pad.button != before)
            fprintf(stderr, "P1_INPUT live vi=%llu buttons=%04x\n", (unsigned long long)Port_OsViCount(),
                    current_pad.button);
        return;
    }
    uint64_t now = Port_OsViCount();
    for (;;) {
        if (next_row == row_count && repeat_period) {
            next_row = repeat_first;
            repeat_offset += repeat_period;
        }
        if (next_row >= row_count || rows[next_row].vi + repeat_offset > now) break;
        fprintf(stderr, "P1_INPUT vi=%llu buttons=%04x stick=%d,%d\n",
                (unsigned long long)now, rows[next_row].buttons,
                rows[next_row].stick_x, rows[next_row].stick_y);
        current_pad.button = rows[next_row].buttons;
        current_pad.stick_x = rows[next_row].stick_x;
        current_pad.stick_y = rows[next_row].stick_y;
        next_row++;
    }
}

s32 osContInit(OSMesgQueue *queue, u8 *pattern, OSContStatus *status) {
    (void)queue;
    *pattern = 1;
    memset(status, 0, 4 * sizeof *status);
    status[0].status = 1; /* controller with a virtual Controller Pak */
    return 0;
}

s32 osContSetCh(u8 n) { channels = n; return 0; }

s32 osContStartReadData(OSMesgQueue *queue) {
    (void)channels;
    osSendMesg(queue, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetReadData(OSContPad *pads) {
    update_pad();
    memset(pads, 0, 4 * sizeof *pads);
    pads[0] = current_pad;
}

s32 osContStartQuery(OSMesgQueue *queue) {
    osSendMesg(queue, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetQuery(OSContStatus *status) {
    memset(status, 0, 4 * sizeof *status);
    status[0].status = 1;
}
