#ifndef COMMON_H_
#define COMMON_H_

#define __SIGMASTAR__

#include <stdio.h>  // FILE, fseek, (f|p)open, (as|f)printf
#include <stdlib.h> // abort, atoi, exit, free, malloc
#include <string.h> // memcpy, memset, strcmp, strlen
#include <unistd.h> // getopt, optarg, opterr
#include <ctype.h>  // isdigit
#include <time.h>   // clock_(get|set)time, timespec
#include <math.h>   // ceil

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/if_link.h>
#include <netdb.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
#if __cplusplus
extern "C"
{
#endif
#endif

#include "config.h"

 
 typedef enum
{
    E_MI_RGN_PIXEL_FORMAT_ARGB1555 = 0,
    E_MI_RGN_PIXEL_FORMAT_ARGB4444,
    E_MI_RGN_PIXEL_FORMAT_I2,
    E_MI_RGN_PIXEL_FORMAT_I4,
    E_MI_RGN_PIXEL_FORMAT_I8,
    E_MI_RGN_PIXEL_FORMAT_RGB565,
    E_MI_RGN_PIXEL_FORMAT_ARGB8888,
    E_MI_RGN_PIXEL_FORMAT_MAX
} MI_RGN_PixelFormat_e;

#define IO_BASE 0x1F000000
#define IO_SIZE 0x400000
#define PIXEL_FORMAT_4444 E_MI_RGN_PIXEL_FORMAT_ARGB4444
#define PIXEL_FORMAT_1555 E_MI_RGN_PIXEL_FORMAT_ARGB1555
#define PIXEL_FORMAT_2BPP E_MI_RGN_PIXEL_FORMAT_I2
#define PIXEL_FORMAT_8888 E_MI_RGN_PIXEL_FORMAT_ARGB8888
 

#ifndef DIV_UP
#define DIV_UP(x, a) (((x) + ((a)-1)) / a)
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define starts_with(a, b) !strncmp(a, b, strlen(b))
#define equals(a, b) !strcmp(a, b)
#define ends_with(a, b)      \
    size_t alen = strlen(a); \
    size_t blen = strlen(b); \
    return (alen > blen) && strcmp(a + alen - blen, b);
#define empty(x) (x[0] == '\0')

    typedef struct osd
    {
        double size;
        int hand;
        short posx, posy;
        char updt;
        char font[32];
        char text[80];
    } OSD;

    typedef struct bitmap
    {
        int enPixelFormat;
        unsigned int u32Width;
        unsigned int u32Height;
        void *pData;
    } BITMAP;

    typedef struct rect
    {
        short width, height;
    } RECT;

    

    static void fatal(const char *message)
    {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }

#ifdef SUPP_UTF32
    static int utf8_to_utf32(const uint8_t *utf8, uint32_t *utf32, int max)
    {
        unsigned int c;
        int i = 0;
        --max;
        while (*utf8)
        {
            if (i >= max)
                return 0;
            if (!(*utf8 & 0x80U))
            {
                utf32[i++] = *utf8++;
            }
            else if ((*utf8 & 0xe0U) == 0xc0U)
            {
                c = (*utf8++ & 0x1fU) << 6;
                if ((*utf8 & 0xc0U) != 0x80U)
                    return 0;
                utf32[i++] = c + (*utf8++ & 0x3fU);
            }
            else if ((*utf8 & 0xf0U) == 0xe0U)
            {
                c = (*utf8++ & 0x0fU) << 12;
                if ((*utf8 & 0xc0U) != 0x80U)
                    return 0;
                c += (*utf8++ & 0x3fU) << 6;
                if ((*utf8 & 0xc0U) != 0x80U)
                    return 0;
                utf32[i++] = c + (*utf8++ & 0x3fU);
            }
            else if ((*utf8 & 0xf8U) == 0xf0U)
            {
                c = (*utf8++ & 0x07U) << 18;
                if ((*utf8 & 0xc0U) != 0x80U)
                    return 0;
                c += (*utf8++ & 0x3fU) << 12;
                if ((*utf8 & 0xc0U) != 0x80U)
                    return 0;
                c += (*utf8++ & 0x3fU) << 6;
                if ((*utf8 & 0xc0U) != 0x80U)
                    return 0;
                c += (*utf8++ & 0x3fU);
                if ((c & 0xFFFFF800U) == 0xD800U)
                    return 0;
                utf32[i++] = c;
            }
            else
                return 0;
        }
        utf32[i] = 0;
        return i;
    }
#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif