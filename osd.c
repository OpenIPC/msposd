#include <stdio.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <math.h>


#include "osd/msp/msp.h"
#include "osd/msp/msp_displayport.h"

#ifdef _x86
    #include <SFML/Graphics.h>
    #include <SFML/Window.h>
 #else
    #include "bmp/region.h"
    #include "bmp/common.h"    
#endif
#include "bmp/region.h"
#include "bmp/bitmap.h"
#include "libpng/lodepng.h"


#define X_OFFSET 0

#define PORT 7654

#define CLOCK_MONOTONIC 1

/*------------------------------------------------------------------------------------------------------*/
/*------------MSP PROTOCOL MSG PROCESSING  ----------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
 

//#include "osd/msp/msp.h"
//#include "osd/msp/msp_displayport.h"


#include "osd/util/debug.h"
#include "osd/util/time_util.h"
#include "osd/util/fs_util.h"

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
static char current_fc_identifier_end_of_string=0x00;

/* For compressed full-frame transmission */
static uint16_t msp_character_map_buffer[MAX_DISPLAY_X][MAX_DISPLAY_Y];
static uint16_t msp_character_map_draw[MAX_DISPLAY_X][MAX_DISPLAY_Y];
static msp_hd_options_e msp_hd_option = 0;
static displayport_vtable_t *display_driver = NULL;

uint8_t update_rate_hz = 2;

int pty_fd;
int serial_fd;
int socket_fd;
int compressed_fd;

int font_pages=2;
static uint8_t serial_passthrough = 1;
static uint8_t compress = 0;
static uint8_t no_btfl_hd = 0;

static int16_t last_pitch = 0;
static int16_t last_roll = 0;

static int16_t last_RC_Channels[16];

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
static int stat_msp_msgs=0;
static int stat_msp_msg_attitude=0;
static int stat_screen_refresh_count=0;
static int stat_draw_overlay_1=0, stat_draw_overlay_2=0, stat_draw_overlay_3=0;

extern void showchannels(int count);
extern void ProcessChannels();
extern uint16_t channels[18];
static void rx_msp_callback(msp_msg_t *msp_message)
{
    // Process a received MSP message from FC and decide whether to send it to the PTY (DJI) or UDP port (MSP-OSD on Goggles)
    DEBUG_PRINT("FC->AU MSP msg %d with data len %d \n", msp_message->cmd, msp_message->size);
    stat_msp_msgs++;
    switch(msp_message->cmd) {
         case MSP_ATTITUDE: {

            last_pitch = *(int16_t*)&msp_message->payload[2];
            last_roll = *(int16_t*)&msp_message->payload[0];
            stat_msp_msg_attitude++;
           //printf("\n Got MSG_ATTITUDE            pitch:%d  roll:%d\n", pitch, roll);
           break;
         }
        case MSP_RC: {
              //printf("Got MSP_RC \n");
              //memcpy(&channels[0], &msp_message->payload[0],32);
              	memcpy(&channels[0], &msp_message->payload[0], 16 * sizeof(uint16_t));	

	            showchannels(16);		
            	ProcessChannels();
              break;
         }
         
        case MSP_CMD_DISPLAYPORT: {
//TEST CHANGE            
            if (true) {
                // This was an MSP DisplayPort message and we're in compressed mode, so pass it off to the DisplayPort handlers.
                displayport_process_message(display_driver, msp_message);
           // } else {
                // This was an MSP DisplayPort message and we're in legacy mode, so buffer it until we get a whole frame.
                if(fb_cursor > sizeof(frame_buffer)) {
                    printf("Exhausted frame buffer! Resetting...\n");
                    fb_cursor = 0;
                    return;
                }
                uint16_t size = msp_data_from_msg(message_buffer, msp_message);
                copy_to_msp_frame_buffer(message_buffer, size);
                if(msp_message->payload[0] == MSP_DISPLAYPORT_DRAW_SCREEN) {
                    // Once we have a whole frame of data, send it DEBUG it !!!
                    /*
                    int written=write(socket_fd, frame_buffer, fb_cursor);                    
                    DEBUG_PRINT("DRAW! wrote %d bytes %d\n", fb_cursor, written);
                    */
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
                //Seems only BetaFlight needs this
                send_version_request(serial_fd);
                printf("Flight Controller detected:%s\r\n",current_fc_identifier);
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
 




/*----------------------------------------------------------------------------------------------------*/
/*------------CONFIGURE SWITCHES ----------------------------------------------------------------*/
static int enable_fast_layout = 0;

void *io_map;
struct osd *osds;
char timefmt[32] = DEF_TIMEFMT;
int PIXEL_FORMAT_BitsPerPixel = 8;


typedef struct display_info_s {
    uint8_t char_width;
    uint8_t char_height;
    uint8_t font_width;
    uint8_t font_height;
    uint16_t num_chars;
} display_info_t; 

#define SD_DISPLAY_INFO {.char_width = 31, .char_height = 15, .font_width = 36, .font_height = 54, .num_chars = 256}

static const display_info_t sd_display_info = SD_DISPLAY_INFO;

static const display_info_t hd_display_info = {
    .char_width = 50,
    .char_height = 18,
    .font_width = 24,
    .font_height = 36,
    .num_chars = 512,
};

/*
36*50=1800
54*18=972
*/
#define MAX_OSD_WIDTH 50
#define MAX_OSD_HEIGHT 18

// for x86 preview only

#define TRANSPARENT_COLOR 0xFBDE

//Not implemented, draw the center part of the screen with much faster rate to keep CPU load low
#define FULL_OVERLAY_ID 1
#define FAST_OVERLAY_ID 2

char font_2_name[256];

uint16_t OVERLAY_WIDTH =1800;
uint16_t OVERLAY_HEIGHT =1000;


static displayport_vtable_t *display_driver;

static display_info_t current_display_info = SD_DISPLAY_INFO;

static int  MinTimeBetweenScreenRefresh;

//bounderies as characters of fast area. Fast Character
int fcX= 12; int fcW=10; int fcY= 5 ; int fcH=8;

#ifdef _x86
    sfTexture *font_1;
    sfTexture *font_2;
    sfSprite *font_sprite_1;
    sfSprite *font_sprite_2;
    sfRenderWindow *window;
#endif


static int load_fontFromBMP(const char *filename, BITMAP *bitmap){
    if (access(filename, F_OK))
        return -1;                                                

    //White will be the transparant color
    return prepare_bitmap(filename, bitmap, 2, TRANSPARENT_COLOR, PIXEL_FORMAT_1555);                                    
      
}


// Function to move the cursor to a specific position
void move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

// Function to draw a character at a specific position
void draw_character_on_console(int row, int col, char ch) {
    move_cursor(row, col);
    printf("%c", ch);
}

uint64_t get_time_ms() // in milliseconds
{
    struct timespec ts;
    int rc = clock_gettime(1 /*CLOCK_MONOTONIC*/, &ts);
    //if (rc < 0) 
//		return get_current_time_ms_Old();
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

BITMAP bitmapFnt;
uint16_t character_map[MAX_OSD_WIDTH][MAX_OSD_HEIGHT];

struct osd *osds;//regions over the overlay

static long cntr=-10;

int center_refresh=0;


static unsigned long long LastCleared=0;
static unsigned long long LastDrawn=0;

 
//This will be where we will copy font icons and then pass to Display API  to render over video.
BITMAP bmpBuff;

int x_start = 600;
int y_start = 500;
int x_end = 1300;
int y_end = 500;

 
// Function to rotate a point (x, y) around the center of an image
 

// Function to rotate a point around the center of an image
/*
void rotate_point(Point original, Point img_center, double angle_degrees, Point *rotated) {
    // Translate the point to move the center to the origin
    int x_translated = original.x - img_center.x;
    int y_translated = original.y - img_center.y;

    // Convert the angle from degrees to radians
    double angle_radians = angle_degrees * M_PI / 180.0;

    // Compute the rotated positions using the rotation matrix
    double cos_theta = cos(angle_radians);
    double sin_theta = sin(angle_radians);

    // Apply the rotation
    rotated->x = (int)(x_translated * cos_theta - y_translated * sin_theta);
    rotated->y = (int)(x_translated * sin_theta + y_translated * cos_theta);

    // Translate the rotated point back to the original center
    rotated->x += img_center.x;
    rotated->y += img_center.y;
}
*/
 static void draw_AHI(){
      
    int OffsY= sin((last_pitch/10) * (M_PI / 180.0))*400;
    
    int img_width = x_end-x_start;
    int img_height = y_end-y_start;
    
    double angle_degrees = -last_roll/10;  // Rotate by 45 degrees
    Transform_OVERLAY_WIDTH=OVERLAY_WIDTH;
    Transform_OVERLAY_HEIGHT=OVERLAY_HEIGHT;
    Transform_Pitch=last_pitch/10;
    Transform_Roll=-last_roll/10; 
    
    Point img_center = {OVERLAY_WIDTH/2, OVERLAY_HEIGHT/2};  // Center of the image (example)
    Point original_point = {600, 500};  // Example point
    Point original_point2 = {1300, 500};  // Example point
    
    drawLineI4Ex(bmpBuff.pData, bmpBuff.u32Width, bmpBuff.u32Height, original_point,original_point2, 7);
    original_point = (Point){600, 480}; original_point2 = (Point){600, 520};
    drawLineI4Ex(bmpBuff.pData, bmpBuff.u32Width, bmpBuff.u32Height, original_point,original_point2, 7);
    original_point = (Point){1300, 480}; original_point2 = (Point){1300, 520};
    drawLineI4Ex(bmpBuff.pData, bmpBuff.u32Width, bmpBuff.u32Height, original_point,original_point2, 7);

     drawRectangleI4(bmpBuff.pData, 600 , 400 , 700 , 6, COLOR_GREEN, 1);

 }

 static void draw_Ladder(){
              
    Transform_OVERLAY_WIDTH=OVERLAY_WIDTH;
    Transform_OVERLAY_HEIGHT=OVERLAY_HEIGHT;
    Transform_Pitch=last_pitch/10;
    Transform_Roll=-last_roll/10; 
    
    //Point img_center = {OVERLAY_WIDTH/2, OVERLAY_HEIGHT/2};  // Center of the image (example)
    //Point original_point = {600, 500};  // Example point
    //Point original_point2 = {1300, 500};  // Example point
     
    
    const bool horizonInvertPitch = false;
    const bool horizonInvertRoll = false;
    double horizonWidth = 3;
    const int horizonSpacing = 165;//????//pixels per degree 165
    const bool horizonShowLadder = true;
    int horizonRange = 80; //total vertical range in degrees
    const int horizonStep = 10;//????//degrees per line
    const bool show_center_indicator = false;////m_show_center_indicator;
    
    const double ladder_stroke_faktor=0.1;
    const int subline_thickness=2;

    if (OVERLAY_HEIGHT<900){//720p mode
        horizonWidth = 2;
        horizonRange = 50;
    }

    double roll_degree = ((double)last_roll)/10;
    double pitch_degree = ((double)last_pitch)/10;

    if (horizonInvertRoll == true){
        roll_degree=roll_degree*-1;
    }
    if (horizonInvertPitch == false){
        pitch_degree=pitch_degree*-1;
    }
    //weird rounding issue where decimals make ladder dissappear
    roll_degree = round(roll_degree);
    //pitch_degree = round(pitch_degree);

    //std::string str = std::to_string(pitch_degree);
    //qDebug()<<"Roll:"<<roll_degree<<" Pitch:"<<pitch_degree<< " : " << str.c_str();

    int TiltY= - 150;//pixels, negative value means up
    const int pos_x= OVERLAY_WIDTH/2;
    const int pos_y= (OVERLAY_HEIGHT/2)  + TiltY;
    const int width_ladder= 100*horizonWidth;

    int px = pos_x - width_ladder / 2;


    if(/*show_center_indicator*/true){

/*
        drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y-50, px+100,pos_y-60, COLOR_WHITE);
        drawLineI4AA(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y-48, px+100,pos_y-58);
        drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y-46, px+100,pos_y-56, COLOR_WHITE);
        drawLineI4AA(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y-45, px+100,pos_y-55);
        drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y-40, px+100,pos_y-50, COLOR_BLACK);
*/

        //painter->setPen(m_color);
        // Line always drawn in the center to have some orientation where the center is
        const auto line_w= 100*horizonWidth * 0.2;
        //const auto circle_r= 100*horizonWidth * 0.05;
        //painter->drawLine(width()/2-(line_w/2),height()/2,width()/2+(line_w/2),height()/2);
        //painter->drawEllipse(QPointF(width()/2,height()/2),circle_r,circle_r);
         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y,px+width_ladder,pos_y, COLOR_GREEN);
        
         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y-2,px+20,pos_y-2, COLOR_WHITE);         
         drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px-20,pos_y-3,px+20,pos_y-3, COLOR_GRAY_Light,1);    
         drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px-20,pos_y-4,px+20,pos_y-4, COLOR_GRAY_Light,1);
         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y-5,px+20,pos_y-5, COLOR_WHITE);
        

         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y+2,px+20,pos_y+2, COLOR_WHITE);
         drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px-20,pos_y+3,px+20,pos_y+3, COLOR_GRAY_Light,1);
         drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px-20,pos_y+4,px+20,pos_y+4, COLOR_GRAY_Light,1);
         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px,pos_y+5,px+20,pos_y+5, COLOR_WHITE);

         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder-20,pos_y-2,px+width_ladder,pos_y-2, COLOR_WHITE);
         drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder-20,pos_y-3,px+width_ladder+20,pos_y-3, COLOR_GRAY_Light,1);
         drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder-20,pos_y-4,px+width_ladder+20,pos_y-4, COLOR_GRAY_Light,1);
         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder-20,pos_y-5,px+width_ladder,pos_y-5, COLOR_WHITE);

         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder-20,pos_y+2,px+width_ladder,pos_y+2, COLOR_WHITE);
         drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder-20,pos_y+3,px+width_ladder+20,pos_y+3, COLOR_GRAY_Light,1);
         drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder-20,pos_y+4,px+width_ladder+20,pos_y+4, COLOR_GRAY_Light,1);
         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder-20,pos_y+5,px+width_ladder,pos_y+5, COLOR_WHITE);


         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder,pos_y,px+width_ladder,pos_y, COLOR_GREEN);
         //drawLineI4(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT,  px+width_ladder,pos_y,px+width_ladder,pos_y, COLOR_GREEN);
    }
/*
    painter->translate(width()/2,height()/2);
    painter->rotate(roll_degree*-1);
    painter->translate((width()/2)*-1,(height()/2)*-1);
*/
 

    

    int ratio = horizonSpacing; //pixels per degree
    int vrange = horizonRange; //total vertical range in degrees
    int step = horizonStep; //degrees per line
    if (step == 0) step = 10;  // avoid div by 0
    if (ratio == 0) ratio = 1; // avoid div by 0

    int m_color=7;
    int i;
    int k;
    int y;
    int n;
    int startH;
    int stopH;
    startH = pitch_degree - vrange/2;
    stopH = pitch_degree + vrange/2;
    if (startH<-90) startH = -90;
    if (stopH>90) stopH = 90;

    //painter->setPen(m_color);
    int m_color_def=m_color;
    for (i = startH/step; i <= stopH/step; i++) {
        m_color = m_color_def;
        if (i>0 && i*ratio<30 && /*showHorizonHeadingLadder*/ true ) i=i+ 30/ratio;

        k = i*step;
        y = pos_y - (i - 1.0*pitch_degree/step)*ratio;
        if (horizonShowLadder == true) {
            if (i != 0) {

                //fix pitch line wrap around at extreme nose up/down
                n=k;//this is the line index number relative to the main one.
                if (n>90){
                    n=180-k;
                }
                if (n<-90){
                    n=-k-180;
                }
                if (abs(n)>20)//pitch higher than 30 degree.
                    m_color=COLOR_MAGENTA;
                if (abs(n)>40)//pitch higher than 30 degree.
                    m_color=COLOR_RED;

                //left numbers
               // painter->setPen(m_color);
               // painter->drawText(px-30, y+6, QString::number(n));

                //right numbers
                //painter->drawText((px + width_ladder)+8, y+6, QString::number(n));
               // painter->setPen(m_color);

                if ((i > 0)) {
                    //Upper ladders
                    // default to stroke strength of 2 (I think it is pixels)
                    const auto stroke_s=2*ladder_stroke_faktor;
        

                    //left upper cap
                    //drawRectangleI4(bmpBuff.pData, px , y , stroke_s , width_ladder/24, m_color,subline_thickness);                    
                    drawLine(bmpBuff.pData, px, y, px, y+width_ladder/24 , m_color, subline_thickness+1); // Top side    

                    //left upper line
                    //drawRectangleI4(bmpBuff.pData, px , y , width_ladder/3 , stroke_s, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px, y, px + width_ladder/3, y , m_color, subline_thickness); // Top side    

                    //right upper cap
                    //drawRectangleI4(bmpBuff.pData, px+width_ladder-2 , y , px+width_ladder-2 + stroke_s , width_ladder/24, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px+width_ladder-2 , y , px+width_ladder-2, y+width_ladder/24 , m_color, subline_thickness+1); // Top side    

                    //right upper line
                    //drawRectangleI4(bmpBuff.pData, px+width_ladder*2/3 , y , width_ladder/3 , stroke_s, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px+width_ladder*2/3 , y  , (px+width_ladder*2/3) + width_ladder/3 , y , m_color, subline_thickness); // Top side    



                } else if (i < 0) {
                    // Lower ladders
                    // default to stroke strength of 2 (I think it is pixels)
                    
                    const auto stroke_s=2*ladder_stroke_faktor;

                    //left to right
                    //left lower cap                    
                    //drawRectangleI4(bmpBuff.pData, px, y-(width_ladder/24)+2 , stroke_s , width_ladder/24, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px, y-(width_ladder/24)+2 , px , y-(width_ladder/24)+2  + width_ladder/24 , m_color, subline_thickness); // Top side    

                    //1l                    
                    //drawRectangleI4(bmpBuff.pData, px , y , width_ladder/12 , stroke_s, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px , y , px + width_ladder/12 , y , m_color, subline_thickness); // Top side    

                    //2l                    
                    //drawRectangleI4(bmpBuff.pData, px+(width_ladder/12)*1.5 , y , width_ladder/12 , stroke_s, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px+(width_ladder/12)*1.5 , y , px+(width_ladder/12)*1.5 + width_ladder/12 , y , m_color, subline_thickness); // Top side    

                    //3l
                    //drawRectangleI4(bmpBuff.pData, px+(width_ladder/12)*3 , y , width_ladder/12 , stroke_s, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px+(width_ladder/12)*3 , y ,px+(width_ladder/12)*3  +  width_ladder/12 , y , m_color, subline_thickness); // Top side    

                    //right lower cap
                    //drawRectangleI4(bmpBuff.pData, px+width_ladder-2 , y-(width_ladder/24)+2 , stroke_s , width_ladder/24, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px+width_ladder-2 , y-(width_ladder/24)+2 , px+width_ladder-2 ,   y-(width_ladder/24)+2 + width_ladder/24 , m_color, subline_thickness); // Top side    

                    //1r ///spacing on these might be a bit off
                    //drawRectangleI4(bmpBuff.pData, px+(width_ladder/12)*8 , y , width_ladder/12 , stroke_s, m_color,subline_thickness);
                    drawLine(bmpBuff.pData, px+(width_ladder/12)*8 , y , px+(width_ladder/12)*8  + width_ladder/12 , y , m_color, subline_thickness); // Top side    


                    //2r ///spacing on these might be a bit off
                    //drawRectangleI4(bmpBuff.pData, px+(width_ladder/12)*9.5 , y , width_ladder/12 , stroke_s, m_color,subline_thickness);
                    drawLine(bmpBuff.pData,  px+(width_ladder/12)*9.5 , y , px+(width_ladder/12)*9.5 + width_ladder/12 , y , m_color, subline_thickness); // Top side    

                    //3r  ///spacing on these might be a bit off tried a decimal here
                    //drawRectangleI4(bmpBuff.pData, px+(width_ladder*.9166) , y , width_ladder/12 , stroke_s, m_color,subline_thickness);
                    drawLine(bmpBuff.pData,  px+(width_ladder*.9166) , y , px+(width_ladder*.9166)  + width_ladder/12 , y , m_color, subline_thickness); // Top side    
                    
                    

                }
            } else { // i==0
                //Main AHI Line
                // default to stroke strength of 3 - a bit bigger than the non center lines
                const auto stroke_s=4*ladder_stroke_faktor;

                
                int rect_height = 5;            // Height of the rectangles (as per your original code)
                int fragments=6;
                float  SpacingK=0.6;
                //int rect_width = (width_ladder*2.5/(fragments + (float)SpacingK*fragments));  // Width of a single rectangle                
                int rect_width = width_ladder*2.5/(fragments);  // Width of a single semiline+spacing

                int spacing = rect_width*SpacingK ;   // Spacing between rectangles

                rect_width = rect_width - spacing;//Length of a small line
                // Calculate the starting X position for the first rectangle
                int start_x = pos_x-width_ladder*2.5/2;

                // Draw 6 rectangles in a line
                for (int i = 0; i < fragments; i++) {
                    // Calculate the X position for the current rectangle
                    int rect_x = start_x + i * (rect_width + spacing) + spacing/2;
                    // Draw the rectangle
                    //drawRectangleI4(bmpBuff.pData, rect_x, y, rect_width, rect_height, COLOR_WHITE, 1);//border with AA
                    //drawRectangleI4(bmpBuff.pData, rect_x, y, rect_width, rect_height , m_color, 100);//solid
                    //drawRectangleI4(bmpBuff.pData, rect_x, y, rect_width, rect_height, COLOR_WHITE, 1);//border with AA
                    //drawRectangleI4(bmpBuff.pData, rect_x, y, rect_width, 0, COLOR_WHITE, 4);//border with AA
                    drawLine(bmpBuff.pData, rect_x, y, rect_x + rect_width, y, COLOR_WHITE, 3);
                    //drawFilledRectangleI4AA(bmpBuff.pData, OVERLAY_WIDTH, OVERLAY_HEIGHT, rect_x, y, rect_width, rect_height);
                    
                }
             }
        }
    }

 
 }

