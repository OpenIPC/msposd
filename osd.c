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

#include "osd/msp/msp.h"
#include "osd/msp/msp_displayport.h"

#ifdef _x86
    #include <SFML/Graphics.h>
    #include <SFML/Window.h>
 #else
    #include "bmp/region.h"f
    #include "bmp/common.h"    
#endif
#include "bmp/region.h"
#include "bmp/bitmap.h"



#define X_OFFSET 120

#define PORT 7654

#define WIDTH 1440
#define HEIGHT 810

#define CLOCK_MONOTONIC 1


void *io_map;
struct osd *osds;
char timefmt[32] = DEF_TIMEFMT;


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

#define MAX_OSD_WIDTH 50
#define MAX_OSD_HEIGHT 18

#define OVERLAY_WIDTH 1280
#define OVERLAY_HEIGHT 800

#define FULL_OVERLAY_ID 1

char font_2_name[256];

static displayport_vtable_t *display_driver;

static display_info_t current_display_info = SD_DISPLAY_INFO;

#ifdef _x86
    sfTexture *font_1;
    sfTexture *font_2;
    sfSprite *font_sprite_1;
    sfSprite *font_sprite_2;
    sfRenderWindow *window;
#endif


static int load_font(const char *filename, BITMAP *bitmap){
    if (access(filename, F_OK))
        return -1;        
    prepare_bitmap(filename, bitmap, 1, 0x8000, PIXEL_FORMAT_1555);                                    
    return 0;    
}

void Convert1555ToRGBA(unsigned short* bitmap1555, unsigned char* rgbaData, unsigned int width, unsigned int height) {
    for (unsigned int i = 0; i < width * height; ++i) {
        unsigned short pixel1555 = bitmap1555[i];

        // Extract components
        unsigned char alpha = (pixel1555 & 0x8000) ? 255 : 0;  // 1-bit alpha, 0x8000 is the alpha bit mask
        unsigned char red = (pixel1555 & 0x7C00) >> 10;        // 5-bit red, shift right to align
        unsigned char green = (pixel1555 & 0x03E0) >> 5;       // 5-bit green
        unsigned char blue = (pixel1555 & 0x001F);             // 5-bit blue

        // Scale 5-bit colors to 8-bit
        red = (red << 3) | (red >> 2);     // Scale from 5-bit to 8-bit
        green = (green << 3) | (green >> 2); // Scale from 5-bit to 8-bit
        blue = (blue << 3) | (blue >> 2);   // Scale from 5-bit to 8-bit

        // Combine into RGBA
        rgbaData[i * 4 + 0] = red;
        rgbaData[i * 4 + 1] = green;
        rgbaData[i * 4 + 2] = blue;
        rgbaData[i * 4 + 3] = alpha;
    }
}


void copyRectARGB1555(
    uint16_t* srcBitmap, uint32_t srcWidth, uint32_t srcHeight,
    uint16_t* destBitmap, uint32_t destWidth, uint32_t destHeight,
    uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height,
    uint32_t destX, uint32_t destY)
{
    // Bounds checking
    if (srcX + width > srcWidth || srcY + height > srcHeight ||
        destX + width > destWidth || destY + height > destHeight){
        // Handle error: the rectangle is out of bounds
        return;
    }

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            // Calculate the source and destination indices
            uint32_t srcIndex = (srcY + y) * srcWidth + (srcX + x);
            uint32_t destIndex = (destY + y) * destWidth + (destX + x);

            // Copy the pixel
            destBitmap[destIndex] = srcBitmap[srcIndex];
        }
    }
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

BITMAP bitmapFnt;
uint16_t character_map[MAX_OSD_WIDTH][MAX_OSD_HEIGHT];

struct osd *osds;//regions over the overlay

int cnt=0;

