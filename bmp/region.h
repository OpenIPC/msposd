#ifndef OSD_H_
#define OSD_H_

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

//#define PIXEL_FORMAT_DEFAULT E_MI_RGN_PIXEL_FORMAT_I8
#include "bitmap.h"
#include "common.h"

int create_region(int *handle, int x, int y, int width, int height);
int prepare_bitmap(
	const char *filename, BITMAP *bitmap, int bFil, unsigned int u16FilColor, int enPixelFmt);
int set_bitmap(int handle, BITMAP *bitmap);
unsigned long set_bitmapEx(int handle, BITMAP *bitmap, int BitsPerPixel);
int unload_region(int *handle);
void *get_directBMP(int handle);
#ifdef __SIGMASTAR__
int GetCanvas(int handle, MI_RGN_CanvasInfo_t *stCanvasInfo);
uint32_t ST_OSD_DrawPoint(
	MI_U16 *pDst, MI_U32 u32Stride, uint32_t u32X, uint32_t u32Y, MI_U32 u32Color);
#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif
