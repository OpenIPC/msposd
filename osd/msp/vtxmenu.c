#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/reboot.h>
#include <linux/reboot.h>  // For LINUX_REBOOT_CMD_RESTART

#include "msp_displayport.h"
#include "vtxmenu.h"
#include "../util/ini_parser.h"

bool clearNextDraw = false;
extern bool vtxMenuActive;

extern MenuSection *current_section;
extern int selected_option;

void exitVTXMenu(){
    vtxMenuActive = false;
}

void clear_vtx_menu() {
    clearNextDraw = true;
}
void display_menu(displayport_vtable_t *display_driver,MenuSection *section, int selected_option) {

    if (clearNextDraw) {
        display_driver->clear_screen();
        clearNextDraw = false;
    }

    int menu_offset_cols = 4;
    int menu_offset_row = 2;

    // Create a 2D array to store the menu representation
    char menu_grid[section->option_count+1][OSD_HD_COLS-menu_offset_cols];
    
    // Initialize the 2D array to empty spaces
    for (int i = 0; i < section->option_count+1; ++i) {
        memset(menu_grid[i], ' ', OSD_HD_COLS-menu_offset_cols); // Fill each row with spaces
        //menu_grid[i][OSD_HD_COLS - 1] = '\0'; // Ensure null termination for each string row
    }

    int current_row=0;
    printf("\n=== %s ===\n", section->name);
    snprintf(menu_grid[current_row++], OSD_HD_COLS-menu_offset_cols, "=== %s ===", section->name);

    for (int i = 0; i < section->option_count; i++) {
        char row_selectd[3] = "  ";
        if (i == selected_option) {
            snprintf(row_selectd, 3, " > ");
        }
        else {
            snprintf(row_selectd, 3, "   ");
        }
        MenuOption *option = &section->options[i];
        switch (option->type) {
            case MENU_OPTION_LIST: {
                char value_list[MAX_OPTIONS][20];
                int value_count;
                split_values(option->values, value_list, &value_count);
                printf("%s%s: %s\n",row_selectd, option->lable, value_list[section->current_value_index[i]]);
                snprintf(menu_grid[current_row++], OSD_HD_COLS-menu_offset_cols, "%s%s: %s", row_selectd,option->lable, value_list[section->current_value_index[i]]);
                break;
            }
            case MENU_OPTION_RANGE: {
                printf("%s%s: %d\n",row_selectd, option->lable, section->current_value_index[i]);
                snprintf(menu_grid[current_row++], OSD_HD_COLS-menu_offset_cols, "%s%s: %d", row_selectd, option->lable, section->current_value_index[i]);
                break;

            }
            case MENU_OPTION_SUBMENU: {
                int len = strlen(option->lable) + strlen(row_selectd);
                printf("%s%s%*s>\n", row_selectd, option->lable, 30 - len, "");
                snprintf(menu_grid[current_row++], OSD_HD_COLS-menu_offset_cols, "%s%s%*s>", row_selectd, option->lable, MAX_VTX_MENU_COLS - len,"");
                break;
            }
            case MENU_OPTION_COMMAND: {
                int len = strlen(option->lable) + strlen(row_selectd);
                printf("%s%s%*s>\n",row_selectd, option->lable, 30 - len, "");
                snprintf(menu_grid[current_row++], OSD_HD_COLS-menu_offset_cols, "%s%s%*s>", row_selectd, option->lable, MAX_VTX_MENU_COLS - len,"");
                break;
            }
            default:
                printf("\n");
                break;
        }
    }

    // Draw the populated menu array on the OSD
    for (int row = 0; row < section->option_count+1; row++) {
        for (int col = 0; col < OSD_HD_COLS-menu_offset_cols; col++) {
            display_driver->draw_character(col+menu_offset_cols, row+menu_offset_row, menu_grid[row][col]);
        }
    }
    display_driver->draw_complete();
}


void doreboot() {
    printf("Rebooting system ...\n");

    // Sync filesystems to ensure no data loss
    sync(); 

    // Call the reboot system call with the "restart" command
    if (reboot(LINUX_REBOOT_CMD_RESTART) == -1) {
        perror("Reboot failed");
    }    
}


void runCustomCommand() {
    printf("Running custom command: %s\n",current_section->options[selected_option].read_command);
    char output[MAX_VALUE_LENGTH] = "";
    run_command(current_section->options[selected_option].read_command, output, sizeof(output));
}