static void draw_screenBMP(){
    
    int s32BytesPerPix=2;//ARGB1555, 16bit

    BITMAP bitmap;

    bitmap.u32Height=OVERLAY_HEIGHT;
    bitmap.u32Width=OVERLAY_WIDTH;
    bitmap.pData = malloc(s32BytesPerPix * bitmap.u32Height * bitmap.u32Width);
    bitmap.enPixelFormat = PIXEL_FORMAT_1555;

    memset( bitmap.pData, 0x7111 + (cnt%0x900) ,bitmap.u32Width * bitmap.u32Height*2);
    current_display_info.font_width = bitmapFnt.u32Width/2;
    current_display_info.font_height = bitmapFnt.u32Height/256;
    cnt++;
    if (cnt>current_display_info.char_height*current_display_info.char_width)
        cnt=0;

    for (int y = 0; y < current_display_info.char_height; y++)
    {
        for (int x = 0; x < current_display_info.char_width; x++)
        {
            uint16_t c = character_map[x][y];
            if (c != 0)
            {
                uint8_t page = 0;
                if (c > 255) {
                    page = 1;
                    c = c & 0xFF;
                }
                DEBUG_PRINT("%02X", c);
                //Where is the rectangle in the Font Bitmap
                u_int16_t s_left = page*current_display_info.font_width;
                u_int16_t s_top = current_display_info.font_height * c;
                u_int16_t s_width = current_display_info.font_width;
                u_int16_t s_height = current_display_info.font_height;                                
                //the location in the screen bmp where we will place the character glyph
                u_int16_t d_x=x * current_display_info.font_width + X_OFFSET;
                u_int16_t d_y=y * current_display_info.font_height;
                
                copyRectARGB1555(bitmapFnt.pData,bitmapFnt.u32Width,bitmapFnt.u32Height,
                                bitmap.pData,bitmap.u32Width, bitmap.u32Height,
                                s_left,s_top,s_width,s_height,
                                d_x,d_y);

               
            }else{
                if (x*y==cnt){
                    memset( bitmap.pData + (bitmap.u32Width*y*2) + x*current_display_info.font_width , 
                    0x7777 ,current_display_info.font_width *2);
                }
            }
            DEBUG_PRINT("  ");
        }
        DEBUG_PRINT("\n");
    }

#ifdef _x86
    sfRenderWindow_clear(window, sfColor_fromRGB(55, 55, 55));
    unsigned char* rgbaData = malloc(bitmap.u32Width * bitmap.u32Height * 4);  // Allocate memory for RGBA data    
    Convert1555ToRGBA( bitmap.pData, rgbaData, bitmap.u32Width, bitmap.u32Height);    
    sfTexture* texture = sfTexture_create(bitmap.u32Width, bitmap.u32Height);
    if (!texture) 
        return;
    sfTexture_updateFromPixels(texture, rgbaData, bitmap.u32Width, bitmap.u32Height, 0, 0);
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
#else
   int id=0;

    set_bitmap(osds[FULL_OVERLAY_ID].hand, &bitmap);

#endif


    free(bitmap.pData);
}

// ---------------------------------------------------
// ---- Functions to process MSP messages 
//  ---------------------------------------------------

static void draw_character(uint32_t x, uint32_t y, uint16_t c)
{
    draw_character_on_console(y,x,c);
    if (x > current_display_info.char_width - 1 || y > current_display_info.char_height - 1)
    {
        return;
    }
    character_map[x][y] = c;
}

static void clear_screen()
{
    DEBUG_PRINT("clear\n");
    memset(character_map, 0, sizeof(character_map));
}

static void draw_complete()
{
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

int fd_mem;
static void InitMSPHook(){
    memset(character_map, 0, sizeof(character_map));
    
    char *font_name;   
    font_name = "font_inav.png";    
    char font_load_name[255];

    snprintf(font_load_name, 255, "%s.png", font_name);
 
    font_name = "font_inav.bmp";
    if (load_font(font_name,&bitmapFnt)<-1)// must clean up after free(bitmap.pData);
        printf("Can't load font %s",font_name);
    

    current_display_info.font_width = bitmapFnt.u32Width/2;
    current_display_info.font_height = bitmapFnt.u32Height/256;
    printf("Font %s character size:%d:%d",font_name,current_display_info.font_width,current_display_info.font_height);


    #ifdef _x86
        sfVideoMode videoMode = {1440, 810, 32};
        window = sfRenderWindow_create(videoMode, "MSP OSD", 0, NULL);
        sfRenderWindow_display(window);
    #else
            //register overlays
        fd_mem = open("/dev/mem", O_RDWR);
        io_map = mmap(NULL, IO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_mem, IO_BASE);

        #ifdef __SIGMASTAR__
            static MI_RGN_PaletteTable_t g_stPaletteTable = {{{0, 0, 0, 0}}};
            int s32Ret = MI_RGN_Init(&g_stPaletteTable);
            if (s32Ret)
                fprintf(stderr, "[%s:%d]RGN_Init failed with %#x!\n", __func__, __LINE__, s32Ret);
        #endif

            osds = mmap(NULL, sizeof(*osds) * MAX_OSD,
                PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
            for (int id = 0; id < MAX_OSD; id++)
            {
                osds[id].hand = id;
                osds[id].size = DEF_SIZE;
                osds[id].posx = DEF_POSX;
                osds[id].posy = DEF_POSY;
                osds[id].updt = 0;
                strcpy(osds[id].font, DEF_FONT);
                osds[id].text[0] = '\0';
            }  
            
            create_region(&osds[FULL_OVERLAY_ID].hand, osds[FULL_OVERLAY_ID].posx, osds[FULL_OVERLAY_ID].posy, OVERLAY_WIDTH, OVERLAY_HEIGHT);
    #endif
    
    display_driver = calloc(1, sizeof(displayport_vtable_t));
    display_driver->draw_character = &draw_character;
    display_driver->clear_screen = &clear_screen;
    display_driver->draw_complete = &draw_complete;
    display_driver->set_options = &set_options;

    msp_state_t *msp_state = calloc(1, sizeof(msp_state_t));
    msp_state->cb = &msp_callback;
    printf("\r\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n",font_name);

}

static void CloseMSP(){
     munmap(osds, sizeof(*osds) * MAX_OSD);
    #ifdef __SIGMASTAR__
    int s32Ret = MI_RGN_DeInit();
    if (s32Ret)
        printf("[%s:%d]RGN_DeInit failed with %#x!\n", __func__, __LINE__, s32Ret);
    #endif
    munmap(io_map, IO_SIZE);
    close(fd_mem);

}