static void draw_screenBMP(){
    
    if (cntr++<0 )//skip in the beginning to show to font preview
        return ;
	if ( (get_time_ms() - LastDrawn  ) < MinTimeBetweenScreenRefresh){//Set some delay to keep CPU load low
        // printf("%lu DrawSkipped LastDrawn:%lu\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)LastDrawn);
		return ;
    }

    if ( (get_time_ms() - LastCleared  ) < 20){//at least 20ms to reload data after clearscreen, otherwise the screen will blink?
        //printf("%lu DrawSkipped after clear LastDrawn:%lu | %lu%\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)LastDrawn%10000 , (uint32_t) LastCleared%10000);
		return ;
    }
    
    LastDrawn= get_time_ms();

    if (bmpBuff.pData==NULL){
        //bmpBuff.u32Width = 
        bmpBuff.enPixelFormat=PIXEL_FORMAT_DEFAULT;
        bmpBuff.u32Width=current_display_info.font_width * current_display_info.char_width;
        bmpBuff.u32Height = current_display_info.font_height * current_display_info.char_height;
        //bmpBuff.pData = malloc( PIXEL_FORMAT_BitsPerPixel  * bmpBuff.u32Height * bmpBuff.u32Width / 8);
        bmpBuff.pData = malloc( PIXEL_FORMAT_BitsPerPixel  * bmpBuff.u32Height * getRowStride(bmpBuff.u32Width , PIXEL_FORMAT_BitsPerPixel));
        
    }else
        bmpBuff.pData = memset(bmpBuff.pData, 0xFF , PIXEL_FORMAT_BitsPerPixel  * bmpBuff.u32Height * getRowStride(bmpBuff.u32Width , PIXEL_FORMAT_BitsPerPixel));

    bmpBuff.enPixelFormat =  PIXEL_FORMAT_DEFAULT;//  PIXEL_FORMAT_DEFAULT ;//PIXEL_FORMAT_1555;

    // a Character icon size in the FontTemplate
   // current_display_info.font_width = bitmapFnt.u32Width/2;
   // current_display_info.font_height = bitmapFnt.u32Height/256;
 


    for (int y = 0; y < current_display_info.char_height; y++){
        for (int x = 0; x < current_display_info.char_width; x++){
            uint16_t c = character_map[x][y];
            if (c != 0){
                uint8_t page = 0;
                if (c > 255) {
                    page = 1;
                    c = c & 0xFF;
                }
               
                //Find the coordinates if the the rectangle in the Font Bitmap
                u_int16_t s_left = page*current_display_info.font_width;
                u_int16_t s_top = current_display_info.font_height * c;
                u_int16_t s_width = current_display_info.font_width;
                u_int16_t s_height = current_display_info.font_height;                                
                //the location in the screen bmp where we will place the character glyph
                u_int16_t d_x=x * current_display_info.font_width + X_OFFSET;
                u_int16_t d_y=y * current_display_info.font_height;

                if (PIXEL_FORMAT_DEFAULT==0)                
                 copyRectARGB1555(bitmapFnt.pData,bitmapFnt.u32Width,bitmapFnt.u32Height,
                                 bmpBuff.pData,bmpBuff.u32Width, bmpBuff.u32Height,
                                 s_left,s_top,s_width,s_height,
                                 d_x,d_y);
                else if (PIXEL_FORMAT_DEFAULT==3)   
                copyRectI4(bitmapFnt.pData,bitmapFnt.u32Width,bitmapFnt.u32Height,
                                bmpBuff.pData,bmpBuff.u32Width, bmpBuff.u32Height,
                                s_left,s_top,s_width,s_height,
                                d_x,d_y);
                else                                
                copyRectI8(bitmapFnt.pData,bitmapFnt.u32Width,bitmapFnt.u32Height,
                                bmpBuff.pData,bmpBuff.u32Width, bmpBuff.u32Height,
                                s_left,s_top,s_width,s_height,
                                d_x,d_y);


            }else{
            }
           // DEBUG_PRINT("  ");
        }
       // DEBUG_PRINT("\n");
    }



    uint64_t step2=get_time_ms();  

    //draw_AHI();
    
    draw_Ladder();
    
    stat_screen_refresh_count++;
    uint64_t step3=get_time_ms();  
#ifdef _x86
    
    
    sfRenderWindow_clear(window, sfColor_fromRGB(175, 195, 255));
    unsigned char* rgbaData = malloc(bmpBuff.u32Width * bmpBuff.u32Height * 4);  // Allocate memory for RGBA data    

    if (PIXEL_FORMAT_DEFAULT==0)          
        Convert1555ToRGBA( bmpBuff.pData, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height);    
    else if (PIXEL_FORMAT_DEFAULT==4) //I8 one byte format
        ConvertI8ToRGBA( bmpBuff.pData, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height,&g_stPaletteTable);    
    else if (PIXEL_FORMAT_DEFAULT==3) //I4 half byte format
        ConvertI4ToRGBA( bmpBuff.pData, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height,&g_stPaletteTable);     

    sfTexture* texture = sfTexture_create(bmpBuff.u32Width, bmpBuff.u32Height);
    if (!texture) 
        return;

    sfTexture_updateFromPixels(texture, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height, 0, 0);
    free(rgbaData);  
    // Step 2: Create a sprite and set the texture
    sfSprite* sprite = sfSprite_create();
    sfSprite_setTexture(sprite, texture, sfTrue);
    // Set the position where you want to draw the sprite
    sfVector2f position = {1, 1};
    sfSprite_setPosition(sprite, position); 

    sfRenderWindow_drawSprite(window, sprite, NULL);

    // Cleanup resources
    sfSprite_destroy(sprite);
    sfTexture_destroy(texture);
    //printf("%lu set_bitmapC for:%u | %u  | %u ms   pitch:%d  roll:%d\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)(get_time_ms() - LastDrawn) , (uint32_t)(get_time_ms() - step2),(uint32_t)(get_time_ms() - step3), last_pitch, last_roll);
    
 

#else
   int id=0;
   // printf("%lu set_bitmapB for:%d | %d ms\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)(get_time_ms() - LastDrawn));
    
    set_bitmap(osds[FULL_OVERLAY_ID].hand, &bmpBuff);
    //printf("%lu set_bitmapB for:%u | %u   | %u ms\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)(get_time_ms() - LastDrawn) , (uint32_t)(get_time_ms() - step2),(uint32_t)(get_time_ms() - step3));

#endif
    stat_draw_overlay_1+=(uint32_t)(get_time_ms() - LastDrawn);
    stat_draw_overlay_2+=(uint32_t)(get_time_ms() - step2);
    stat_draw_overlay_3+=(uint32_t)(get_time_ms() - step3);


    //free(bitmap.pData);
}

