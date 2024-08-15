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
    #include "bmp/region.h"
    #include "bmp/common.h"    
#endif
#include "bmp/region.h"
#include "bmp/bitmap.h"


#define X_OFFSET 0

#define PORT 7654

#define CLOCK_MONOTONIC 1

/*----------------------------------------------------------------------------------------------------*/
/*------------CONFIGURE SWITCHES ----------------------------------------------------------------*/
static int enable_fast_layout = 0;
static int Use_Fast_Font = 3;

void *io_map;
struct osd *osds;
char timefmt[32] = DEF_TIMEFMT;
float PIXEL_FORMAT_BytesPerPixel = 1.0F;


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

#define OVERLAY_WIDTH 1800
#define OVERLAY_HEIGHT 1000

#define TRANSPARENT_COLOR 0xFBDE

#define FULL_OVERLAY_ID 1
#define FAST_OVERLAY_ID 2

char font_2_name[256];

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


static int load_font(const char *filename, BITMAP *bitmap){
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
//static int center_x=3,center_y=0, center_width=0,center_height=0;


static unsigned long long LastCleared=0;
static unsigned long long LastDrawn=0;

static void draw_screenCenter(){

    cntr++;
    int s32BytesPerPix=2;//ARGB1555, 16bit

	if ( (get_time_ms() - LastDrawn  ) < MinTimeBetweenScreenRefresh){//Once a second max{        
		return ;
    }

    if ( (get_time_ms() - LastCleared  ) < 150){//at least 200ms to reload data
        printf("%lu DrawCenter skipped after clear LastDrawn:%lu | %lu%\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)LastDrawn%10000 , (uint32_t) LastCleared%10000);
		return ;
    }

    BITMAP bitmap;
    LastDrawn = get_time_ms();
    bitmap.u32Height= osds[FAST_OVERLAY_ID].height;
    bitmap.u32Width= osds[FAST_OVERLAY_ID].width;
    bitmap.pData = malloc(s32BytesPerPix * bitmap.u32Height * bitmap.u32Width);
    bitmap.enPixelFormat = PIXEL_FORMAT_1555;
    
    current_display_info.font_width = bitmapFnt.u32Width/2;
    current_display_info.font_height = bitmapFnt.u32Height/256;


    for (int y = fcY; y < current_display_info.char_height-fcH; y++)
    {
        for (int x = fcX; x < current_display_info.char_width-fcW; x++)
        {
            uint16_t c = character_map[x][y];
            if (c != 0)
            {
                uint8_t page = 0;
                if (c > 255) {
                    page = 1;
                    c = c & 0xFF;
                }
               // DEBUG_PRINT("%02X", c);
                //Where is the rectangle in the Font Bitmap
                u_int16_t s_left = page*current_display_info.font_width;
                u_int16_t s_top = current_display_info.font_height * c;
                u_int16_t s_width = current_display_info.font_width;
                u_int16_t s_height = current_display_info.font_height;                                
                //the location in the screen bmp where we will place the character glyph
                u_int16_t d_x=(x-fcX) * current_display_info.font_width + X_OFFSET;
                u_int16_t d_y=(y-fcY) * current_display_info.font_height;
                
                copyRectARGB1555(bitmapFnt.pData,bitmapFnt.u32Width,bitmapFnt.u32Height,
                                bitmap.pData,bitmap.u32Width, bitmap.u32Height,
                                s_left,s_top,s_width,s_height,
                                d_x,d_y);
               
            }else{
            }
           // DEBUG_PRINT("  ");
        }
       // DEBUG_PRINT("\n");
    }

#ifdef _x86
    uint64_t step2=get_time_ms();  
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

    
    printf("%lu set_bitmapC for:%u | %u  ms\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)(get_time_ms() - LastDrawn) , (uint32_t)(get_time_ms() - step2));

#else
    int id=0;
    uint64_t step2=get_time_ms();  
    set_bitmap(osds[FAST_OVERLAY_ID].hand, &bitmap);
    printf("%lu set_bitmapC for:%u | %u  ms\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)(get_time_ms() - LastDrawn) , (uint32_t)(get_time_ms() - step2));


#endif


    free(bitmap.pData);
}

 BITMAP bmpBuff;

static void draw_screenBMP(){
    
    if (cntr++<0 )
        return ;
	if ( (get_time_ms() - LastDrawn  ) < MinTimeBetweenScreenRefresh){//Once a second max{
        // printf("%lu DrawSkipped LastDrawn:%lu\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)LastDrawn);
		return ;
    }

    if ( (get_time_ms() - LastCleared  ) < 200){//at least 200ms to reload data
        printf("%lu DrawSkipped after clear LastDrawn:%lu | %lu%\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)LastDrawn%10000 , (uint32_t) LastCleared%10000);
		return ;
    }

    LastDrawn= get_time_ms();

    float s32BytesPerPix=PIXEL_FORMAT_BytesPerPixel;//ARGB1555, 16bit

    if (bmpBuff.pData==NULL)//????
    bmpBuff.u32Height=OVERLAY_HEIGHT;
    bmpBuff.u32Width=OVERLAY_WIDTH;
    if (bmpBuff.pData==NULL)
        bmpBuff.pData = malloc(s32BytesPerPix * bmpBuff.u32Height * bmpBuff.u32Width);
    bmpBuff.enPixelFormat =  PIXEL_FORMAT_DEFAULT;//  PIXEL_FORMAT_DEFAULT ;//PIXEL_FORMAT_1555;

    
    current_display_info.font_width = bitmapFnt.u32Width/2;
    current_display_info.font_height = bitmapFnt.u32Height/256;
 

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
               // DEBUG_PRINT("%02X", c);
                //Where is the rectangle in the Font Bitmap
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

#ifdef _x86
    
    uint64_t step2=get_time_ms();  
    sfRenderWindow_clear(window, sfColor_fromRGB(55, 55, 55));
    unsigned char* rgbaData = malloc(bmpBuff.u32Width * bmpBuff.u32Height * 4);  // Allocate memory for RGBA data    

    if (PIXEL_FORMAT_DEFAULT==0)          
        Convert1555ToRGBA( bmpBuff.pData, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height);    
    else
        ConvertI8ToRGBA( bmpBuff.pData, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height,&g_stPaletteTable);    
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
    printf("%lu set_bitmapC for:%u | %u  ms\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)(get_time_ms() - LastDrawn) , (uint32_t)(get_time_ms() - step2));

#else
   int id=0;
   // printf("%lu set_bitmapB for:%d | %d ms\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)(get_time_ms() - LastDrawn));
    uint64_t step2=get_time_ms();
    set_bitmap(osds[FULL_OVERLAY_ID].hand, &bmpBuff);
    printf("%lu set_bitmapB for:%u | %u  ms\r\n",(uint32_t)get_time_ms()%10000, (uint32_t)(get_time_ms() - LastDrawn) , (uint32_t)(get_time_ms() - step2));

#endif


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
    
    if (get_time_ms() - LastCleared>1500) {//no faster than 1 per second
        memset(character_map, 0, sizeof(character_map));
        if (bmpBuff.pData!=NULL)//Set whole BMP as transparant
            bmpBuff.pData = memset(bmpBuff.pData, 0xFF , (PIXEL_FORMAT_BytesPerPixel * bmpBuff.u32Height * bmpBuff.u32Width) );

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
            draw_screenCenter();
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

//------------------------------------------------------------------
//-----------------------bitmap conversion --------------
//------------------------------------------------------------------
#define PALETTE_SIZE 16
// Function to calculate the squared difference between two colors
uint32_t colorDistance(uint16_t color1, uint16_t color2) {
    int r1 = (color1 >> 10) & 0x1F;
    int g1 = (color1 >> 5) & 0x1F;
    int b1 = color1 & 0x1F;

    int r2 = (color2 >> 10) & 0x1F;
    int g2 = (color2 >> 5) & 0x1F;
    int b2 = color2 & 0x1F;

    return (r1 - r2) * (r1 - r2) + (g1 - g2) * (g1 - g2) + (b1 - b2) * (b1 - b2);
}

// Function to find the closest color in the palette
 



int fd_mem;
static void InitMSPHook(){
    memset(character_map, 0, sizeof(character_map));
    
    char *font_name;   
    font_name = "font_inav";    
    char font_load_name[255];

    snprintf(font_load_name, 255, "%s.bmp", font_name);
 

    font_name = "/usr/bin/font_inav.bmp";
    #ifdef _x86
    font_name = "font_inav.bmp";
    #endif
    if (load_font(font_name,&bitmapFnt)<0)// must clean up after free(bitmap.pData);
        printf("Can't load font %s \r\n",font_name);
    
    if (bitmapFnt.u32Width==0)
        printf("Error in font %s \r\n !!!",font_name);
    current_display_info.font_width = bitmapFnt.u32Width/2;
    current_display_info.font_height = bitmapFnt.u32Height/256;
    printf("Font %s character size:%d:%d \r\n",font_name,current_display_info.font_width,current_display_info.font_height);
    int rgn=0;   


     // Convert icons font to i8 format
     // This will speed up a little... :)
     if (Use_Fast_Font){
        //PIXEL_FORMAT_DEFAULT=4;//E_MI_RGN_PIXEL_FORMAT_I8
        //PIXEL_FORMAT_BytesPerPixel=1;
        //TESTTING, not working with SetBitmap?!??!
        PIXEL_FORMAT_DEFAULT=3;//E_MI_RGN_PIXEL_FORMAT_I4
        PIXEL_FORMAT_BytesPerPixel=0.52;

        uint8_t* destBitmap =  (uint8_t*)malloc(((bitmapFnt.u32Width + 1) * bitmapFnt.u32Height)*PIXEL_FORMAT_BytesPerPixel);  

        if  (PIXEL_FORMAT_DEFAULT==4)
            convertBitmap1555ToI8(bitmapFnt.pData, bitmapFnt.u32Width , bitmapFnt.u32Height, destBitmap, &g_stPaletteTable);        
                   
        if  (PIXEL_FORMAT_DEFAULT==3)
             convertBitmap1555ToI4(bitmapFnt.pData, bitmapFnt.u32Width , bitmapFnt.u32Height, destBitmap, &g_stPaletteTable);      
             
        
        free(bitmapFnt.pData);
        bitmapFnt.pData=(void *)destBitmap;
        bitmapFnt.enPixelFormat =  PIXEL_FORMAT_DEFAULT ;//E_MI_RGN_PIXEL_FORMAT_I8; //I8
     }else{
        PIXEL_FORMAT_DEFAULT=0;//1555 PIXEL_FORMAT_1555
        PIXEL_FORMAT_BytesPerPixel = 2;
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
            printf("Create_region PixelFormat:%d results: %d \r\n", PIXEL_FORMAT_DEFAULT, rgn);
    
            //LOGO TEST, for TEST ONLY, loads a file for several seconds as overlay
            char img[32];//test to show a simple files
            #ifdef __SIGMASTAR__ 
            sprintf(img, "/osd%d.bmp", FULL_OVERLAY_ID);
            #else
            sprintf(img, "osd%d.bmp", FULL_OVERLAY_ID);
            #endif
            if (!access(img, F_OK)){    
                cntr= - 200;                
                BITMAP bitmap;                                
                int prepared =!(prepare_bitmap(img, &bitmap, 2, TRANSPARENT_COLOR, PIXEL_FORMAT_1555));                                                                        
                //rgn=create_region(&osds[FULL_OVERLAY_ID].hand, osds[FULL_OVERLAY_ID].posx, osds[FULL_OVERLAY_ID].posy, bitmap.u32Width, bitmap.u32Height);
                printf("Loaded LOGO bmp  %d x %d success:%d\n",bitmap.u32Height, bitmap.u32Width,rgn);                 

                if (false){//Load a BITMAP and show it on the screen

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
                if (true){//LOAD a bitmap and show it on the screen using canvas, works directly into display memory, faster!
                    
                    //uint8_t buff[10000];
                  
                    float bytesperpixel=PIXEL_FORMAT_BytesPerPixel;                                                        

                    uint8_t* destBitmap =  (uint8_t*)malloc((bitmap.u32Width * bitmap.u32Height)*bytesperpixel);   
                    if (PIXEL_FORMAT_DEFAULT==4)  
                       convertBitmap1555ToI8(bitmap.pData, bitmap.u32Width , bitmap.u32Height, destBitmap, &g_stPaletteTable);
                    
                    if (PIXEL_FORMAT_DEFAULT==3)  
                        convertBitmap1555ToI4(bitmap.pData, bitmap.u32Width , bitmap.u32Height, destBitmap, &g_stPaletteTable);

                    free(bitmap.pData);
                    
                    bitmap.pData=(void *)destBitmap;//to thenew structure data only
                    bitmap.enPixelFormat = PIXEL_FORMAT_DEFAULT; // E_MI_RGN_PIXEL_FORMAT_I4; //0
                    printf("convertBitmap1555ToI%d  success. FULL_OVERLAY_ID.hand= %d\n",(PIXEL_FORMAT_DEFAULT==4)?8:4,osds[FULL_OVERLAY_ID].hand);                 

#ifdef __SIGMASTAR__   
                    MI_RGN_CanvasInfo_t stCanvasInfo; 
                    memset(&stCanvasInfo,0,sizeof(stCanvasInfo));                     
                    //memset(&stCanvasInfo,0,sizeof(stCanvasInfo));
                    s32Ret =  GetCanvas(osds[FULL_OVERLAY_ID].hand, &stCanvasInfo);                    
                    printf("stCanvasInfo  u32Stride: %d  Size: %d:%d \r\n", stCanvasInfo.u32Stride,bitmap.u32Width,bitmap.u32Height);
                    int byteWidth =  bitmap.u32Width ; // Each row's width in bytes (I4 = 4 bits per pixel)
                    if (PIXEL_FORMAT_DEFAULT==4)
                        byteWidth =  (bitmap.u32Width) * bytesperpixel;
                    if (PIXEL_FORMAT_DEFAULT==3){ //I4
                        bytesperpixel=0.5F;
                        byteWidth = (uint16_t)(bitmap.u32Width + 1) * bytesperpixel ; // Each row's width in bytes (I4 = 4 bits per pixel)                        
                    }

                    for (int i = 0; i < bitmap.u32Height; i++)                    
                        memcpy((void *)(stCanvasInfo.virtAddr + i * (stCanvasInfo.u32Stride)), bitmap.pData + i * byteWidth, byteWidth);

                    //This tests direct copy to overlay - will further speed up!
                    //DrawBitmap1555ToI4(bitmap.pData, bitmap.u32Width , bitmap.u32Height, destBitmap, &g_stPaletteTable, (void *)stCanvasInfo.virtAddr,stCanvasInfo.u32Stride);
                    /*
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
                        bmpBuff=bitmap;
                        unsigned char* rgbaData = malloc(bmpBuff.u32Width * bmpBuff.u32Height * 4);  // Allocate memory for RGBA data    

                        if (PIXEL_FORMAT_DEFAULT==0)          
                            Convert1555ToRGBA( bmpBuff.pData, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height);    
                        if (PIXEL_FORMAT_DEFAULT==4)          
                            ConvertI8ToRGBA( bmpBuff.pData, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height,&g_stPaletteTable);    
                        if (PIXEL_FORMAT_DEFAULT==3)          
                            ConvertI4ToRGBA( bmpBuff.pData, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height,&g_stPaletteTable);    

                        sfTexture* texture = sfTexture_create(bmpBuff.u32Width, bmpBuff.u32Height);
                        if (!texture) 
                            return;
                        sfTexture_updateFromPixels(texture, rgbaData, bmpBuff.u32Width, bmpBuff.u32Height, 0, 0);
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

    printf("\r\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n",font_name);

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
