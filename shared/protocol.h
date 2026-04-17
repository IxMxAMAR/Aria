// shared/protocol.h — Communication protocol between VMM and usermode
#pragma once

#if defined(_KERNEL_MODE)
#include <ntddk.h>
#elif defined(ARIA_TYPES_DEFINED)
// VMM freestanding context — types provided by vmm.h
#else
#include <stdint.h>
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t  UINT8;
typedef int32_t  INT32;
#endif

// Magic value for CPUID-based communication
#define ARIA_CPUID_MAGIC    0x41524941  // 'ARIA'

// Command IDs (passed in ECX subleaf)
#define CMD_PING            0x01
#define CMD_GET_CR3         0x02
#define CMD_READ_MEMORY     0x03
#define CMD_WRITE_MEMORY    0x04
#define CMD_PATTERN_SCAN    0x05
#define CMD_READ_PLAYERS    0x06
#define CMD_SUBMIT_DRAWS    0x07
#define CMD_UPDATE_CONFIG   0x08
#define CMD_INIT_RENDER     0x09
#define CMD_MOVE_MOUSE      0x0A
#define CMD_PRESENT_HOOK    0x10
#define CMD_READ_CONFIG     0x20
#define CMD_WRITE_CONFIG    0x21
#define CMD_GET_STATUS      0x22
#define CMD_UPDATE_OFFSETS  0x23
#define CMD_FETCH_OFFSETS   0x24
#define CMD_DISCORD_HEARTBEAT 0x25
#define CMD_SHUTDOWN        0xFF

// Status codes (returned in EAX after CPUID)
#define ARIA_OK             0x00000000
#define ARIA_ERR_GENERIC    0x00000001
#define ARIA_ERR_NO_PROCESS 0x00000002
#define ARIA_ERR_READ_FAIL  0x00000003
#define ARIA_ERR_NOT_FOUND  0x00000004
#define ARIA_ERR_BAD_CMD    0x000000FF

#define ARIA_VERSION        0x00010000  // 1.0.0

#pragma pack(push, 1)

typedef struct _ARIA_REQUEST {
    UINT32 command;
    UINT32 status;
    UINT64 param1;
    UINT64 param2;
    UINT64 param3;
    UINT64 param4;
    UINT64 result1;
    UINT64 result2;
    UINT32 payload_size;
    UINT32 reserved;
} ARIA_REQUEST;

typedef struct _ARIA_READ_REQ {
    UINT64 address;
    UINT32 size;
    UINT32 reserved;
} ARIA_READ_REQ;

typedef struct _ARIA_SCAN_REQ {
    UINT64 module_base;
    UINT64 module_size;
    UINT32 pattern_len;
    UINT32 reserved;
    UINT8  pattern[256];
    UINT8  mask[256];
} ARIA_SCAN_REQ;

typedef struct _ARIA_PLAYER {
    float  pos_x, pos_y, pos_z;
    float  head_x, head_y, head_z;
    float  health;
    float  max_health;
    float  shield;
    INT32  team;
    UINT8  is_local;
    UINT8  is_alive;
    UINT8  pad[2];
} ARIA_PLAYER;

#define MAX_PLAYERS 32

typedef struct _ARIA_PLAYER_LIST {
    UINT32      count;
    UINT32      local_team;
    float       cam_x, cam_y, cam_z;
    float       cam_pitch, cam_yaw, cam_roll;
    float       cam_fov;
    UINT32      reserved;
    ARIA_PLAYER players[MAX_PLAYERS];
} ARIA_PLAYER_LIST;

// Runtime offset table (for auto-updater, spec §15)
#define OFF_ID_GAME_INSTANCE     0
#define OFF_ID_LOCAL_PLAYERS     1
#define OFF_ID_PLAYER_CONTROLLER 2
#define OFF_ID_ACKNOWLEDGED_PAWN 3
#define OFF_ID_CAMERA_MANAGER    4
#define OFF_ID_CAMERA_CACHE      5
#define OFF_ID_PERSISTENT_LEVEL  6
#define OFF_ID_ACTOR_ARRAY       7
#define OFF_ID_ACTOR_COUNT       8
#define OFF_ID_ROOT_COMPONENT    9
#define OFF_ID_RELATIVE_LOCATION 10
#define OFF_ID_PLAYER_STATE      11
#define OFF_ID_MESH_3P           12
#define OFF_ID_TEAM_ID           13
#define OFF_ID_HEALTH            14
#define OFF_ID_MAX_HEALTH        15
#define OFF_ID_SHIELD            16
#define OFF_ID_BONE_ARRAY        17
#define OFF_ID_BONE_COUNT        18
#define OFF_ID_PLAYER_NAME       19
#define OFF_ID_COUNT             20

typedef struct _ARIA_OFFSET_TABLE {
    UINT32 version;
    UINT32 count;
    struct {
        UINT32 id;
        UINT32 value;
    } entries[64];
} ARIA_OFFSET_TABLE;

#pragma pack(pop)

#define ARIA_SHARED_PAGE_GPA  0x0000000DEAD00000ULL
#define ARIA_DRAW_BUF_GPA     0x0000000DEAD10000ULL
#define ARIA_DRAW_BUF_SIZE    0x10000  // 64KB