// ---------------------------------------------------
// ---- Functions to process MSP messages 
//  ---------------------------------------------------

static void draw_character(uint32_t x, uint32_t y, uint16_t c)
{
    //draw_character_on_console(y,x,c);
    if (x > current_display_info.char_width - 1 || y > current_display_info.char_height - 1)
    {
        return;
    }
    character_map[x][y] = c;
}

static void clear_screen()
{
    if (cntr++<0 )
        return ;
    //BetaFlight needs this. INAV can be configured to skip it
    if (font_pages>2 || get_time_ms() - LastCleared>2500) {//no faster than 0.5 per second
        memset(character_map, 0, sizeof(character_map));
        if (bmpBuff.pData!=NULL)//Set whole BMP as transparant, Palette index 0xF if 4bit
            bmpBuff.pData = memset(bmpBuff.pData, 0xFF , (bmpBuff.u32Height * bmpBuff.u32Width * PIXEL_FORMAT_BitsPerPixel / 8) );

        LastCleared=(get_time_ms());
        printf("%lu Clear screen\n",(uint32_t)(LastCleared%10000));
        //LastDrawn=(get_time_ms())+500;///give 200ms no refresh  to load data in buffer 
    }else
         printf("%lu Clear screen skipped\n",(uint32_t)((uint32_t)get_time_ms())%10000 );
    //LastDrawn=(get_time_ms())+200;//give 120ms more to load data 
}
static int draws=0;
static void draw_complete()
{
    //draw_screenBMP();
    if (enable_fast_layout){
        draws++;
        if(draws%5==1)
            draw_screenBMP();
        else
            {/*draw_screenCenter();*/}//not used
    }else
        draw_screenBMP();

#ifdef _x86
    sfRenderWindow_display(window);
#endif    
    DEBUG_PRINT("draw complete!\n");
    
}

static void msp_callback(msp_msg_t *msp_message)
{
    displayport_process_message(display_driver, msp_message);
}

static void set_options(uint8_t font, uint8_t is_hd) {
    if(is_hd) { 
        current_display_info = hd_display_info;
    } else {
        current_display_info = sd_display_info;
    }
}


/**  Load and Convert PNG to BMP format, 4bit per pixel.
 *  Allocates memory, return width and height
 */
unsigned char* loadPngToBMPI4(const char* filename, unsigned int* width, unsigned int* height) {
    unsigned char* pngData;
    unsigned int error = lodepng_decode32_file(&pngData, width, height, filename);

    if (error) {
        printf("Error %u: %s\n", error, lodepng_error_text(error));
        return NULL;
    }

    // Calculate BMP stride (row size aligned to 4 bytes)
    unsigned int bmpStride = (*width * 4 + 3) & ~3;
    bmpStride =  getRowStride(*width,4);

    // Allocate memory for BMP data
    unsigned int bmpSize = bmpStride * (*height);
    unsigned char* bmpData = (unsigned char*)malloc(bmpSize);
    if (!bmpData) {
        printf("Failed to allocate memory for BMP data\n");
        free(pngData);
        return NULL;
    }

    convertRGBAToI4( pngData, *width , *height, bmpData, &g_stPaletteTable);

    // Clean up
    free(pngData);

    return bmpData;
}

int GetMajesticVideoConfig(){

    FILE *file;
    char line[256];
    bool inVideo0Section = false;
    char sizeValue[50];  // Buffer to store the size value
    int width, height;

    // Open the configuration file
#ifdef _x86    
    file = fopen("majestic.yaml", "r");
#else
    file = fopen("/etc/majestic.yaml", "r");
#endif
    if (file == NULL) {
        printf("Could not open majestic.yaml.\n");
        return 0;
    }

    // Read the file line by line
    while (fgets(line, sizeof(line), file)) {
        // Check if we've entered the video0 section
        if (strstr(line, "video0:") != NULL) 
            inVideo0Section = true;

        // If we're in the video0 section, look for the size parameter
        if (inVideo0Section && line[0] != '#') {
            if (strstr(line, "size:") != NULL) {
                // Extract the value after "size:"
                sscanf(line, " size: %49s", sizeValue); // Extract the size value with a buffer limit
                break;  // Exit the loop once the size is found
            }
        }

        // If another section starts, exit the video0 section
        if (line[0] != ' ' && line[0] != '#' && strstr(line, ":") != NULL && strstr(line, "video0:") == NULL) {
            inVideo0Section = false;
        }
    }

    // Close the file
    fclose(file);

    // Print the extracted size value
    if (strlen(sizeValue) > 0) {
        // Use sscanf to parse the size value into width and height
        if (sscanf(sizeValue, "%dx%d", &width, &height) == 2) {
            printf("Width: %d, Height: %d\n", width, height);
        } else {
            printf("Failed to parse size value. %s\n",sizeValue);
            height = 1080;
        }

    } else {
        printf("Size parameter not found in Video0 section.\n");
    }

    return height;

}


