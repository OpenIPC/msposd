#include "region.h"

const double inv16 = 1.0 / 16.0;
 
static int skipcol=1;

int prepare_bitmap(const char *filename, BITMAP *bitmap, int bFil, unsigned int u16FilColor, int enPixelFmt)
{
    OSD_SURFACE_S Surface;
    OSD_BITMAPFILEHEADER bmpFileHeader;
    OSD_BITMAPINFO bmpInfo;
    int s32BytesPerPix = 2;
    unsigned char *pu8Data;
    int R_Value;
    int G_Value;
    int B_Value;
    int Gr_Value;
    unsigned char Value_tmp;
    unsigned char Value;
    int s32Width;

    if (parse_bitmap(filename, &bmpFileHeader, &bmpInfo) < 0)
    {
        fprintf(stderr, "GetBmpInfo err!\n");
        return -1;
    }

    switch (enPixelFmt)
    {
    case PIXEL_FORMAT_4444:
        Surface.enColorFmt = OSD_COLOR_FMT_RGB4444;
        break;
    case PIXEL_FORMAT_1555:
    case PIXEL_FORMAT_2BPP:
        Surface.enColorFmt = OSD_COLOR_FMT_RGB1555;
        break;
    case PIXEL_FORMAT_8888:
        Surface.enColorFmt = OSD_COLOR_FMT_RGB8888;
        s32BytesPerPix = 4;
        break;
    default:
        fprintf(stderr, "enPixelFormat err %d \n", enPixelFmt);
        return -1;
    }

    if (!(bitmap->pData = malloc(s32BytesPerPix * bmpInfo.bmiHeader.biWidth * bmpInfo.bmiHeader.biHeight)))
    {
        fputs("malloc osd memory err!\n", stderr);
        return -1;
    }

    CreateSurfaceByBitMap(filename, &Surface, (unsigned char *)(bitmap->pData));

    bitmap->u32Width = Surface.u16Width;
    bitmap->u32Height = Surface.u16Height;
    bitmap->enPixelFormat = enPixelFmt;

    int i, j, k;
    unsigned char *pu8Temp;
    
    if (skipcol>700)//test performance
        skipcol=1;
    skipcol++;

#ifndef __16CV300__
    if (enPixelFmt == PIXEL_FORMAT_2BPP)
    {
        s32Width = DIV_UP(bmpInfo.bmiHeader.biWidth, 4);
        pu8Data = malloc(s32Width * bmpInfo.bmiHeader.biHeight);
        if (!pu8Data)
        {
            fputs("malloc osd memory err!\n", stderr);
            return -1;
        }
    }

#endif
    long ccc=0;
    if (enPixelFmt != PIXEL_FORMAT_2BPP)
    {
        unsigned short *pu16Temp;
        pu16Temp = (unsigned short *)bitmap->pData;
        if (bFil)
        {
             
            for (i = 0; i < bitmap->u32Height; i++)
            {
                for (j = 0; j < bitmap->u32Width; j++)
                {
                    if (u16FilColor == *pu16Temp)
                    {
                        *pu16Temp &= 0x7FFF;
                        
                    }
                    if (j==skipcol){
                        *pu16Temp = 0x80FF;
                         ccc++;
                    }
                    pu16Temp++;
                }
            }
        }
        printf("enPixelFmt:%d\n",ccc);
    }
    else
    {
        unsigned short *pu16Temp;

        pu16Temp = (unsigned short *)bitmap->pData;
        pu8Temp = (unsigned char *)pu8Data;

        for (i = 0; i < bitmap->u32Height; i++)
        {
            for (j = 0; j < bitmap->u32Width / 4; j++)
            {
                Value = 0;

                for (k = j; k < j + 4; k++)
                {
                    B_Value = *pu16Temp & 0x001F;
                    G_Value = *pu16Temp >> 5 & 0x001F;
                    R_Value = *pu16Temp >> 10 & 0x001F;
                    pu16Temp++;

                    Gr_Value = (R_Value * 299 + G_Value * 587 + B_Value * 144 + 500) / 1000;
                    if (Gr_Value > 16)
                        Value_tmp = 0x01;
                    else
                        Value_tmp = 0x00;
                    Value = (Value << 2) + Value_tmp;
                }
                *pu8Temp = Value;
                pu8Temp++;
            }
        }
        free(bitmap->pData);
        bitmap->pData = pu8Data;
    }

    return 0;
}



