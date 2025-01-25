#include <arpa/inet.h>
#include <linux/reboot.h> // For LINUX_REBOOT_CMD_RESTART
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../msposd.h"
#include "../util/ini_parser.h"
#include "msp_displayport.h"
#include "vtxmenu.h"

bool clearNextDraw = false;
bool showStatusScreen = false;
uint64_t lastStatusScreen = 0;
extern bool verbose;
extern bool vtxMenuActive;
extern int out_sock; // to resend
extern struct sockaddr_in sin_out;

extern MenuSection *current_section;
extern int selected_option;
static uint8_t message_buffer[256];
static uint8_t payload_buffer[256];

void exitVTXMenu() { vtxMenuActive = false; }

void clear_vtx_menu() { clearNextDraw = true; }

void display_menu(displayport_vtable_t *display_driver, MenuSection *section, int selected_option) {

	int menu_offset_cols = 4;
	int menu_offset_row = 2;

	if (clearNextDraw) {
		display_driver->clear_screen();
		clearNextDraw = false;
	}

	// Draw status screen when a command was triggered
	if ((get_current_time_ms() - lastStatusScreen > 1500) && showStatusScreen) {
		showStatusScreen = false;
		clearNextDraw = true;
	}
	if (showStatusScreen) {
		for (int row = 0; row < OSD_HD_ROWS - menu_offset_row; row++) {
			for (int col = 0; col < OSD_HD_COLS - menu_offset_cols; col++) {
				display_driver->draw_character(col + menu_offset_cols, row + menu_offset_row, ' ');
			}
		}

		const char *status_msg = "DONE";
		int msg_len = strlen(status_msg);
		int start_col = OSD_HD_COLS / 2 - msg_len / 2;

		for (int i = 0; i < msg_len; i++) {
			display_driver->draw_character(start_col + i, OSD_HD_ROWS / 2, status_msg[i]);
		}
		display_driver->draw_complete();

		if (out_sock > 0) { // Send status screen to the ground
			// Clear the screen remotely
			payload_buffer[0] = MSP_DISPLAYPORT_CLEAR;
			construct_msp_command(
				message_buffer, MSP_CMD_DISPLAYPORT, payload_buffer, 2, MSP_INBOUND);
			sendto(
				out_sock, message_buffer, 6 + 2, 0, (struct sockaddr *)&sin_out, sizeof(sin_out));

			// Draw the "DONE" message remotely
			payload_buffer[0] = MSP_DISPLAYPORT_DRAW_STRING;
			payload_buffer[1] = OSD_HD_ROWS / 2;			   // Row for "DONE"
			payload_buffer[2] = OSD_HD_COLS / 2 - msg_len / 2; // Center column
			payload_buffer[3] = 0;							   // Reserved byte
			memcpy(&payload_buffer[4], status_msg,
				msg_len); // Copy the message into the payload

			construct_msp_command(
				message_buffer, MSP_CMD_DISPLAYPORT, payload_buffer, 4 + msg_len, MSP_INBOUND);
			sendto(out_sock, message_buffer, 6 + 4 + msg_len, 0, (struct sockaddr *)&sin_out,
				sizeof(sin_out));

			// Instruct remote display to draw the complete screen
			payload_buffer[0] = MSP_DISPLAYPORT_DRAW_SCREEN;
			construct_msp_command(
				message_buffer, MSP_CMD_DISPLAYPORT, payload_buffer, 2, MSP_INBOUND);
			sendto(
				out_sock, message_buffer, 6 + 2, 0, (struct sockaddr *)&sin_out, sizeof(sin_out));
		}
		return;
	}

	// Create a 2D array to store the menu representation
	char menu_grid[section->option_count + 1][OSD_HD_COLS - menu_offset_cols];

	// Initialize the 2D array to empty spaces
	for (int i = 0; i < section->option_count + 1; ++i) {
		memset(menu_grid[i], ' ',
			OSD_HD_COLS - menu_offset_cols); // Fill each row with spaces
											 // menu_grid[i][OSD_HD_COLS - 1] = '\0'; // Ensure null
											 // termination for each string row
	}

	int current_row = 0;
	if (verbose)
		printf("\n=== %s ===\n", section->name);
	snprintf(menu_grid[current_row++], OSD_HD_COLS - menu_offset_cols, "=== %s ===", section->name);

	for (int i = 0; i < section->option_count; i++) {
		char row_selectd[3] = "  ";
		if (i == selected_option) {
			snprintf(row_selectd, 3, " > ");
		} else {
			snprintf(row_selectd, 3, "   ");
		}
		MenuOption *option = &section->options[i];
		switch (option->type) {
		case MENU_OPTION_LIST: {
			char value_list[MAX_VALUE_LIST_ITEMS][MAX_VALUE_LENGTH];
			int value_count;
			split_values(option->values, value_list, &value_count);
			if (verbose)
				printf("%s%s: %s\n", row_selectd, option->lable,
					value_list[section->current_value_index[i]]);
			snprintf(menu_grid[current_row++], OSD_HD_COLS - menu_offset_cols, "%s%s: %s",
				row_selectd, option->lable, value_list[section->current_value_index[i]]);
			break;
		}
		case MENU_OPTION_RANGE: {
			if (verbose)
				printf("%s%s: %d\n", row_selectd, option->lable, section->current_value_index[i]);
			snprintf(menu_grid[current_row++], OSD_HD_COLS - menu_offset_cols, "%s%s: %d",
				row_selectd, option->lable, section->current_value_index[i]);
			break;
		}
		case MENU_OPTION_FLOATRANGE: {
			if (verbose)
				printf("%s%s: %.1f\n", row_selectd, option->lable,
					section->current_value_index[i] / 10.0f);
			snprintf(menu_grid[current_row++], OSD_HD_COLS - menu_offset_cols, "%s%s: %.1f",
				row_selectd, option->lable, section->current_value_index[i] / 10.0f);
			break;
		}
		case MENU_OPTION_SUBMENU: {
			int len = strlen(option->lable) + strlen(row_selectd);
			if (verbose)
				printf("%s%s%*s>\n", row_selectd, option->lable, 30 - len, "");
			snprintf(menu_grid[current_row++], OSD_HD_COLS - menu_offset_cols, "%s%s%*s>",
				row_selectd, option->lable, MAX_VTX_MENU_COLS - len, "");
			break;
		}
		case MENU_OPTION_COMMAND: {
			int len = strlen(option->lable) + strlen(row_selectd);
			if (verbose)
				printf("%s%s%*s>\n", row_selectd, option->lable, 30 - len, "");
			snprintf(menu_grid[current_row++], OSD_HD_COLS - menu_offset_cols, "%s%s%*s>",
				row_selectd, option->lable, MAX_VTX_MENU_COLS - len, "");
			break;
		}
		default:
			printf("\n");
			break;
		}
	}

	if (out_sock > 0) { // send   to the ground
		payload_buffer[0] = MSP_DISPLAYPORT_CLEAR;
		construct_msp_command(
			&message_buffer[0], MSP_CMD_DISPLAYPORT, &payload_buffer[0], 2, MSP_INBOUND);
		sendto(out_sock, message_buffer, 6 + 2, 0, (struct sockaddr *)&sin_out, sizeof(sin_out));
	}

	// Draw the populated menu array on the OSD
	for (int row = 0; row < section->option_count + 1; row++) {
		for (int col = 0; col < OSD_HD_COLS - menu_offset_cols; col++) {
			display_driver->draw_character(
				col + menu_offset_cols, row + menu_offset_row, menu_grid[row][col]);
		}
		if (out_sock > 0) { // send the line to the ground
			memcpy(&payload_buffer[4], &menu_grid[row], OSD_HD_COLS - menu_offset_cols);
			payload_buffer[0] = MSP_DISPLAYPORT_DRAW_STRING;
			payload_buffer[1] = row + menu_offset_row;
			payload_buffer[2] = menu_offset_cols;
			payload_buffer[3] = 0;

			construct_msp_command(message_buffer, MSP_CMD_DISPLAYPORT, &payload_buffer[0],
				OSD_HD_COLS - menu_offset_cols + 4, MSP_INBOUND);
			sendto(out_sock, message_buffer, OSD_HD_COLS - menu_offset_cols + 4 + 6, 0,
				(struct sockaddr *)&sin_out, sizeof(sin_out));
		}
	}

	display_driver->draw_complete();

	if (out_sock > 0) { // send  to the ground
		payload_buffer[0] = MSP_DISPLAYPORT_DRAW_SCREEN;
		construct_msp_command(
			message_buffer, MSP_CMD_DISPLAYPORT, &payload_buffer[0], 2, MSP_INBOUND);
		sendto(out_sock, message_buffer, 6 + 2, 0, (struct sockaddr *)&sin_out, sizeof(sin_out));
	}
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
	printf("Running custom command: %s\n", current_section->options[selected_option].read_command);
	char output[MAX_VALUE_LENGTH] = "";
	run_command(current_section->options[selected_option].read_command, output, sizeof(output));
}

void safeboot() {
	printf("Running safeboot command\n");
	char output[MAX_VALUE_LENGTH] = "";
	run_command("/usr/bin/safeboot.sh", output, sizeof(output));
}