int fd_mem;
static void InitMSPHook(){
    memset(character_map, 0, sizeof(character_map));
    
    
    char *font_path;
    char *font_suffix;
    char font_load_name[255];
    font_suffix="";
    font_path = "/usr/bin/";   
     #ifdef _x86
    font_path = "";//.bmp
    #endif 
 
    int height = GetMajesticVideoConfig();
    //height=1080;
    printf("Video Mode %dp\r\n",height);
    if (height<800 && height>400){
        font_suffix = "_hd";
        OVERLAY_WIDTH=1200;
        OVERLAY_HEIGHT=700;
        current_display_info = hd_display_info;
        current_display_info.font_width = 24;
        current_display_info.font_height = 36;
    }else{
        OVERLAY_WIDTH=1800;
        OVERLAY_HEIGHT=1000;
        current_display_info = hd_display_info;
        current_display_info.font_width = 36;
        current_display_info.font_height = 54;
    }

    snprintf(font_load_name, 255, "%sfont%s.png", font_path, font_suffix);
     
    PIXEL_FORMAT_DEFAULT=3;//I4 format, 4 bits per pixel. 
    PIXEL_FORMAT_BitsPerPixel = 4;
    if (bitmapFnt.pData!=NULL)//if called by mistake
        free(bitmapFnt.pData);

    bitmapFnt.pData=(void *)loadPngToBMPI4(font_load_name,&bitmapFnt.u32Width,&bitmapFnt.u32Height);
    bitmapFnt.enPixelFormat =  PIXEL_FORMAT_DEFAULT ;//E_MI_RGN_PIXEL_FORMAT_I8; //I8

    
    //INAV only constants, need to calculate them
    //current_display_info.font_width = bitmapFnt.u32Width/2;
    //current_display_info.font_height = bitmapFnt.u32Height/256;

    
    font_pages = bitmapFnt.u32Width/current_display_info.font_width;

    printf("Font %s character size:%d:%d \r\n",font_load_name,current_display_info.font_width,current_display_info.font_height);
    if (bitmapFnt.u32Width==0 || bitmapFnt.u32Height==0){
        printf("Can't find font file : %s \r\n OSD Disabled! \r\n",font_load_name);
        return;
    }
    int rgn=0;   



     // OBSOLETE, used for BMP font files. Convert icons font to i8 or i4 format
     // This will speed up a little... :)
     if (false){
        //PIXEL_FORMAT_DEFAULT=4;//E_MI_RGN_PIXEL_FORMAT_I8
        //PIXEL_FORMAT_BitsPerPixel=8;
        //TESTTING, not working with SetBitmap?!??!
        PIXEL_FORMAT_DEFAULT=3;//E_MI_RGN_PIXEL_FORMAT_I4
        PIXEL_FORMAT_BitsPerPixel=4;

        uint8_t* destBitmap =  (uint8_t*)malloc(bitmapFnt.u32Height*getRowStride(bitmapFnt.u32Width , PIXEL_FORMAT_BitsPerPixel));  

        if  (PIXEL_FORMAT_DEFAULT==4)
            convertBitmap1555ToI8(bitmapFnt.pData, bitmapFnt.u32Width , bitmapFnt.u32Height, destBitmap, &g_stPaletteTable);        
                   
        if  (PIXEL_FORMAT_DEFAULT==3)
             convertBitmap1555ToI4(bitmapFnt.pData, bitmapFnt.u32Width , bitmapFnt.u32Height, destBitmap, &g_stPaletteTable);      
             
        
        free(bitmapFnt.pData);
        bitmapFnt.pData=(void *)destBitmap;
        bitmapFnt.enPixelFormat =  PIXEL_FORMAT_DEFAULT ;//E_MI_RGN_PIXEL_FORMAT_I8; //I8
     }else{
        /* 
        //Test to load PNG pic
        PIXEL_FORMAT_DEFAULT=3;//1555 PIXEL_FORMAT_1555
        PIXEL_FORMAT_BitsPerPixel = 4;
        bitmapFnt.pData=(void *)loadPngToBmpMemory("font_inav.png",&bitmapFnt.u32Width,&bitmapFnt.u32Height);
        bitmapFnt.enPixelFormat =  PIXEL_FORMAT_DEFAULT ;//E_MI_RGN_PIXEL_FORMAT_I8; //I8
        */
     }

    osds = mmap(NULL, sizeof(*osds) * MAX_OSD,
                PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    for (int id = 0; id < MAX_OSD; id++){
        osds[id].hand = id;
        osds[id].size = DEF_SIZE;
        osds[id].posx = DEF_POSX;
        osds[id].posy = DEF_POSY;
        osds[id].updt = 0;
        strcpy(osds[id].font, DEF_FONT);
        osds[id].text[0] = '\0';
    }  

    #ifdef _x86
        sfVideoMode videoMode = {OVERLAY_WIDTH+20, OVERLAY_HEIGHT+20, 32};
        window = sfRenderWindow_create(videoMode, "MSP OSD", 0, NULL);
        sfRenderWindow_display(window);
    #endif


    #ifdef __SIGMASTAR__            
        int s32Ret = MI_RGN_Init(&g_stPaletteTable);
        printf("MI_RGN_Init results: %d \r\n", s32Ret);
        if (s32Ret)
            fprintf(stderr, "[%s:%d]RGN_Init failed with %#x!\n", __func__, __LINE__, s32Ret);
    #endif

        //THIS IS NEEDED, the main 
        rgn=create_region(&osds[FULL_OVERLAY_ID].hand, osds[FULL_OVERLAY_ID].posx, osds[FULL_OVERLAY_ID].posy, OVERLAY_WIDTH, OVERLAY_HEIGHT);
        printf("Create_region PixelFormat:%d Size: %d:%d results: %d \r\n", PIXEL_FORMAT_DEFAULT, OVERLAY_WIDTH,OVERLAY_HEIGHT, rgn);

        //LOGO TEST, for TEST ONLY, loads a file for several seconds as overlay
        char img[32];//test to show a simple files
        #ifdef __SIGMASTAR__ 
        sprintf(img, "/osd%d.png", FULL_OVERLAY_ID);
        #else
        sprintf(img, "osd%d.png", FULL_OVERLAY_ID);
        #endif

        if (true/*!access(img, F_OK)*/){    
            cntr= - 100;                
            BITMAP bitmap;                                
            int prepared=0;
            if (false){//Load a BITMAP and show it on the screen
                prepared =!(prepare_bitmap(img, &bitmap, 2, TRANSPARENT_COLOR, PIXEL_FORMAT_1555));                                                                                                  
                printf("Loaded LOGO bmp  %d x %d success:%d\n",bitmap.u32Height, bitmap.u32Width,rgn);                 

                // Destination bitmap in I4 format (4 bits per pixel)
                //uint8_t* destBitmap = (uint8_t*)malloc((bitmap.u32Width * bitmap.u32Height) / 2);                                     
                //convertBitmap1555ToI4(bitmap.pData, bitmap.u32Width , bitmap.u32Height, destBitmap, palette);
            
                // Destination bitmap in I8 format (8 bits per pixel)
                uint8_t* destBitmap =  (uint8_t*)malloc((bitmap.u32Width * bitmap.u32Height));     
                convertBitmap1555ToI8(bitmap.pData, bitmap.u32Width , bitmap.u32Height, destBitmap, &g_stPaletteTable);
                free(bitmap.pData);

                bitmap.pData=(void *)destBitmap;//to thenew structure data only
                ///E_MI_RGN_PIXEL_FORMAT_I4 DOEAS NOT WORK!!!
                #ifdef __SIGMASTAR__
                bitmap.enPixelFormat = PIXEL_FORMAT_DEFAULT; // E_MI_RGN_PIXEL_FORMAT_I4; //0
                #endif
                
            }
            //TEST how bitmaps conversions work
            if (false){//LOAD a bitmap and show it on the screen using canvas, works directly into display memory, faster!

                uint8_t* destBitmap =  (uint8_t*)malloc(bitmap.u32Height * getRowStride(bitmap.u32Width , PIXEL_FORMAT_BitsPerPixel));   
                if (PIXEL_FORMAT_DEFAULT==4)  
                    convertBitmap1555ToI8(bitmap.pData, bitmap.u32Width , bitmap.u32Height, destBitmap, &g_stPaletteTable);
                
                if (PIXEL_FORMAT_DEFAULT==3)  
                    convertBitmap1555ToI4(bitmap.pData, bitmap.u32Width , bitmap.u32Height, destBitmap, &g_stPaletteTable);

                free(bitmap.pData);
                
                bitmap.pData=(void *)destBitmap;//to the new structure data only
                bitmap.enPixelFormat = PIXEL_FORMAT_DEFAULT; // E_MI_RGN_PIXEL_FORMAT_I4; //0
                printf("convertBitmap1555ToI%d  success. FULL_OVERLAY_ID.hand= %d\n",(PIXEL_FORMAT_DEFAULT==4)?8:4,osds[FULL_OVERLAY_ID].hand);                 

#ifdef __SIGMASTAR__   
                MI_RGN_CanvasInfo_t stCanvasInfo; 
                memset(&stCanvasInfo,0,sizeof(stCanvasInfo));                     
                //memset(&stCanvasInfo,0,sizeof(stCanvasInfo));
                s32Ret =  GetCanvas(osds[FULL_OVERLAY_ID].hand, &stCanvasInfo);                                       
                printf("stCanvasInfo  CanvasStride: %d , BMP_Stride:%d Size: %d:%d \r\n", 
                    stCanvasInfo.u32Stride, getRowStride(bitmap.u32Width, PIXEL_FORMAT_BitsPerPixel), bitmap.u32Width,bitmap.u32Height);
                int byteWidth =  bitmap.u32Width ; // Each row's width in bytes (I4 = 4 bits per pixel)
                if (PIXEL_FORMAT_DEFAULT==4)
                    byteWidth =  (bitmap.u32Width) * PIXEL_FORMAT_BitsPerPixel / 8;
                if (PIXEL_FORMAT_DEFAULT==3){ //I4
                    //bytesperpixel=0.5F;
                    byteWidth = (uint16_t)(bitmap.u32Width + 1) * PIXEL_FORMAT_BitsPerPixel / 8 ; // Each row's width in bytes (I4 = 4 bits per pixel)                        
                    byteWidth = getRowStride(bitmap.u32Width,PIXEL_FORMAT_BitsPerPixel);
                }

                for (int i = 0; i < bitmap.u32Height; i++)                    
                    memcpy((void *)(stCanvasInfo.virtAddr + i * (stCanvasInfo.u32Stride)), bitmap.pData + i * byteWidth, byteWidth);

                //This tests direct copy to overlay - will further speed up!
                //DrawBitmap1555ToI4(bitmap.pData, bitmap.u32Width , bitmap.u32Height, destBitmap, &g_stPaletteTable, (void *)stCanvasInfo.virtAddr,stCanvasInfo.u32Stride);
                /* this draws a line directly into the OSD memory
                for (int y = 0; y < bitmap.u32Height/2; y++)                           
                    for (int x = 0; x < 5; x+=2)
                        ST_OSD_DrawPoint((void *)stCanvasInfo.virtAddr,stCanvasInfo.u32Stride, x,y, 3 );//y%14+1
                */

                s32Ret = MI_RGN_UpdateCanvas(osds[FULL_OVERLAY_ID].hand);
                printf("MI_RGN_UpdateCanvas completed byteWidth:%d!\n",byteWidth);
                if  (s32Ret!= MI_RGN_OK)    
                    fprintf(stderr, "MI_RGN_UpdateCanvas failed with %#x!\n", __func__, __LINE__, s32Ret);                 
#elif _x86 
                //TEST ON x86
                
                    sfRenderWindow_clear(window, sfColor_fromRGB(255, 255, 0));
                    
                    unsigned char* rgbaData = malloc(bitmap.u32Width * bitmap.u32Height * 4);  // Allocate memory for RGBA data    

                    if (PIXEL_FORMAT_DEFAULT==0)          
                        Convert1555ToRGBA( bitmap.pData, rgbaData, bitmap.u32Width, bitmap.u32Height);    
                    if (PIXEL_FORMAT_DEFAULT==4)          
                        ConvertI8ToRGBA( bitmap.pData, rgbaData, bitmap.u32Width, bitmap.u32Height,&g_stPaletteTable);    
                    if (PIXEL_FORMAT_DEFAULT==3)          
                        ConvertI4ToRGBA( bitmap.pData, rgbaData, bitmap.u32Width, bitmap.u32Height,&g_stPaletteTable);    

                    sfTexture* texture = sfTexture_create(bitmap.u32Width, bitmap.u32Height);
                    if (!texture) 
                        return;
                    sfTexture_updateFromPixels(texture, rgbaData, bitmap.u32Width, bitmap.u32Height, 0, 0);
                    free(rgbaData);  
                    
                    sfSprite* sprite = sfSprite_create();
                    sfSprite_setTexture(sprite, texture, sfTrue);
                    // Set the position where you want to draw the sprite
                    sfVector2f position = {1, 1};
                    sfSprite_setPosition(sprite, position);    
                    sfRenderWindow_drawSprite(window, sprite, NULL);

                    // Cleanup resources
                    sfSprite_destroy(sprite);
                    sfTexture_destroy(texture);
                    printf("Test show bitmap s\r\n");
                    sfRenderWindow_display(window);

#endif
                prepared=false;
            }
            //LOAD PNG TEST
            if (true){//Split and show a review of the selected font for several seconds
                prepared=1;
                
                bitmap.enPixelFormat-PIXEL_FORMAT_DEFAULT;
                int preview_height=current_display_info.font_height * current_display_info.char_height;
                int cols=bitmapFnt.u32Height / preview_height;/*OVERLAY_HEIGH*/;
                int rows=preview_height/    current_display_info.font_height;
                int fontPageHeight=rows * current_display_info.font_height;//OVERLAY_HEIGHT;;
                bitmap.u32Height = preview_height;//rows * current_display_info.font_height;//OVERLAY_HEIGHT;
                bitmap.u32Width = OVERLAY_WIDTH;//bitmapFnt.u32Width * cols;                    
                bitmap.pData = (unsigned char*)malloc(PIXEL_FORMAT_BitsPerPixel * bitmap.u32Height * getRowStride(bitmap.u32Width , PIXEL_FORMAT_BitsPerPixel));
                memset(bitmap.pData, 0, PIXEL_FORMAT_BitsPerPixel * bitmap.u32Height * getRowStride(bitmap.u32Width , PIXEL_FORMAT_BitsPerPixel));
                //bmp.u32Width=current_display_info.font_width * current_display_info.char_width;
                //bmp.u32Height = current_display_info.font_height * current_display_info.char_height;
                //bmp.pData = malloc( PIXEL_FORMAT_BitsPerPixel  * bmp.u32Height * getRowStride(bmp.u32Width , PIXEL_FORMAT_BitsPerPixel));

                for (int i=0; i<cols ; i++ )
                    copyRectI4(bitmapFnt.pData,bitmapFnt.u32Width,bitmapFnt.u32Height,
                                bitmap.pData,bitmap.u32Width, bitmap.u32Height,
                                0,i * fontPageHeight, bitmapFnt.u32Width, fontPageHeight,
                                i*bitmapFnt.u32Width,0);                
            
                /* Test load BMP image and show it on the screen 
                bitmap.pData = loadPngToBMPI4(img, &bitmap.u32Width, &bitmap.u32Height);
                bitmap.enPixelFormat-PIXEL_FORMAT_DEFAULT;
                */
                
                #ifdef __SIGMASTAR__   
                    printf("Set Font Review %d:%d", bitmap.u32Width, bitmap.u32Height);
                    //For some reason this fails...?!
                    //set_bitmap(osds[FULL_OVERLAY_ID].hand, &bitmap);//bitmap must match region dimensions!
                    
                    set_bitmapEx(osds[FULL_OVERLAY_ID].hand, &bitmap, PIXEL_FORMAT_BitsPerPixel);

                #else
                    sfRenderWindow_clear(window, sfColor_fromRGB(255, 255, 0));
                    
                    unsigned char* rgbaData = malloc(bitmap.u32Width * bitmap.u32Height * 4);  // Allocate memory for RGBA data          
                    ConvertI4ToRGBA( bitmap.pData, rgbaData, bitmap.u32Width, bitmap.u32Height,&g_stPaletteTable);    

                    //test simple load PNG file and show it
                    //free(rgbaData);
                    //int width,height;
                    //unsigned int error = lodepng_decode32_file(&rgbaData, &bitmap.u32Width, &bitmap.u32Height, font_load_name);                    

                    sfTexture* texture = sfTexture_create(bitmap.u32Width, bitmap.u32Height);
                    if (!texture) 
                        return;
                    sfTexture_updateFromPixels(texture, rgbaData, bitmap.u32Width, bitmap.u32Height, 0, 0);
                    free(rgbaData);  
                    
                    sfSprite* sprite = sfSprite_create();
                    sfSprite_setTexture(sprite, texture, sfTrue);
                    // Set the position where you want to draw the sprite
                    sfVector2f position = {1, 1};
                    sfSprite_setPosition(sprite, position);    
                    sfRenderWindow_drawSprite(window, sprite, NULL);

                    // Cleanup resources
                    sfSprite_destroy(sprite);
                    sfTexture_destroy(texture);
                    printf("Test show bitmap s\r\n");
                    sfRenderWindow_display(window);
            #endif
                //free(bitmap.pData);
            }

            if (prepared){
                printf("set_LOGO with u32Height:%d enPixelFormat %d\n",bitmap.u32Height, bitmap.enPixelFormat);                   
                int s32Ret=set_bitmap(osds[FULL_OVERLAY_ID].hand, &bitmap);
                if(s32Ret!=0)
                    printf("ERROR set_bitmap%d \n",s32Ret);  

                free(bitmap.pData);
            }

        }else
                printf("No logo file %s \n",img);
    

     if (enable_fast_layout){ //fast_overlay , can be smaller     
        fcX= 18; fcW=16;
        fcY= 6 ; fcH=8;  
        int fY=54;
        int fX=36;

        printf("%d\r",osds[FULL_OVERLAY_ID].posx);      
        osds[FAST_OVERLAY_ID].width=fcW * fX; //  OVERLAY_WIDTH/3; 
        osds[FAST_OVERLAY_ID].height = fcH*fY;
        
        osds[FAST_OVERLAY_ID].posx=  fcX*fX;  //( OVERLAY_WIDTH-osds[FAST_OVERLAY_ID].width)/2;
        osds[FAST_OVERLAY_ID].posy=  fcY*fY; //( OVERLAY_HEIGHT-osds[FAST_OVERLAY_ID].height)/2;

        rgn=create_region(&osds[FAST_OVERLAY_ID].hand, osds[FAST_OVERLAY_ID].posx, osds[FAST_OVERLAY_ID].posy, osds[FAST_OVERLAY_ID].width, osds[FAST_OVERLAY_ID].height);
    }
    
    display_driver = calloc(1, sizeof(displayport_vtable_t));
    display_driver->draw_character = &draw_character;
    display_driver->clear_screen = &clear_screen;
    display_driver->draw_complete = &draw_complete;
    display_driver->set_options = &set_options;

    msp_state_t *msp_state = calloc(1, sizeof(msp_state_t));
    msp_state->cb = &msp_callback;
 

}

static void CloseMSP(){
     munmap(osds, sizeof(*osds) * MAX_OSD);
    #ifdef __SIGMASTAR__
    int s32Ret = MI_RGN_DeInit();
    if (s32Ret)
        printf("[%s:%d]RGN_DeInit failed with %#x!\n", __func__, __LINE__, s32Ret);        
    #endif
    if (bitmapFnt.pData!=NULL)
        free(bitmapFnt.pData);

}
