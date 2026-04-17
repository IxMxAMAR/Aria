// shared/draw_types.h — Draw command definitions for render injection
#pragma once

#include "protocol.h"

#define DRAW_CMD_NONE       0
#define DRAW_CMD_LINE       1
#define DRAW_CMD_BOX        2
#define DRAW_CMD_FILLED_BOX 3
#define DRAW_CMD_CIRCLE     4
#define DRAW_CMD_TEXT       5

#define COLOR_RED       0xFF3333FF
#define COLOR_GREEN     0x33FF33FF
#define COLOR_YELLOW    0xFFFF33FF
#define COLOR_WHITE     0xFFFFFFFF
#define COLOR_CYAN      0x00FFFFFF

#pragma pack(push, 1)

typedef struct _DRAW_CMD {
    UINT8  type;
    UINT8  thickness;
    UINT8  pad[2];
    UINT32 color;
    float  x1, y1;
    float  x2, y2;
    float  extra;
} DRAW_CMD;

#define MAX_DRAW_CMDS  512

typedef struct _DRAW_CMD_BUFFER {
    UINT32   count;
    UINT32   screen_w;
    UINT32   screen_h;
    UINT32   reserved;
    DRAW_CMD cmds[MAX_DRAW_CMDS];
} DRAW_CMD_BUFFER;

#pragma pack(pop)

#define MAKE_LINE(x1_, y1_, x2_, y2_, col_, thick_) \
    (DRAW_CMD){ DRAW_CMD_LINE, (thick_), {0}, (col_), (x1_), (y1_), (x2_), (y2_), 0 }

#define MAKE_BOX(x1_, y1_, x2_, y2_, col_, thick_) \
    (DRAW_CMD){ DRAW_CMD_BOX, (thick_), {0}, (col_), (x1_), (y1_), (x2_), (y2_), 0 }

#define MAKE_FILLED_BOX(x1_, y1_, x2_, y2_, col_) \
    (DRAW_CMD){ DRAW_CMD_FILLED_BOX, 0, {0}, (col_), (x1_), (y1_), (x2_), (y2_), 0 }

#define MAKE_CIRCLE(cx_, cy_, r_, col_, thick_) \
    (DRAW_CMD){ DRAW_CMD_CIRCLE, (thick_), {0}, (col_), (cx_), (cy_), 0, 0, (r_) }
