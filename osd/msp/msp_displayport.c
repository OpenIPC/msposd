#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <linux/reboot.h>  // For LINUX_REBOOT_CMD_RESTART
#include "msp.h"
#include "msp_displayport.h"


static void process_draw_string(displayport_vtable_t *display_driver, uint8_t *payload) {
    if(!display_driver || !display_driver->draw_character) return;
    uint8_t row = payload[0];
    uint8_t col = payload[1];
    uint8_t attrs = payload[2]; // INAV and Betaflight use this to specify a higher page number. 
    uint8_t str_len;
    for(str_len = 1; str_len < 255; str_len++) {
        if(payload[2 + str_len] == '\0') {
            break;
        }
    }
    for(uint8_t idx = 0; idx < (str_len - 1); idx++) {
        uint16_t character = payload[3 + idx];
        if(attrs & 0x3) {
            // shift over by the page number if they were specified
            character |= ((attrs & 0x3) * 0x100);
        }
        display_driver->draw_character(col, row, character);
        col++;
    }
}

static void process_clear_screen(displayport_vtable_t *display_driver) {
    if(!display_driver || !display_driver->clear_screen) return;
    display_driver->clear_screen();
}

static void process_draw_complete(displayport_vtable_t *display_driver) {
    
    if(!display_driver || !display_driver->draw_complete) return;
    display_driver->draw_complete();
}

static void process_set_options(displayport_vtable_t *display_driver, uint8_t *payload) {
    if(!display_driver || !display_driver->set_options) return;
    uint8_t font = payload[0];
    msp_hd_options_e is_hd = payload[1];
    display_driver->set_options(font, is_hd);
}

static void process_open(displayport_vtable_t *display_driver) {

}

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
    switch(sub_cmd) {
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
#define OSD_HD_COLS 53
#define OSD_HD_ROWS 20
#define NUM_OPTIONS 7
#define BUFFER_SIZE 128


static int current_selection = 0;

// Option ranges for dynamic boundaries
static const int option_ranges[NUM_OPTIONS][2] = {
    {0, 4},     // Bitrate range example
    {0, 1},     // Codec range example (e.g., 0 to 5 for different codecs)
    {0, 3},     // Size range example (e.g., resolution width)
    {0, 2},     // FPS range example
    {0, 0},     // Save option doesn't have a range, can be set to 0
    {0, 0},      // Reboot option doesn't have a range, can be set to 0
    {0, 0}      // Save&Reboot option doesn't have a range, can be set to 0

};

// Textual representations for each option value
const char* bitrate_texts[5] = {
    "4096",
    "5120",
    "6144",
    "7168",
    "8192"
};

const char* codec_texts[2] = {
    "H264",
    "H265"
};

const char* size_texts[5] = {
  "1280X720",
  "1920X1080",
  "2944X1656",
  "3840X2160"
};

const char* fps_texts[3] = {
    "60",
    "90",
    "120"
};

// Enum for menu options
typedef enum {
    OPTION_BITRATE,
    OPTION_CODEC,
    OPTION_SIZE,
    OPTION_FPS,
    OPTION_SAVE,
    OPTION_REBOOT,
    OPTION_SAVEREBOOT
} MenuOption;

// Option values
static int options[NUM_OPTIONS] = { 0, 0, 0, 0, 0, 0, 0};

// Labels for menu options (corresponds to the enum values)
const char *option_labels[NUM_OPTIONS] = {
    "BITRATE :",
    "CODEC   :",
    "SIZE    :",
    "FPS     : ",
    "SAVE",
    "REBOOT",
    "SAVE&REBOOT",
};

const char *read_commands[NUM_OPTIONS-2] = {
    "cli -g .video0.bitrate",
    "cli -g .video0.codec",
    "cli -g .video0.size",
    "cli -g .video0.fps"
};

// Commands similar to read_commands but we'll change -g to -s
const char *save_commands[NUM_OPTIONS - 2] = {
    "cli -s .video0.bitrate ",
    "cli -s .video0.codec ",
    "cli -s .video0.size ",
    "cli -s .video0.fps "
};

// Function to convert a string to uppercase
void str_to_upper(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = toupper((unsigned char)str[i]);
    }
}

// Function to convert a string to lowercase
void str_to_lower(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower((unsigned char)str[i]);
    }
}
// Function to find the index of a string in an array
int find_string_index(const char *value, const char *array[], int array_size) {
    for (int i = 0; i < array_size; i++) {
        if (strcmp(value, array[i]) == 0) {
            return i; // Return the index if found
        }
    }
    return -1; // Not found
}

