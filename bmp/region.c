#include "region.h"


PIXEL_FORMAT_DEFAULT=0;

const double inv16 = 1.0 / 16.0;

int create_region(int *handle, int x, int y, int width, int height)
{
    int s32Ret = -1;
#ifndef _x86    
#ifdef __SIGMASTAR__
    MI_RGN_ChnPort_t stChn;

    MI_RGN_Attr_t stRegionCurrent;
    MI_RGN_Attr_t stRegion;

    MI_RGN_ChnPortParam_t stChnAttr;
    MI_RGN_ChnPortParam_t stChnAttrCurrent;

    stChn.eModId = E_MI_RGN_MODID_VPE;
    stChn.s32DevId = 0;
    stChn.s32ChnId = 0;

    stRegion.eType = E_MI_RGN_TYPE_OSD;
    stRegion.stOsdInitParam.stSize.u32Height = height;
    stRegion.stOsdInitParam.stSize.u32Width = width;
//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!    
    stRegion.stOsdInitParam.ePixelFmt = PIXEL_FORMAT_DEFAULT ;//PIXEL_FORMAT_1555;// E_MI_RGN_PIXEL_FORMAT_I4; //;PIXEL_FORMAT_1555;//E_MI_RGN_PIXEL_FORMAT_I4;//PIXEL_FORMAT_1555

    s32Ret = MI_RGN_GetAttr(*handle, &stRegionCurrent);
#else
    MPP_CHN_S stChn;

    RGN_ATTR_S stRegionCurrent;
    RGN_ATTR_S stRegion;

    RGN_CHN_ATTR_S stChnAttr;
    RGN_CHN_ATTR_S stChnAttrCurrent;

    stChn.enModId = HI_ID_VENC;
    stChn.s32DevId = 0;
    stChn.s32ChnId = 0;

    stRegion.enType = OVERLAY_RGN;
    stRegion.unAttr.stOverlay.stSize.u32Height = height;
    stRegion.unAttr.stOverlay.stSize.u32Width = width;
    stRegion.unAttr.stOverlay.u32CanvasNum = 2;
    stRegion.unAttr.stOverlay.enPixelFmt = PIXEL_FORMAT_1555;
    stRegion.unAttr.stOverlay.u32BgColor = 0x7fff;

    s32Ret = HI_MPI_RGN_GetAttr(*handle, &stRegionCurrent);
#endif
    if (s32Ret)
    {
        fprintf(stderr, "[%s:%d]RGN_GetAttr failed with %#x , creating region %d...\n", __func__, __LINE__, s32Ret, *handle);
#ifdef __SIGMASTAR__
        s32Ret = MI_RGN_Create(*handle, &stRegion);
#else
        s32Ret = HI_MPI_RGN_Create(*handle, &stRegion);
#endif
        if (s32Ret)
        {
            fprintf(stderr, "[%s:%d]RGN_Create failed with %#x!\n", __func__, __LINE__, s32Ret);
            return -1;
        }
    }
    else
    {
#ifdef __SIGMASTAR__
        if (stRegionCurrent.stOsdInitParam.stSize.u32Height != stRegion.stOsdInitParam.stSize.u32Height || stRegionCurrent.stOsdInitParam.stSize.u32Width != stRegion.stOsdInitParam.stSize.u32Width)
#else
        if (stRegionCurrent.unAttr.stOverlay.stSize.u32Height != stRegion.unAttr.stOverlay.stSize.u32Height || stRegionCurrent.unAttr.stOverlay.stSize.u32Width != stRegion.unAttr.stOverlay.stSize.u32Width)
#endif
        {
            fprintf(stderr, "[%s:%d] Region parameters are different, recreating ... \n", __func__, __LINE__);
#ifdef __SIGMASTAR__
            stChn.s32OutputPortId = 1;
            MI_RGN_DetachFromChn(*handle, &stChn);
            stChn.s32OutputPortId = 0;
            MI_RGN_DetachFromChn(*handle, &stChn);
            MI_RGN_Destroy(*handle);
            s32Ret = MI_RGN_Create(*handle, &stRegion);
#else
            HI_MPI_RGN_DetachFromChn(*handle, &stChn);
            HI_MPI_RGN_Destroy(*handle);
            s32Ret = HI_MPI_RGN_Create(*handle, &stRegion);
#endif
            if (s32Ret)
            {
                fprintf(stderr, "[%s:%d]RGN_Create failed with %#x!\n", __func__, __LINE__, s32Ret);
                return -1;
            }
        }
    }

#ifdef __SIGMASTAR__
    s32Ret = MI_RGN_GetDisplayAttr(*handle, &stChn, &stChnAttrCurrent);
#else
    s32Ret = HI_MPI_RGN_GetDisplayAttr(*handle, &stChn, &stChnAttrCurrent);
#endif
    if (s32Ret)
        fprintf(stderr, "[%s:%d]RGN_GetDisplayAttr failed with %#x %d, attaching...\n", __func__, __LINE__, s32Ret, *handle);
#ifdef __SIGMASTAR__
    else if (stChnAttrCurrent.stPoint.u32X != x || stChnAttrCurrent.stPoint.u32Y != y)
#else
    else if (stChnAttrCurrent.unChnAttr.stOverlayChn.stPoint.s32X != x || stChnAttrCurrent.unChnAttr.stOverlayChn.stPoint.s32Y != y)
#endif
    {
        fprintf(stderr, "[%s:%d] Position has changed, detaching handle %d from channel %d...\n", __func__, __LINE__, *handle, &stChn.s32ChnId);
#ifdef __SIGMASTAR__
        stChn.s32OutputPortId = 1;
        MI_RGN_DetachFromChn(*handle, &stChn);
        stChn.s32OutputPortId = 0;
        MI_RGN_DetachFromChn(*handle, &stChn);
#else
        HI_MPI_RGN_DetachFromChn(*handle, &stChn);
#endif
    }

#ifdef __SIGMASTAR__
    memset(&stChnAttr, 0, sizeof(MI_RGN_ChnPortParam_t));
    stChnAttr.bShow = 1;
    stChnAttr.stPoint.u32X = x;
    stChnAttr.stPoint.u32Y = y;
    stChnAttr.unPara.stOsdChnPort.u32Layer = 0;
    
    stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.eAlphaMode = E_MI_RGN_PIXEL_ALPHA;
    stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8BgAlpha = 0;//0;
    stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8FgAlpha = 255;
    
    /*
    stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.eAlphaMode =  E_MI_RGN_CONSTANT_ALPHA;
    stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8BgAlpha = 64;
    stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8FgAlpha = 127;
    */

    stChn.s32OutputPortId = 0;
    s32Ret =MI_RGN_AttachToChn(*handle, &stChn, &stChnAttr);
    stChn.s32OutputPortId = 1;
    s32Ret = MI_RGN_AttachToChn(*handle, &stChn, &stChnAttr);
#else
    memset(&stChnAttr, 0, sizeof(RGN_CHN_ATTR_S));
    stChnAttr.bShow = 1;
    stChnAttr.enType = OVERLAY_RGN;
    stChnAttr.unChnAttr.stOverlayChn.u32BgAlpha = 0;
    stChnAttr.unChnAttr.stOverlayChn.u32FgAlpha = 128;
    stChnAttr.unChnAttr.stOverlayChn.stQpInfo.bQpDisable = 0;
    stChnAttr.unChnAttr.stOverlayChn.stQpInfo.bAbsQp = 0;
    stChnAttr.unChnAttr.stOverlayChn.stQpInfo.s32Qp = 0;
#ifndef __16CV300__
    stChnAttr.unChnAttr.stOverlayChn.u16ColorLUT[0] = 0x3e0;
    stChnAttr.unChnAttr.stOverlayChn.u16ColorLUT[1] = 0x7FFF;
    stChnAttr.unChnAttr.stOverlayChn.enAttachDest = ATTACH_JPEG_MAIN;
#endif
    stChnAttr.unChnAttr.stOverlayChn.stPoint.s32X = x;
    stChnAttr.unChnAttr.stOverlayChn.stPoint.s32Y = y;
    stChnAttr.unChnAttr.stOverlayChn.u32Layer = 7;

    HI_MPI_RGN_AttachToChn(*handle, &stChn, &stChnAttr);
#endif

#endif
    return s32Ret;
}

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
        if (bFil>0)
        {
             
            for (i = 0; i < bitmap->u32Height; i++)
            {
                for (j = 0; j < bitmap->u32Width; j++)
                {//64478 = 0xFBDE R:245,G:245,B:245
                    if (bFil==2 && i==0 && j==0)//take transparent color from first pixel
                        u16FilColor = *pu16Temp; 

                    if (u16FilColor == *pu16Temp){
                        *pu16Temp &= 0x7FFF;                        
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


int set_bitmap(int handle, BITMAP *bitmap)
{

    int s32Ret=0;
#ifndef _x86    
#ifdef __SIGMASTAR__
     s32Ret = MI_RGN_SetBitMap(handle, (MI_RGN_Bitmap_t *)(bitmap));
#elif __GOKE__
     s32Ret = HI_MPI_RGN_SetBitMap(handle, (BITMAP_S *)(bitmap));
#endif
    if (s32Ret)
    {
        if (s32Ret==E_MI_ERR_ILLEGAL_PARAM)
            fprintf(stderr, "RGN_SetBitMap failed E_MI_ERR_ILLEGAL_PARAM %#x!\n", s32Ret);
        fprintf(stderr, "RGN_SetBitMap failed with %#x  %d!\n", s32Ret, (s32Ret & 0xFFF) );
        return -1;
    }
#endif
    return s32Ret;
}

void unload_region(int *handle)
{
#ifdef __SIGMASTAR__
    MI_RGN_ChnPort_t stChn;
    stChn.s32DevId = 0;
    stChn.s32ChnId = 0;

    stChn.eModId = E_MI_RGN_MODID_VPE;
    stChn.s32OutputPortId = 1;
    MI_RGN_DetachFromChn(*handle, &stChn);
    stChn.s32OutputPortId = 0;
    MI_RGN_DetachFromChn(*handle, &stChn);
    int s32Ret = MI_RGN_Destroy(*handle);
    if (s32Ret)
        fprintf(stderr, "[%s:%d]RGN_Destroy failed with %#x %d!\n", __func__, __LINE__, s32Ret, *handle);
#elif __GOKE__
    MPP_CHN_S stChn;
    stChn.s32DevId = 0;
    stChn.s32ChnId = 0;

    stChn.enModId = HI_ID_VENC;
    HI_MPI_RGN_DetachFromChn(*handle, &stChn);
    int s32Ret = HI_MPI_RGN_Destroy(*handle);
    if (s32Ret)
        fprintf(stderr, "[%s:%d]RGN_Destroy failed with %#x %d!\n", __func__, __LINE__, s32Ret, *handle);
#endif
    
}