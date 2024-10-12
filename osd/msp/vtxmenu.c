#include <stdio.h>
#include "msp_displayport.h"
#include "vtxmenu.h"
#include "../util/ini_parser.h"

bool clearNextDraw = false;

void clear_vtx_menu() {
    clearNextDraw = true;
}
void display_menu(displayport_vtable_t *display_driver,MenuSection *section, int selected_option) {

    if (clearNextDraw) {
        display_driver->clear_screen();
        clearNextDraw = false;
    }

    // Create a 2D array to store the menu representation
    char menu_grid[section->option_count][OSD_HD_COLS];
    
    // Initialize the 2D array to empty spaces
    for (int i = 0; i < section->option_count; ++i) {
        memset(menu_grid[i], ' ', OSD_HD_COLS); // Fill each row with spaces
        menu_grid[i][OSD_HD_COLS - 1] = '\0'; // Ensure null termination for each string row
    }

    int current_row=0;
    printf("\n=== %s ===\n", section->name);
    snprintf(menu_grid[current_row++], OSD_HD_COLS, "=== %s ===", section->name);

    for (int i = 0; i < section->option_count; i++) {
        char row_selectd[3] = "  ";
        if (i == selected_option) {
            printf(" > ");
            snprintf(row_selectd, 3, " > ");
        }
        else {
            printf("   ");
            snprintf(row_selectd, 3, "   ");
        }
        MenuOption *option = &section->options[i];
        switch (option->type) {
            case MENU_OPTION_LIST: {
                char value_list[MAX_OPTIONS][20];
                int value_count;
                split_values(option->values, value_list, &value_count);
                printf("%s: %s\n",option->lable, value_list[section->current_value_index[i]]);
                snprintf(menu_grid[current_row++], OSD_HD_COLS, "%s%s: %s", row_selectd,option->lable, value_list[section->current_value_index[i]]);
                break;
            }
            case MENU_OPTION_RANGE: {
                printf("%s: %d\n", option->lable, section->current_value_index[i]);
                snprintf(menu_grid[current_row++], OSD_HD_COLS, "%s%s: %d", row_selectd, option->lable, section->current_value_index[i]);
                break;

            }
            case MENU_OPTION_SUBMENU:
                printf("[%s]\n", option->lable);
                snprintf(menu_grid[current_row++], OSD_HD_COLS, "%s[%s]", row_selectd, option->lable);
                break;
            case MENU_OPTION_COMMAND:
                printf("[%s]\n", option->lable);
                //snprintf(menu_grid[current_row++], OSD_HD_COLS, "%s[%s]", row_selectd, option->lable);
                //snprintf(menu_grid[current_row++], OSD_HD_COLS, "%s[SAVE]", row_selectd);
                break;
            default:
                printf("\n");
                break;
        }
    }

    // Draw the populated menu array on the OSD
    for (int row = 0; row < section->option_count; row++) {
        for (int col = 0; col < OSD_HD_COLS; col++) {
            display_driver->draw_character(col+4, row+4, menu_grid[row][col]);
        }
    }
    display_driver->draw_complete();
}