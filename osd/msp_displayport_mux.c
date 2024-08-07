#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>

//#include "hw/dji_radio_shm.h"
//#include "json/osd_config.h"
#include "lz4/lz4.h"
//#include "net/data_protocol.h"
//#include "net/network.h"
//#include "net/serial.h"

#include "msp/msp.h"
#include "msp/msp_displayport.h"
#include "msp/msp_displayport.c"//Why needed to compile!

#include "util/debug.h"
#include "util/time_util.h"
#include "util/fs_util.h"

#define CPU_TEMP_PATH "/sys/devices/platform/soc/f0a00000.apb/f0a71000.omc/temp1"
#define AU_VOLTAGE_PATH "/sys/devices/platform/soc/f0a00000.apb/f0a71000.omc/voltage4"

#define FAST_SERIAL_KEY "fast_serial"
#define CACHE_SERIAL_KEY "cache_serial"
#define COMPRESS_KEY "compress_osd"
#define UPDATE_RATE_KEY "osd_update_rate_hz"
#define NO_BTFL_HD_KEY "disable_betaflight_hd"

// The MSP_PORT is used to send MSP passthrough messages.
// The DATA_PORT is used to send arbitrary data - for example, bitrate and temperature data.

#define MSP_PORT 7654
#define DATA_PORT 7655
#define COMPRESSED_DATA_PORT 7656

#define COMPRESSED_DATA_VERSION 1

enum {
    MAX_DISPLAY_X = 60,
    MAX_DISPLAY_Y = 22
};

// The Betaflight MSP minor version in which MSP DisplayPort sizing is supported.
#define MSP_DISPLAY_SIZE_VERSION 45

typedef struct msp_cache_entry_s {
    struct timespec time;
    msp_msg_t message;
} msp_cache_entry_t;

static msp_cache_entry_t *msp_message_cache[256]; // make a slot for all possible messages

static uint8_t frame_buffer[8192]; // buffer a whole frame of MSP commands until we get a draw command
static uint32_t fb_cursor = 0;

static uint8_t message_buffer[256]; // only needs to be the maximum size of an MSP packet, we only care to fwd MSP
static char current_fc_identifier[4];

/* For compressed full-frame transmission */
static uint16_t msp_character_map_buffer[MAX_DISPLAY_X][MAX_DISPLAY_Y];
static uint16_t msp_character_map_draw[MAX_DISPLAY_X][MAX_DISPLAY_Y];
static msp_hd_options_e msp_hd_option = 0;
static displayport_vtable_t *display_driver = NULL;
LZ4_stream_t *lz4_ref_ctx = NULL;
uint8_t update_rate_hz = 2;

int pty_fd;
int serial_fd;
int socket_fd;
int compressed_fd;

 
static uint8_t serial_passthrough = 1;
static uint8_t compress = 0;
static uint8_t no_btfl_hd = 0;

 

  

static void send_display_size(int serial_fd) {
    uint8_t buffer[8];
    uint8_t payload[2] = {MAX_DISPLAY_X, MAX_DISPLAY_Y};
    construct_msp_command(buffer, MSP_CMD_SET_OSD_CANVAS, payload, 2, MSP_OUTBOUND);
    write(serial_fd, &buffer, sizeof(buffer));
}

static void send_variant_request(int serial_fd) {
    uint8_t buffer[6];
    construct_msp_command(buffer, MSP_CMD_FC_VARIANT, NULL, 0, MSP_OUTBOUND);
    write(serial_fd, &buffer, sizeof(buffer));
}

static void send_version_request(int serial_fd) {
    uint8_t buffer[6];
    construct_msp_command(buffer, MSP_CMD_API_VERSION, NULL, 0, MSP_OUTBOUND);
    write(serial_fd, &buffer, sizeof(buffer));
}

static void copy_to_msp_frame_buffer(void *buffer, uint16_t size) {
    memcpy(&frame_buffer[fb_cursor], buffer, size);
    fb_cursor += size;
}

//int displayport_process_message(displayport_vtable_t *display_driver, msp_msg_t *msg) {
//}

