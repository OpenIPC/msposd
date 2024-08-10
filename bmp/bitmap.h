#ifndef BITMAP_H_
#define BITMAP_H_

#ifdef __cplusplus
#if __cplusplus
extern "C"
{
#endif
#endif

#include "common.h"

    typedef enum osd_color_fmt
    {
        OSD_COLOR_FMT_RGB444 = 0,
        OSD_COLOR_FMT_RGB4444 = 1,
        OSD_COLOR_FMT_RGB555 = 2,
        OSD_COLOR_FMT_RGB565 = 3,
        OSD_COLOR_FMT_RGB1555 = 4,
        OSD_COLOR_FMT_RGB888 = 6,
        OSD_COLOR_FMT_RGB8888 = 7,
        OSD_COLOR_FMT_BUTT
    } OSD_COLOR_FMT_E;

    typedef struct osd_rgb
    {
        unsigned char u8B;
        unsigned char u8G;
        unsigned char u8R;
        unsigned char u8Reserved;
    } OSD_RGB_S;

    typedef struct osd_surface
    {
        OSD_COLOR_FMT_E enColorFmt; /* color format */
        unsigned char *pu8PhyAddr;  /* physical address */
        unsigned short u16Height;   /* operation height */
        unsigned short u16Width;    /* operation width */
        unsigned short u16Stride;   /* surface stride */
        unsigned short u16Reserved;
    } OSD_SURFACE_S;

    typedef struct osd_logo
    {
        unsigned int width;        /* out */
        unsigned int height;       /* out */
        unsigned int stride;       /* in */
        unsigned char *pRGBBuffer; /* in/out */
    } OSD_LOGO_T;

    typedef struct osd_bitmapinfoheader
    {
        unsigned short biSize;
        unsigned int biWidth;
        int biHeight;
        unsigned short biPlanes;
        unsigned short biBitCount;
        unsigned int biCompression;
        unsigned int biSizeImage;
        unsigned int biXPelsPerMeter;
        unsigned int biYPelsPerMeter;
        unsigned int biClrUsed;
        unsigned int biClrImportant;
    } OSD_BITMAPINFOHEADER;

    typedef struct osd_bitmapfileheader
    {
        unsigned int bfSize;
        unsigned short bfReserved1;
        unsigned short bfReserved2;
        unsigned int bfOffBits;
    } OSD_BITMAPFILEHEADER;

    typedef struct osd_rgbquad
    {
        unsigned char rgbBlue;
        unsigned char rgbGreen;
        unsigned char rgbRed;
        unsigned char rgbReserved;
    } OSD_RGBQUAD;

    typedef struct osd_bitmapinfo
    {
        OSD_BITMAPINFOHEADER bmiHeader;
        OSD_RGBQUAD bmiColors[1];
    } OSD_BITMAPINFO;

    typedef struct osd_comp_info
    {
        int alen;
        int rlen;
        int glen;
        int blen;
    } OSD_COMP_INFO;

    int parse_bitmap(const char *filename, OSD_BITMAPFILEHEADER *pBmpFileHeader, OSD_BITMAPINFO *pBmpInfo);
    int CreateSurfaceByBitMap(const char *pszFileName, OSD_SURFACE_S *pstSurface, unsigned char *pu8Virt);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif