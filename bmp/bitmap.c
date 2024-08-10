#include "bitmap.h"

OSD_COMP_INFO s_OSDCompInfo[OSD_COLOR_FMT_BUTT] = {
    {0, 4, 4, 4}, /*RGB444*/
    {4, 4, 4, 4}, /*ARGB4444*/
    {0, 5, 5, 5}, /*RGB555*/
    {0, 5, 6, 5}, /*RGB565*/
    {1, 5, 5, 5}, /*ARGB1555*/
    {0, 0, 0, 0}, /*RESERVED*/
    {0, 8, 8, 8}, /*RGB888*/
    {8, 8, 8, 8}  /*ARGB8888*/
};

static inline unsigned short convert_2bpp(unsigned char r, unsigned char g, unsigned char b, OSD_COMP_INFO compinfo)
{
    unsigned short pixel = 0;
    unsigned int tmp = 15;

    unsigned char r1 = r >> (8 - compinfo.rlen);
    unsigned char g1 = g >> (8 - compinfo.glen);
    unsigned char b1 = b >> (8 - compinfo.blen);
    while (compinfo.alen)
    {
        pixel |= (1 << tmp);
        tmp--;
        compinfo.alen--;
    }

    pixel |= (r1 | (g1 << compinfo.blen) | (b1 << (compinfo.blen + compinfo.glen)));
    return pixel;
}

int parse_bitmap(const char *filename, OSD_BITMAPFILEHEADER *pBmpFileHeader, OSD_BITMAPINFO *pBmpInfo)
{
    FILE *pFile;
    unsigned short bfType;

    if (!filename)
    {
        fprintf(stderr, "load_bitmap: filename=NULL\n");
        return -1;
    }

    if ((pFile = fopen(filename, "rb")) == NULL)
    {
        fprintf(stderr, "Open file faild:%s!\n", filename);
        return -1;
    }

    if (fread(&bfType, 1, sizeof(bfType), pFile) != sizeof(bfType))
    {
        fprintf(stderr, "fread file failed:%s!\n", filename);
        fclose(pFile);
        return -1;
    }

    if (bfType != 0x4d42)
    {
        fprintf(stderr, "not bitmap file\n");
        fclose(pFile);
        return -1;
    }

    if (fread(pBmpFileHeader, 1, sizeof(OSD_BITMAPFILEHEADER), pFile) != sizeof(OSD_BITMAPFILEHEADER))
    {
        fprintf(stderr, "fread OSD_BITMAPFILEHEADER failed:%s!\n", filename);
        fclose(pFile);
        return -1;
    }

    if (fread(pBmpInfo, 1, sizeof(OSD_BITMAPINFO), pFile) != sizeof(OSD_BITMAPINFO))
    {
        fprintf(stderr, "fread OSD_BITMAPINFO failed:%s!\n", filename);
        fclose(pFile);
        return -1;
    }

    if (pBmpInfo->bmiHeader.biBitCount / 8 < 2)
    {
        fprintf(stderr, "bitmap format not supported!\n");
        fclose(pFile);
        return -1;
    }

    if (pBmpInfo->bmiHeader.biCompression != 0 && pBmpInfo->bmiHeader.biCompression != 3)
    {
        fprintf(stderr, "not support compressed bitmap file!\n");
        fclose(pFile);
        return -1;
    }

    if (pBmpInfo->bmiHeader.biHeight < 0)
    {
        fprintf(stderr, "bmpInfo.bmiHeader.biHeight < 0\n");
        fclose(pFile);
        return -1;
    }

    fclose(pFile);
    return 0;
}

int load_bitmap(const char *filename, OSD_LOGO_T *pVideoLogo)
{
    FILE *pFile;

    unsigned int w, h, stride;
    unsigned short Bpp, dstBpp;

    OSD_BITMAPFILEHEADER bmpFileHeader;
    OSD_BITMAPINFO bmpInfo;

    unsigned char *pOrigBMPBuf;
    unsigned char *pRGBBuf;

    if (parse_bitmap(filename, &bmpFileHeader, &bmpInfo) < 0)
        return -1;

    if (!(pFile = fopen(filename, "rb")))
    {
        fprintf(stderr, "Open file faild:%s!\n", filename);
        return -1;
    }

    pVideoLogo->width = (unsigned short)bmpInfo.bmiHeader.biWidth;
    pVideoLogo->height = (unsigned short)((bmpInfo.bmiHeader.biHeight > 0) ? bmpInfo.bmiHeader.biHeight : (-bmpInfo.bmiHeader.biHeight));
    w = pVideoLogo->width;
    h = pVideoLogo->height;

    Bpp = bmpInfo.bmiHeader.biBitCount / 8;
    stride = w * Bpp;
    if (stride % 4)
        stride = (stride & 0xfffc) + 4;

    /* RGB8888 or RGB1555 */
    pOrigBMPBuf = (unsigned char *)malloc(h * stride);
    if (!pOrigBMPBuf)
    {
        fprintf(stderr, "not enough memory to malloc!\n");
        fclose(pFile);
        return -1;
    }

    pRGBBuf = pVideoLogo->pRGBBuffer;

    if (fseek(pFile, bmpFileHeader.bfOffBits, 0))
    {
        fprintf(stderr, "fseek failed!\n");
        fclose(pFile);
        free(pOrigBMPBuf);
        pOrigBMPBuf = NULL;
        return -1;
    }

    if (fread(pOrigBMPBuf, 1, (unsigned int)(h * stride), pFile) != (unsigned int)(h * stride))
    {
        fprintf(stderr, "fread (%d*%d)error!line:%d\n", h, stride, __LINE__);
        perror("fread:");
    }

    if (Bpp > 2)
        dstBpp = 4;
    else
        dstBpp = 2;

    if (pVideoLogo->stride == 0)
        pVideoLogo->stride = pVideoLogo->width * dstBpp;

    for (unsigned short i = 0; i < h; i++)
    {
        for (unsigned short j = 0; j < w; j++)
        {
            memcpy(pRGBBuf + i * pVideoLogo->stride + j * dstBpp, pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);

            if (dstBpp == 4)
                *(pRGBBuf + i * pVideoLogo->stride + j * dstBpp + 3) = 0x80; /*alpha*/
        }
    }

    free(pOrigBMPBuf);
    pOrigBMPBuf = NULL;

    fclose(pFile);
    return 0;
}

int load_bitmapex(const char *filename, OSD_LOGO_T *pVideoLogo, OSD_COLOR_FMT_E enFmt)
{
    FILE *pFile;

    unsigned int w, h, stride;
    unsigned short Bpp;

    OSD_BITMAPFILEHEADER bmpFileHeader;
    OSD_BITMAPINFO bmpInfo;

    unsigned char *pOrigBMPBuf;
    unsigned char *pRGBBuf;
    unsigned char r, g, b;
    unsigned char *pStart;
    unsigned short *pDst;

    if (parse_bitmap(filename, &bmpFileHeader, &bmpInfo) < 0)
        return -1;

    if (!(pFile = fopen(filename, "rb")))
    {
        fprintf(stderr, "Open file failed:%s!\n", filename);
        return -1;
    }

    pVideoLogo->width = (unsigned short)bmpInfo.bmiHeader.biWidth;
    pVideoLogo->height = (unsigned short)((bmpInfo.bmiHeader.biHeight > 0) ? bmpInfo.bmiHeader.biHeight : (-bmpInfo.bmiHeader.biHeight));
    w = pVideoLogo->width;
    h = pVideoLogo->height;

    Bpp = bmpInfo.bmiHeader.biBitCount / 8;
    stride = w * Bpp;
    if (stride % 4)
        stride = (stride & 0xfffc) + 4;

    /* RGB8888 or RGB1555 */
    pOrigBMPBuf = (unsigned char *)malloc(h * stride);
    if (!pOrigBMPBuf)
    {
        fprintf(stderr, "not enough memory to malloc!\n");
        fclose(pFile);
        return -1;
    }

    pRGBBuf = pVideoLogo->pRGBBuffer;

    if (fseek(pFile, bmpFileHeader.bfOffBits, 0))
    {
        fprintf(stderr, "fseek failed!\n");
        fclose(pFile);
        free(pOrigBMPBuf);
        pOrigBMPBuf = NULL;
        return -1;
    }

    if (fread(pOrigBMPBuf, 1, (unsigned int)(h * stride), pFile) != (unsigned int)(h * stride))
    {
        fprintf(stderr, "fread (%d*%d)error!line:%d\n", h, stride, __LINE__);
        perror("fread:");
    }

    if (enFmt >= OSD_COLOR_FMT_RGB888)
        pVideoLogo->stride = pVideoLogo->width * 4;
    else
        pVideoLogo->stride = pVideoLogo->width * 2;

    for (unsigned short i = 0; i < h; i++)
    {
        for (unsigned short j = 0; j < w; j++)
        {
            if (Bpp == 3) /*.....*/
            {
                switch (enFmt)
                {
                case OSD_COLOR_FMT_RGB444:
                case OSD_COLOR_FMT_RGB555:
                case OSD_COLOR_FMT_RGB565:
                case OSD_COLOR_FMT_RGB1555:
                case OSD_COLOR_FMT_RGB4444:
                    /* start color convert */
                    pStart = pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp;
                    pDst = (unsigned short *)(pRGBBuf + i * pVideoLogo->stride + j * 2);
                    r = *(pStart);
                    g = *(pStart + 1);
                    b = *(pStart + 2);
                    *pDst = convert_2bpp(r, g, b, s_OSDCompInfo[enFmt]);
                    break;

                case OSD_COLOR_FMT_RGB888:
                case OSD_COLOR_FMT_RGB8888:
                    memcpy(pRGBBuf + i * pVideoLogo->stride + j * 4, pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);
                    *(pRGBBuf + i * pVideoLogo->stride + j * 4 + 3) = 0xff; /*alpha*/
                    break;

                default:
                    fprintf(stderr, "file(%s), line(%d), no such format!\n", __FILE__, __LINE__);
                    break;
                }
            }
            else if ((Bpp == 2) || (Bpp == 4)) /*..............*/
            {
                memcpy(pRGBBuf + i * pVideoLogo->stride + j * Bpp, pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);
            }
        }
    }

    free(pOrigBMPBuf);
    pOrigBMPBuf = NULL;

    fclose(pFile);
    return 0;
}

int load_bitmap_canvas(const char *filename, OSD_LOGO_T *pVideoLogo, OSD_COLOR_FMT_E enFmt)
{
    FILE *pFile;

    unsigned int w, h, stride;
    unsigned short Bpp;

    OSD_BITMAPFILEHEADER bmpFileHeader;
    OSD_BITMAPINFO bmpInfo;

    unsigned char *pOrigBMPBuf;
    unsigned char *pRGBBuf;
    unsigned char r, g, b;
    unsigned char *pStart;
    unsigned short *pDst;

    if (parse_bitmap(filename, &bmpFileHeader, &bmpInfo) < 0)
        return -1;

    if (!(pFile = fopen(filename, "rb")))
    {
        fprintf(stderr, "Open file faild:%s!\n", filename);
        return -1;
    }

    Bpp = bmpInfo.bmiHeader.biBitCount / 8;
    w = (unsigned short)bmpInfo.bmiHeader.biWidth;
    h = (unsigned short)((bmpInfo.bmiHeader.biHeight > 0) ? bmpInfo.bmiHeader.biHeight : (-bmpInfo.bmiHeader.biHeight));

    stride = w * Bpp;
    if (stride % 4)
        stride = (stride & 0xfffc) + 4;

    /* RGB8888 or RGB1555 */
    pOrigBMPBuf = (unsigned char *)malloc(h * stride);
    if (!pOrigBMPBuf)
    {
        fprintf(stderr, "not enough memory to malloc!\n");
        fclose(pFile);
        return -1;
    }

    pRGBBuf = pVideoLogo->pRGBBuffer;

    if (stride > pVideoLogo->stride)
    {
        fprintf(stderr, "Bitmap's stride(%d) is bigger than canvas's stide(%d). Load bitmap error!\n", stride, pVideoLogo->stride);
        fclose(pFile);
        free(pOrigBMPBuf);
        return -1;
    }

    if (h > pVideoLogo->height)
    {
        fprintf(stderr, "Bitmap's height(%d) is bigger than canvas's height(%d). Load bitmap error!\n", h, pVideoLogo->height);
        fclose(pFile);
        free(pOrigBMPBuf);
        return -1;
    }

    if (w > pVideoLogo->width)
    {
        fprintf(stderr, "Bitmap's width(%d) is bigger than canvas's width(%d). Load bitmap error!\n", w, pVideoLogo->width);
        fclose(pFile);
        free(pOrigBMPBuf);
        return -1;
    }

    if (fseek(pFile, bmpFileHeader.bfOffBits, 0))
    {
        fprintf(stderr, "fseek error!\n");
        fclose(pFile);
        free(pOrigBMPBuf);
        return -1;
    }

    if (fread(pOrigBMPBuf, 1, (unsigned int)(h * stride), pFile) != (unsigned int)(h * stride))
    {
        fprintf(stderr, "fread (%d*%d)error!line:%d\n", h, stride, __LINE__);
        perror("fread:");
    }

    for (unsigned short i = 0; i < h; i++)
    {
        for (unsigned short j = 0; j < w; j++)
        {
            if (Bpp == 3) /*.....*/
            {
                switch (enFmt)
                {
                case OSD_COLOR_FMT_RGB444:
                case OSD_COLOR_FMT_RGB555:
                case OSD_COLOR_FMT_RGB565:
                case OSD_COLOR_FMT_RGB1555:
                case OSD_COLOR_FMT_RGB4444:
                    /* start color convert */
                    pStart = pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp;
                    pDst = (unsigned short *)(pRGBBuf + i * pVideoLogo->stride + j * 2);
                    r = *(pStart);
                    g = *(pStart + 1);
                    b = *(pStart + 2);
                    // fprintf(stderr, "Func: %s, line:%d, Bpp: %d, bmp stride: %d, Canvas stride: %d, h:%d, w:%d.\n",
                    //     __FUNCTION__, __LINE__, Bpp, stride, pVideoLogo->stride, i, j);
                    *pDst = convert_2bpp(r, g, b, s_OSDCompInfo[enFmt]);

                    break;

                case OSD_COLOR_FMT_RGB888:
                case OSD_COLOR_FMT_RGB8888:
                    memcpy(pRGBBuf + i * pVideoLogo->stride + j * 4, pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);
                    *(pRGBBuf + i * pVideoLogo->stride + j * 4 + 3) = 0xff; /*alpha*/
                    break;

                default:
                    fprintf(stderr, "file(%s), line(%d), no such format!\n", __FILE__, __LINE__);
                    break;
                }
            }
            else if ((Bpp == 2) || (Bpp == 4)) /*..............*/
            {
                memcpy(pRGBBuf + i * pVideoLogo->stride + j * Bpp, pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);
            }
        }
    }

    free(pOrigBMPBuf);
    pOrigBMPBuf = NULL;

    fclose(pFile);
    return 0;
}

char *extract_extension(char *filename)
{
    char *pret = NULL;

    if (!filename)
    {
        fprintf(stderr, "filename can't be null!");
        return NULL;
    }

    unsigned int fnLen = strlen(filename);
    while (fnLen)
    {
        pret = filename + fnLen;
        if (*pret == '.')
            return (pret + 1);
        fnLen--;
    }

    return pret;
}

int load_image(const char *filename, OSD_LOGO_T *pVideoLogo)
{
    char *ext = extract_extension((char *)filename);

    if (ext && !strcmp(ext, "bmp"))
    {
        if (load_bitmap(filename, pVideoLogo))
        {
            fprintf(stderr, "load_bitmap error!\n");
            return -1;
        }
    }
    else
    {
        fprintf(stderr, "not supported image file!\n");
        return -1;
    }

    return 0;
}

int load_imageex(const char *filename, OSD_LOGO_T *pVideoLogo, OSD_COLOR_FMT_E enFmt)
{
    char *ext = extract_extension((char*)filename);

    if (ext && !strcmp(ext, "bmp"))
    {
        if (load_bitmapex(filename, pVideoLogo, enFmt))
        {
            fprintf(stderr, "load_bitmap error!\n");
            return -1;
        }
    }
    else
    {
        fprintf(stderr, "not supported image file!\n");
        return -1;
    }

    return 0;
}

int load_canvasex(const char *filename, OSD_LOGO_T *pVideoLogo, OSD_COLOR_FMT_E enFmt)
{
    char *ext = extract_extension((char *)filename);

    if (ext && !strcmp(ext, "bmp"))
    {
        if (load_bitmap_canvas(filename, pVideoLogo, enFmt))
        {
            fprintf(stderr, "load_bitmap error!\n");
            return -1;
        }
    }
    else
    {
        fprintf(stderr, "not supported image file!\n");
        return -1;
    }

    return 0;
}

int LoadBitMap2Surface(const char *pszFileName, const OSD_SURFACE_S *pstSurface, unsigned char *pu8Virt)
{
    OSD_LOGO_T stLogo;

    stLogo.stride = pstSurface->u16Stride;
    stLogo.pRGBBuffer = pu8Virt;

    return load_image(pszFileName, &stLogo);
}

int CreateSurfaceByBitMap(const char *pszFileName, OSD_SURFACE_S *pstSurface, unsigned char *pu8Virt)
{
    OSD_LOGO_T stLogo;

    stLogo.pRGBBuffer = pu8Virt;

    if (load_imageex(pszFileName, &stLogo, pstSurface->enColorFmt) < 0)
    {
        fprintf(stderr, "load bmp error!\n");
        return -1;
    }

    pstSurface->u16Height = stLogo.height;
    pstSurface->u16Width = stLogo.width;
    pstSurface->u16Stride = stLogo.stride;

    return 0;
}

int CreateSurfaceByCanvas(const char *pszFileName, OSD_SURFACE_S *pstSurface, unsigned char *pu8Virt, unsigned int u32Width, unsigned int u32Height, unsigned int u32Stride)
{
    OSD_LOGO_T stLogo;

    stLogo.pRGBBuffer = pu8Virt;
    stLogo.width = u32Width;
    stLogo.height = u32Height;
    stLogo.stride = u32Stride;

    if (load_canvasex(pszFileName, &stLogo, pstSurface->enColorFmt) < 0)
    {
        fprintf(stderr, "load bmp error!\n");
        return -1;
    }

    pstSurface->u16Height = u32Height;
    pstSurface->u16Width = u32Width;
    pstSurface->u16Stride = u32Stride;

    return 0;
}