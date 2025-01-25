#include "msp_displayport.h"
#include "../../msposd.h"
#include "../util/ini_parser.h"
#include "msp.h"
#include "vtxmenu.h"
#include <linux/reboot.h> // For LINUX_REBOOT_CMD_RESTART
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

MenuSystem menu_system;
MenuSection *current_section;
int current_section_index = 0;
int selected_option = 0;

extern bool verbose;
extern bool vtxMenuEnabled;

static void process_draw_string(displayport_vtable_t *display_driver, uint8_t *payload) {
	if (!display_driver || !display_driver->draw_character)
		return;
	uint8_t row = payload[0];
	uint8_t col = payload[1];
	uint8_t attrs = payload[2]; // INAV and Betaflight use this to specify a
								// higher page number.
	uint8_t str_len;
	for (str_len = 1; str_len < 255; str_len++) {
		if (payload[2 + str_len] == '\0') {
			break;
		}
	}
	for (uint8_t idx = 0; idx < (str_len - 1); idx++) {
		uint16_t character = payload[3 + idx];
		if (attrs & 0x3) {
			// shift over by the page number if they were specified
			character |= ((attrs & 0x3) * 0x100);
		}
		display_driver->draw_character(col, row, character);
		col++;
	}
}

static void process_clear_screen(displayport_vtable_t *display_driver) {
	if (!display_driver || !display_driver->clear_screen)
		return;
	display_driver->clear_screen();
}

static void process_draw_complete(displayport_vtable_t *display_driver) {

	if (!display_driver || !display_driver->draw_complete)
		return;
	display_driver->draw_complete();
}

static void process_set_options(displayport_vtable_t *display_driver, uint8_t *payload) {
	if (!display_driver || !display_driver->set_options)
		return;
	uint8_t font = payload[0];
	msp_hd_options_e is_hd = payload[1];
	display_driver->set_options(font, is_hd);
}

static void process_open(displayport_vtable_t *display_driver) {}

static void process_close(displayport_vtable_t *display_driver) {
	process_clear_screen(display_driver);
}

int displayport_process_message(displayport_vtable_t *display_driver, msp_msg_t *msg) {
	if (msg->direction != MSP_INBOUND) {
		return 1;
	}
	if (msg->cmd != MSP_CMD_DISPLAYPORT) {
		return 1;
	}
	msp_displayport_cmd_e sub_cmd = msg->payload[0];
	switch (sub_cmd) {
	case MSP_DISPLAYPORT_KEEPALIVE: // 0 -> Open/Keep-Alive DisplayPort
		process_open(display_driver);
		break;
	case MSP_DISPLAYPORT_CLOSE: // 1 -> Close DisplayPort
		process_close(display_driver);
		break;
	case MSP_DISPLAYPORT_CLEAR: // 2 -> Clear Screen
		process_clear_screen(display_driver);
		break;
	case MSP_DISPLAYPORT_DRAW_STRING: // 3 -> Draw String
		process_draw_string(display_driver, &msg->payload[1]);
		break;
	case MSP_DISPLAYPORT_DRAW_SCREEN: // 4 -> Draw Screen
		process_draw_complete(display_driver);
		break;
	case MSP_DISPLAYPORT_SET_OPTIONS: // 5 -> Set Options (HDZero/iNav)
		process_set_options(display_driver, &msg->payload[1]);
		break;
	default:
		break;
	}
	return 0;
}

// VTX CAM Menu
static bool newMenu = true;
static int current_selection = 0;
extern uint64_t lastStatusScreen;

// Default path To test on x86
#ifndef CONFIG_PATH
#ifdef _x86
#define CONFIG_PATH "./vtxmenu.ini"
#else
#define CONFIG_PATH "/etc/vtxmenu.ini"
#endif
#endif

bool init_state_manager() {

	if (parse_ini(CONFIG_PATH, &menu_system)) {
		printf("Failed to load menu config /etc/vtxmenu.ini\n");
		return false;
	} else {
		current_section = &menu_system.sections[current_section_index];
		if (verbose)
			print_menu_system_state(&menu_system);
		return true;
	}
	lastStatusScreen = get_current_time_ms();
}

void print_current_state(displayport_vtable_t *display_driver) {
	if (newMenu) {
		display_driver->clear_screen();
		newMenu = false;
	}
	display_menu(display_driver, current_section, selected_option);
}

// Stick command handleing
// stolen from hdzero

#define IS_HI(x) ((x) > 1750)
#define IS_LO(x) ((x) < 1250)
#define IS_MID(x) ((!IS_HI(x)) && (!IS_LO(x)))

// Enum for menu commands
typedef enum { RIGHT, EXIT, UP, DOWN, LEFT, ENTER, VTXMENU, SAFEBOOT, NONE } stickcommands;

int last_command = NONE;
extern bool vtxMenuActive;
extern bool showStatusScreen;