static void rx_msp_callback(msp_msg_t *msp_message)
{
    // Process a received MSP message from FC and decide whether to send it to the PTY (DJI) or UDP port (MSP-OSD on Goggles)
    DEBUG_PRINT("FC->AU MSP msg %d with data len %d \n", msp_message->cmd, msp_message->size);
    switch(msp_message->cmd) {
        case MSP_CMD_DISPLAYPORT: {
//TEST CHANGE            
            if (true) {
                // This was an MSP DisplayPort message and we're in compressed mode, so pass it off to the DisplayPort handlers.
                displayport_process_message(display_driver, msp_message);
            } else {
                // This was an MSP DisplayPort message and we're in legacy mode, so buffer it until we get a whole frame.
                if(fb_cursor > sizeof(frame_buffer)) {
                    printf("Exhausted frame buffer! Resetting...\n");
                    fb_cursor = 0;
                    return;
                }
                uint16_t size = msp_data_from_msg(message_buffer, msp_message);
                copy_to_msp_frame_buffer(message_buffer, size);
                if(msp_message->payload[0] == MSP_DISPLAYPORT_DRAW_SCREEN) {
                    // Once we have a whole frame of data, send it to the goggles.
                    write(socket_fd, frame_buffer, fb_cursor);
                    DEBUG_PRINT("DRAW! wrote %d bytes\n", fb_cursor);
                    fb_cursor = 0;
                }
            }
            break;
        }
        case MSP_CMD_FC_VARIANT: {
            // This is an FC Variant response, so we want to use it to set our FC variant.
            DEBUG_PRINT("Got FC Variant response!\n");
            if(strncmp(current_fc_identifier, msp_message->payload, 4) != 0) {
                // FC variant changed or was updated. Update the current FC identifier and send an MSP version request.
                memcpy(current_fc_identifier, msp_message->payload, 4);
                send_version_request(serial_fd);
            }
            break;
        }
        case MSP_CMD_API_VERSION: {
            // Got an MSP API version response. Compare the version if we have Betaflight in order to see if we should send the new display size message.
            if(strncmp(current_fc_identifier, "BTFL", 4) == 0) {
                uint8_t msp_minor_version = msp_message->payload[2];
                DEBUG_PRINT("Got Betaflight minor MSP version %d\n", msp_minor_version);
                if(msp_minor_version >= MSP_DISPLAY_SIZE_VERSION) {
                    if(!no_btfl_hd) {
                        if(!compress) {
                            // If compression is disabled, we need to manually inject a canvas-change command into the command stream.
                            uint8_t displayport_set_size[3] = {MSP_DISPLAYPORT_SET_OPTIONS, 0, MSP_HD_OPTION_60_22};
                            construct_msp_command(message_buffer, MSP_CMD_DISPLAYPORT, displayport_set_size, 3, MSP_INBOUND);
                            copy_to_msp_frame_buffer(message_buffer, 9);
                            DEBUG_PRINT("Sent display size to goggles\n");

                        }
                        // Betaflight with HD support. Send our display size and set 60x22.
                        send_display_size(serial_fd);
                        msp_hd_option = MSP_HD_OPTION_60_22;
                        DEBUG_PRINT("Sent display size to FC\n");
                    }
                }
            }
            break;
        }
        default: {
            uint16_t size = msp_data_from_msg(message_buffer, msp_message);
            if(serial_passthrough /*|| cache_msp_message(msp_message)*/) {
                // Either serial passthrough was on, or the cache was enabled but missed (a response was not available). 
                // Either way, this means we need to send the message through to DJI.
                write(pty_fd, message_buffer, size);
            }
            break;
        }
    }
}
 

/* MSP DisplayPort handlers for compressed mode */

static void msp_draw_character(uint32_t x, uint32_t y, uint16_t c) {
    DEBUG_PRINT("drawing char %d at x %d y %d\n", c, x, y);
    msp_character_map_buffer[x][y] = c;
}

static void msp_clear_screen() {
    memset(msp_character_map_buffer, 0, sizeof(msp_character_map_buffer));
}

static void msp_draw_complete() {
    memcpy(msp_character_map_draw, msp_character_map_buffer, sizeof(msp_character_map_buffer));
}

static void msp_set_options(uint8_t font_num, msp_hd_options_e is_hd) {
   DEBUG_PRINT("Got options!\n");
   msp_clear_screen();
   msp_hd_option = is_hd;
}
 