void init_state_manager() {
    for (int i = 0; i < NUM_OPTIONS - 2; ++i) { //save and reboot to not have options
        // Open the command for reading
        FILE *pipe = popen(read_commands[i], "r");
        if (pipe == NULL) {
            fprintf(stderr, "Failed to run command: %s\n", read_commands[i]);
            exit(1);
        }

        // Buffer to store command output
        char buffer[BUFFER_SIZE];

        // Read the output a line at a time
        if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            // Strip newline characters and extra whitespace
            buffer[strcspn(buffer, "\n")] = 0; // Remove the trailing newline

            // Convert buffer to uppercase for comparison
            str_to_upper(buffer);

            // Handle different types of outputs based on the option index
            switch (i) {
                case OPTION_BITRATE: {
                    // Check if the bitrate is in the predefined array
                    int bitrate_index = find_string_index(buffer, bitrate_texts, sizeof(bitrate_texts) / sizeof(bitrate_texts[0]));
                    if (bitrate_index >= 0) {
                        options[i] = bitrate_index; // Store the enum value
                    } else {
                        options[i] = 0; // Handle unknown bitrate (if needed)
                    }
                    break;
                }
                case OPTION_CODEC: {
                    // Check if the codec is in the predefined array
                    int codec_index = find_string_index(buffer, codec_texts, sizeof(codec_texts) / sizeof(codec_texts[0]));
                    if (codec_index >= 0) {
                        options[i] = codec_index; // Store the enum value
                    } else {
                        options[i] = 0; // Handle unknown codec (if needed)
                    }
                    break;
                }
                case OPTION_SIZE: {
                    // Check if the size is in the predefined array
                    int size_index = find_string_index(buffer, size_texts, sizeof(size_texts) / sizeof(size_texts[0]));
                    if (size_index >= 0) {
                        options[i] = size_index; // Store the enum value
                    } else {
                        options[i] = 0; // Handle unknown size (if needed)
                    }
                    break;
                }
                case OPTION_FPS: {
                    // Check if the fps is in the predefined array
                    int fps_index = find_string_index(buffer, fps_texts, sizeof(fps_texts) / sizeof(fps_texts[0]));
                    if (fps_index >= 0) {
                        options[i] = fps_index; // Store the enum value
                    } else {
                        options[i] = 0; // Handle unknown fps (if needed)
                    }
                    break;
                }
            }
        } else {
            fprintf(stderr, "Failed to read output from command: %s\n", read_commands[i]);
        }

        // Close the pipe
        pclose(pipe);
    }
}


int get_current_selection() {
    return current_selection;
}

const char* get_option_label(MenuOption option) {
    if (option >= 0 && option < NUM_OPTIONS) {
        return option_labels[option];
    }
    return "";
}

void move_selection(int direction) {
    current_selection += direction;
    if (current_selection < 0) current_selection = 0;
    if (current_selection >= NUM_OPTIONS) current_selection = NUM_OPTIONS - 1;
}

void change_option(int direction) {
    int new_value = options[current_selection] + direction;
    // Ensure the new value is within defined ranges
    if (new_value < option_ranges[current_selection][0]) {
        new_value = option_ranges[current_selection][0];
    } else if (new_value > option_ranges[current_selection][1]) {
        new_value = option_ranges[current_selection][1];
    }
    options[current_selection] = new_value; // Update option value
}

void print_current_state(displayport_vtable_t *display_driver) {
    display_driver->clear_screen();

    // Create a 2D array to store the menu representation
    char menu_grid[NUM_OPTIONS][OSD_HD_COLS];
    
    // Initialize the 2D array to empty spaces
    for (int i = 0; i < NUM_OPTIONS; ++i) {
        memset(menu_grid[i], ' ', OSD_HD_COLS); // Fill each row with spaces
        menu_grid[i][OSD_HD_COLS - 1] = '\0'; // Ensure null termination for each string row
    }

    // Populate the 2D array with the menu options
    for (int i = 0; i < NUM_OPTIONS; ++i) {
        char buffer[OSD_HD_COLS];
        const char* option_label = get_option_label(i);
        const char* value_text = "";

        // Select the appropriate textual representation based on the option
        switch (i) {
            case OPTION_BITRATE:
                value_text = bitrate_texts[options[i]];
                break;
            case OPTION_CODEC:
                value_text = codec_texts[options[i]];
                break;
            case OPTION_SIZE:
                value_text = size_texts[options[i]];
                break;
            case OPTION_FPS:
                value_text = fps_texts[options[i]];
                break;
            case OPTION_SAVE:
                value_text = "";
                break;
            case OPTION_REBOOT:
                value_text = "";
                break;
            case OPTION_SAVEREBOOT:
                value_text = "";
                break;
        }
        
        // Format the display string
        snprintf(buffer, OSD_HD_COLS, "%-20s %s", option_label, value_text);

        // Mark the selected option with ">"
        if (i == current_selection) {
            snprintf(menu_grid[i], OSD_HD_COLS, "> %-20s %s", option_label, value_text); // Selected option
        } else {
            snprintf(menu_grid[i], OSD_HD_COLS, "  %-20s %s", option_label, value_text); // Non-selected option
        }
    }

    // Draw the populated menu array on the OSD
    for (int row = 0; row < NUM_OPTIONS; row++) {
        for (int col = 0; col < OSD_HD_COLS; col++) {
            display_driver->draw_character(col+5, row+5, menu_grid[row][col]);
        }
    }
    display_driver->draw_complete();
}

// Function to handle saving the settings
void save_settings() {
    printf("Saving settings...\n");
    printf("Bitrate: %s\n", bitrate_texts[options[OPTION_BITRATE]]);
    printf("Codec: %s\n", codec_texts[options[OPTION_CODEC]]);
    printf("Size: %s\n", size_texts[options[OPTION_SIZE]]);
    printf("FPS: %s\n", fps_texts[options[OPTION_FPS]]);

    char command[BUFFER_SIZE];
    char option_value[BUFFER_SIZE];

    // Iterate over each option except for SAVE and REBOOT
    for (int i = 0; i < NUM_OPTIONS - 3; ++i) {
        // Get the current value based on the option
        const char *value_text = "";
        switch (i) {
            case OPTION_BITRATE:
                value_text = bitrate_texts[options[OPTION_BITRATE]];
                break;
            case OPTION_CODEC:
                value_text = codec_texts[options[OPTION_CODEC]];
                break;
            case OPTION_SIZE:
                value_text = size_texts[options[OPTION_SIZE]];
                break;
            case OPTION_FPS:
                value_text = fps_texts[options[OPTION_FPS]];
                break;
        }

        // Convert the value text to uppercase
        strncpy(option_value, value_text, sizeof(option_value));
        str_to_lower(option_value);

        // Create the full save command (e.g., cli -s .video0.bitrate 8192)
        snprintf(command, sizeof(command), "%s%s", save_commands[i], option_value);

        // Print the command for debugging (optional)
        printf("Executing: %s\n", command);

        // Execute the command using popen
        FILE *pipe = popen(command, "r");
        if (pipe == NULL) {
            fprintf(stderr, "Failed to run command: %s\n", command);
            continue;
        }

        // Read the output to confirm the command ran successfully (if needed)
        char buffer[BUFFER_SIZE];
        if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            printf("Command output: %s\n", buffer);
        }

        // Close the pipe
        pclose(pipe);
    }

    printf("Settings have been saved.\n");
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


void handle_selection() {
    switch (current_selection) {
        case OPTION_BITRATE:
            // Logic for selecting bitrate
            break;
        case OPTION_CODEC:
            // Logic for selecting codec
            break;
        case OPTION_SIZE:
            // Logic for selecting size
            break;
        case OPTION_FPS:
            // Logic for selecting FPS
            break;
        case OPTION_SAVE:
            save_settings(); // Call the save function
            break;
        case OPTION_REBOOT:
            doreboot(); // Call the reboot function
            break;
        case OPTION_SAVEREBOOT:
            save_settings(); // Call the save function
            doreboot(); // Call the reboot function
            break;
        default:
            break; // Handle unexpected cases if necessary
    }
}



//Stick command handleing
//stolen from hdzero

#define IS_HI(x)  ((x) > 1750)
#define IS_LO(x)  ((x) < 1250)
#define IS_MID(x) ((!IS_HI(x)) && (!IS_LO(x)))


// Enum for menu commands
typedef enum {
    RIGHT,
    EXIT,
    UP,
    DOWN,
    LEFT,
    ENTER,
    VTXMENU,
    NONE
} stickcommands;

int last_command = NONE;
extern bool vtxMenuActive;

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
    command=ENTER;
  else if (IS_LO_yaw && IS_MID_roll && IS_MID_pitch)
    command=EXIT;
  else if (IS_MID_yaw && IS_MID_roll && IS_HI_pitch && !IS_HI_throttle)
    command=UP;
  else if (IS_MID_yaw && IS_MID_roll && IS_LO_pitch && !IS_HI_throttle)
    command=DOWN;
  else if (IS_MID_yaw && IS_LO_roll && IS_MID_pitch && !IS_HI_throttle)
    command=LEFT;
  else if (IS_MID_yaw && IS_HI_roll && IS_MID_pitch && !IS_HI_throttle)
    command=RIGHT;
  else if (IS_HI_yaw && IS_LO_throttle && IS_LO_roll && IS_LO_pitch)
    command=VTXMENU;
  else
    command=NONE;

    if (command != last_command) {


        switch (command) {
            case VTXMENU:
                printf("Entering vtxMenu\n");
                vtxMenuActive = true;
                init_state_manager();
                break;
            case UP:
                move_selection(-1); // Move up
                break;
            case DOWN:
                move_selection(1); // Move down
                break;
            case LEFT:
                change_option(-1); // Decrease the selected option
                break;
            case RIGHT:
                change_option(1); // Increase the selected option
                break;
            case ENTER:
                handle_selection();
                break;           
            case EXIT:
                printf("Exit vtxMenu\n");
                vtxMenuActive = false;
                break;
        }
        last_command=command;
    }

}