int prepare_bitmapOld(const char *filename, BITMAP *bitmap, int bFil, unsigned int u16FilColor, int enPixelFmt)
{
    OSD_SURFACE_S Surface;
    OSD_BITMAPFILEHEADER bmpFileHeader;
    OSD_BITMAPINFO bmpInfo;
    int s32BytesPerPix = 2;
    unsigned char *pu8Data;
    int R_Value;
    int G_Value;
    int B_Value;
    int Gr_Value;
    unsigned char Value_tmp;
    unsigned char Value;
    int s32Width;

    if (parse_bitmap(filename, &bmpFileHeader, &bmpInfo) < 0)
    {
        fprintf(stderr, "GetBmpInfo err!\n");
        return -1;
    }

    switch (enPixelFmt)
    {
    case PIXEL_FORMAT_4444:
        Surface.enColorFmt = OSD_COLOR_FMT_RGB4444;
        break;
    case PIXEL_FORMAT_1555:
    case PIXEL_FORMAT_2BPP:
        Surface.enColorFmt = OSD_COLOR_FMT_RGB1555;
        break;
    case PIXEL_FORMAT_8888:
        Surface.enColorFmt = OSD_COLOR_FMT_RGB8888;
        s32BytesPerPix = 4;
        break;
    default:
        fprintf(stderr, "enPixelFormat err %d \n", enPixelFmt);
        return -1;
    }

    if (!(bitmap->pData = malloc(s32BytesPerPix * bmpInfo.bmiHeader.biWidth * bmpInfo.bmiHeader.biHeight)))
    {
        fputs("malloc osd memory err!\n", stderr);
        return -1;
    }

    CreateSurfaceByBitMap(filename, &Surface, (unsigned char *)(bitmap->pData));

    bitmap->u32Width = Surface.u16Width;
    bitmap->u32Height = Surface.u16Height;
    bitmap->enPixelFormat = enPixelFmt;

    int i, j, k;
    unsigned char *pu8Temp;
    
    if (skipcol>700)//test performance
        skipcol=1;
    skipcol++;

#ifndef __16CV300__
    if (enPixelFmt == PIXEL_FORMAT_2BPP)
    {
        s32Width = DIV_UP(bmpInfo.bmiHeader.biWidth, 4);
        pu8Data = malloc(s32Width * bmpInfo.bmiHeader.biHeight);
        if (!pu8Data)
        {
            fputs("malloc osd memory err!\n", stderr);
            return -1;
        }
    }

#endif
    long ccc=0;
    if (enPixelFmt != PIXEL_FORMAT_2BPP)
    {
        unsigned short *pu16Temp;
        pu16Temp = (unsigned short *)bitmap->pData;
        if (bFil)
        {
             
            for (i = 0; i < bitmap->u32Height; i++)
            {
                for (j = 0; j < bitmap->u32Width; j++)
                {
                    if (u16FilColor == *pu16Temp)
                    {
                        *pu16Temp &= 0x7FFF;
                        
                    }
                    if (j==skipcol){
                        *pu16Temp = 0x80FF;
                         ccc++;
                    }
                    pu16Temp++;
                }
            }
        }
        printf("enPixelFmt:%d\n",ccc);
    }
    else
    {
        unsigned short *pu16Temp;

        pu16Temp = (unsigned short *)bitmap->pData;
        pu8Temp = (unsigned char *)pu8Data;

        for (i = 0; i < bitmap->u32Height; i++)
        {
            for (j = 0; j < bitmap->u32Width / 4; j++)
            {
                Value = 0;

                for (k = j; k < j + 4; k++)
                {
                    B_Value = *pu16Temp & 0x001F;
                    G_Value = *pu16Temp >> 5 & 0x001F;
                    R_Value = *pu16Temp >> 10 & 0x001F;
                    pu16Temp++;

                    Gr_Value = (R_Value * 299 + G_Value * 587 + B_Value * 144 + 500) / 1000;
                    if (Gr_Value > 16)
                        Value_tmp = 0x01;
                    else
                        Value_tmp = 0x00;
                    Value = (Value << 2) + Value_tmp;
                }
                *pu8Temp = Value;
                pu8Temp++;
            }
        }
        free(bitmap->pData);
        bitmap->pData = pu8Data;
    }

    return 0;
}


