#ifndef OSD_H_
#define OSD_H_

#ifdef __cplusplus
#if __cplusplus
extern "C"
{
#endif
#endif

#include "common.h"
#include "bitmap.h"

    int create_region(int *handle, int x, int y, int width, int height);
    int prepare_bitmap(const char *filename, BITMAP *bitmap, int bFil, unsigned int u16FilColor, int enPixelFmt);
    int set_bitmap(int handle, BITMAP *bitmap);
    void unload_region(int *handle);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif