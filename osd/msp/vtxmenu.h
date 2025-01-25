#pragma once
#include "../util/ini_parser.h"
#include "msp_displayport.h"
#include <stdbool.h>

#define OSD_HD_COLS 53
#define OSD_HD_ROWS 20
#define MAX_VTX_MENU_COLS 30

void display_menu(displayport_vtable_t *display_driver, MenuSection *section, int selected_option);
void clear_vtx_menu();
void doreboot();
void exitVTXMenu();
void runCustomCommand();
void safeboot();
