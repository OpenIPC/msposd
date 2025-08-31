#pragma once

#include "macros.h"

#ifndef ALIGN_BACK
#define ALIGN_BACK(x, a) (((x) / (a)) * (a))
#endif
#ifndef ALIGN_UP
#define ALIGN_UP(x, a) ((((x) + ((a)-1)) / a) * a)
#endif
#ifndef CEILING_2_POWER
#define CEILING_2_POWER(x, a) (((x) + ((a)-1)) & (~((a) - 1)))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

typedef enum {
    HAL_PLATFORM_UNK,
    HAL_PLATFORM_I6,
    HAL_PLATFORM_I6C,
    HAL_PLATFORM_M6,
    HAL_PLATFORM_V1,
    HAL_PLATFORM_V2,
    HAL_PLATFORM_V3,
    HAL_PLATFORM_V4
} hal_platform;

typedef enum {
    OP_READ = 0b1,
    OP_WRITE = 0b10,
    OP_MODIFY = 0b11
} hal_register_op;

typedef struct {
    unsigned short width, height;
} hal_dim;

typedef struct {
    hal_dim dim;
    void *data;
} hal_bitmap;

typedef struct {
    unsigned short x, y, width, height;
} hal_rect;