void handle_stickcommands(uint16_t channels[18]) {

	uint8_t IS_HI_yaw = IS_HI(channels[2]);
	uint8_t IS_LO_yaw = IS_LO(channels[2]);
	uint8_t IS_MID_yaw = IS_MID(channels[2]);

	uint8_t IS_HI_throttle = IS_HI(channels[3]);
	uint8_t IS_LO_throttle = IS_LO(channels[3]);
	uint8_t IS_MID_throttle = IS_MID(channels[3]);

	uint8_t IS_HI_pitch = IS_HI(channels[1]);
	uint8_t IS_LO_pitch = IS_LO(channels[1]);
	uint8_t IS_MID_pitch = IS_MID(channels[1]);

	uint8_t IS_HI_roll = IS_HI(channels[0]);
	uint8_t IS_LO_roll = IS_LO(channels[0]);
	uint8_t IS_MID_roll = IS_MID(channels[0]);

	int command = last_command;

	if (IS_HI_yaw && IS_MID_roll && IS_MID_pitch && IS_MID_throttle)
		command = ENTER;
	else if (IS_LO_yaw && IS_MID_roll && IS_MID_pitch)
		command = EXIT;
	else if (IS_MID_yaw && IS_MID_roll && IS_HI_pitch && !IS_HI_throttle)
		command = UP;
	else if (IS_MID_yaw && IS_MID_roll && IS_LO_pitch && !IS_HI_throttle)
		command = DOWN;
	else if (IS_MID_yaw && IS_LO_roll && IS_MID_pitch && !IS_HI_throttle)
		command = LEFT;
	else if (IS_MID_yaw && IS_HI_roll && IS_MID_pitch && !IS_HI_throttle)
		command = RIGHT;
	else if (IS_HI_yaw && IS_LO_throttle && IS_LO_roll && IS_LO_pitch)
		command = VTXMENU;
	else if (IS_HI_yaw && IS_HI_throttle && IS_LO_roll && IS_HI_pitch)
		command = SAFEBOOT;
	else
		command = NONE;

	if (command != last_command) {

		if (!vtxMenuActive) {
			switch (command) {
			case VTXMENU:
				if (verbose)
					printf("Entering vtxMenu\n");
				newMenu = true;
				vtxMenuEnabled = init_state_manager();
				if (vtxMenuEnabled)
					vtxMenuActive = true;
				break;
			case SAFEBOOT:
				safeboot();
			}
			last_command = command;
		} else {
			switch (command) {
			case UP:
				selected_option = (selected_option - 1 + current_section->option_count) %
								  current_section->option_count;
				break;
			case DOWN:
				selected_option = (selected_option + 1) % current_section->option_count;
				break;
			case LEFT: {
				MenuOption *option = &current_section->options[selected_option];

				// Handle list value cycling backward
				switch (option->type) {
				case MENU_OPTION_LIST: {
					char value_list[MAX_VALUE_LIST_ITEMS][MAX_VALUE_LENGTH];
					int value_count;
					split_values(option->values, value_list, &value_count);
					current_section->current_value_index[selected_option] =
						(current_section->current_value_index[selected_option] - 1 + value_count) %
						value_count;
					break;
				}
				case MENU_OPTION_RANGE:
				case MENU_OPTION_FLOATRANGE: {
					// Decrement within range
					if (current_section->current_value_index[selected_option] > option->min) {
						current_section->current_value_index[selected_option]--;
					}
					break;
				}
				default:
					break;
				}
				break;
			}
			case RIGHT: {
				MenuOption *option = &current_section->options[selected_option];

				// Handle list value cycling (forward)
				switch (option->type) {
				case MENU_OPTION_LIST: {
					char value_list[MAX_VALUE_LIST_ITEMS][MAX_VALUE_LENGTH];
					int value_count;
					split_values(option->values, value_list, &value_count);
					// cycle through list
					current_section->current_value_index[selected_option] =
						(current_section->current_value_index[selected_option] + 1) % value_count;
					break;
				}
				case MENU_OPTION_RANGE:
				case MENU_OPTION_FLOATRANGE: {
					// Increment within range
					if (current_section->current_value_index[selected_option] < option->max) {
						current_section->current_value_index[selected_option]++;
					}
					break;
				}
				case MENU_OPTION_SUBMENU: {
					// Handle submenu navigation
					clear_vtx_menu();
					for (int i = 0; i < menu_system.section_count; i++) {
						if (strcmp(menu_system.sections[i].name, option->lable) == 0) {
							current_section_index = i;
							current_section = &menu_system.sections[current_section_index];
							selected_option = 0; // Reset the option selection
							break;
						}
					}
					break;
				}
				case MENU_OPTION_COMMAND: {
					// Execute the command function
					if (option->command_function) {
						option->command_function(current_section);
					}
					showStatusScreen = true;
					lastStatusScreen = abs(get_current_time_ms());
					break;
				}
				default:
					break;
				}
				break;
			}
			case ENTER:
				// handle_selection();
				break;
			case EXIT:
				if (verbose)
					printf("Exit vtxMenu\n");
				newMenu = true;
				vtxMenuActive = false;
				clear_vtx_menu();
				break;
			}
			last_command = command;
		}
	}
}
