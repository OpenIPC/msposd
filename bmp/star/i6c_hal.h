#pragma once

#include "i6c_common.h"
#include "i6c_rgn.h"
#include "i6c_sys.h"

#include <sys/select.h>
#include <unistd.h>

extern char keepRunning;

void i6c_hal_deinit(void);
int i6c_hal_init(void);

int i6c_region_create(char handle, hal_rect rect, short opacity);
void i6c_region_deinit(void);
void i6c_region_destroy(char handle);
void i6c_region_init(i6c_rgn_pal *palette);
int i6c_region_setbitmap(int handle, hal_bitmap *bitmap);

void i6c_system_deinit(void);
int i6c_system_init(void);