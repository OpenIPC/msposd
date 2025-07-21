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

static inline unsigned short convert_2bpp(
	unsigned char r, unsigned char g, unsigned char b, OSD_COMP_INFO compinfo) {
	unsigned short pixel = 0;
	unsigned int tmp = 15;

	unsigned char r1 = r >> (8 - compinfo.rlen);
	unsigned char g1 = g >> (8 - compinfo.glen);
	unsigned char b1 = b >> (8 - compinfo.blen);
	while (compinfo.alen) {
		pixel |= (1 << tmp);
		tmp--;
		compinfo.alen--;
	}

	pixel |= (r1 | (g1 << compinfo.blen) | (b1 << (compinfo.blen + compinfo.glen)));
	return pixel;
}

int parse_bitmap(
	const char *filename, OSD_BITMAPFILEHEADER *pBmpFileHeader, OSD_BITMAPINFO *pBmpInfo) {
	FILE *pFile;
	unsigned short bfType;

	if (!filename) {
		fprintf(stderr, "load_bitmap: filename=NULL\n");
		return -1;
	}

	if ((pFile = fopen(filename, "rb")) == NULL) {
		fprintf(stderr, "Open file faild:%s!\n", filename);
		return -1;
	}

	if (fread(&bfType, 1, sizeof(bfType), pFile) != sizeof(bfType)) {
		fprintf(stderr, "fread file failed:%s!\n", filename);
		fclose(pFile);
		return -1;
	}

	if (bfType != 0x4d42) {
		fprintf(stderr, "not bitmap file\n");
		fclose(pFile);
		return -1;
	}

	if (fread(pBmpFileHeader, 1, sizeof(OSD_BITMAPFILEHEADER), pFile) !=
		sizeof(OSD_BITMAPFILEHEADER)) {
		fprintf(stderr, "fread OSD_BITMAPFILEHEADER failed:%s!\n", filename);
		fclose(pFile);
		return -1;
	}

	if (fread(pBmpInfo, 1, sizeof(OSD_BITMAPINFO), pFile) != sizeof(OSD_BITMAPINFO)) {
		fprintf(stderr, "fread OSD_BITMAPINFO failed:%s!\n", filename);
		fclose(pFile);
		return -1;
	}

	if (pBmpInfo->bmiHeader.biBitCount / 8 < 2) {
		fprintf(stderr, "bitmap format not supported!\n");
		fclose(pFile);
		return -1;
	}

	if (pBmpInfo->bmiHeader.biCompression != 0 && pBmpInfo->bmiHeader.biCompression != 3) {
		fprintf(stderr, "not support compressed bitmap file!\n");
		fclose(pFile);
		return -1;
	}

	if (pBmpInfo->bmiHeader.biHeight < 0) {
		fprintf(stderr, "bmpInfo.bmiHeader.biHeight < 0\n");
		fclose(pFile);
		return -1;
	}

	fclose(pFile);
	return 0;
}

int load_bitmap(const char *filename, OSD_LOGO_T *pVideoLogo) {
	FILE *pFile;

	unsigned int w, h, stride;
	unsigned short Bpp, dstBpp;

	OSD_BITMAPFILEHEADER bmpFileHeader;
	OSD_BITMAPINFO bmpInfo;

	unsigned char *pOrigBMPBuf;
	unsigned char *pRGBBuf;

	if (parse_bitmap(filename, &bmpFileHeader, &bmpInfo) < 0)
		return -1;

	if (!(pFile = fopen(filename, "rb"))) {
		fprintf(stderr, "Open file faild:%s!\n", filename);
		return -1;
	}

	pVideoLogo->width = (unsigned short)bmpInfo.bmiHeader.biWidth;
	pVideoLogo->height =
		(unsigned short)((bmpInfo.bmiHeader.biHeight > 0) ? bmpInfo.bmiHeader.biHeight
														  : (-bmpInfo.bmiHeader.biHeight));
	w = pVideoLogo->width;
	h = pVideoLogo->height;

	Bpp = bmpInfo.bmiHeader.biBitCount / 8;
	stride = w * Bpp;
	if (stride % 4)
		stride = (stride & 0xfffc) + 4;

	/* RGB8888 or RGB1555 */
	pOrigBMPBuf = (unsigned char *)malloc(h * stride);
	if (!pOrigBMPBuf) {
		fprintf(stderr, "not enough memory to malloc!\n");
		fclose(pFile);
		return -1;
	}

	pRGBBuf = pVideoLogo->pRGBBuffer;

	if (fseek(pFile, bmpFileHeader.bfOffBits, 0)) {
		fprintf(stderr, "fseek failed!\n");
		fclose(pFile);
		free(pOrigBMPBuf);
		pOrigBMPBuf = NULL;
		return -1;
	}

	if (fread(pOrigBMPBuf, 1, (unsigned int)(h * stride), pFile) != (unsigned int)(h * stride)) {
		fprintf(stderr, "fread (%d*%d)error!line:%d\n", h, stride, __LINE__);
		perror("fread:");
	}

	if (Bpp > 2)
		dstBpp = 4;
	else
		dstBpp = 2;

	if (pVideoLogo->stride == 0)
		pVideoLogo->stride = pVideoLogo->width * dstBpp;

	for (unsigned short i = 0; i < h; i++) {
		for (unsigned short j = 0; j < w; j++) {
			memcpy(pRGBBuf + i * pVideoLogo->stride + j * dstBpp,
				pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);

			if (dstBpp == 4)
				*(pRGBBuf + i * pVideoLogo->stride + j * dstBpp + 3) = 0x80; /*alpha*/
		}
	}

	free(pOrigBMPBuf);
	pOrigBMPBuf = NULL;

	fclose(pFile);
	return 0;
}

int load_bitmapex(const char *filename, OSD_LOGO_T *pVideoLogo, OSD_COLOR_FMT_E enFmt) {
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

	if (!(pFile = fopen(filename, "rb"))) {
		fprintf(stderr, "Open file failed:%s!\n", filename);
		return -1;
	}

	pVideoLogo->width = (unsigned short)bmpInfo.bmiHeader.biWidth;
	pVideoLogo->height =
		(unsigned short)((bmpInfo.bmiHeader.biHeight > 0) ? bmpInfo.bmiHeader.biHeight
														  : (-bmpInfo.bmiHeader.biHeight));
	w = pVideoLogo->width;
	h = pVideoLogo->height;

	Bpp = bmpInfo.bmiHeader.biBitCount / 8;
	stride = w * Bpp;
	if (stride % 4)
		stride = (stride & 0xfffc) + 4;

	/* RGB8888 or RGB1555 */
	pOrigBMPBuf = (unsigned char *)malloc(h * stride);
	if (!pOrigBMPBuf) {
		fprintf(stderr, "not enough memory to malloc!\n");
		fclose(pFile);
		return -1;
	}

	pRGBBuf = pVideoLogo->pRGBBuffer;

	if (fseek(pFile, bmpFileHeader.bfOffBits, 0)) {
		fprintf(stderr, "fseek failed!\n");
		fclose(pFile);
		free(pOrigBMPBuf);
		pOrigBMPBuf = NULL;
		return -1;
	}

	if (fread(pOrigBMPBuf, 1, (unsigned int)(h * stride), pFile) != (unsigned int)(h * stride)) {
		fprintf(stderr, "fread (%d*%d)error!line:%d\n", h, stride, __LINE__);
		perror("fread:");
	}

	if (enFmt >= OSD_COLOR_FMT_RGB888)
		pVideoLogo->stride = pVideoLogo->width * 4;
	else
		pVideoLogo->stride = pVideoLogo->width * 2;

	for (unsigned short i = 0; i < h; i++) {
		for (unsigned short j = 0; j < w; j++) {
			if (Bpp == 3) /*.....*/
			{
				switch (enFmt) {
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
					memcpy(pRGBBuf + i * pVideoLogo->stride + j * 4,
						pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);
					*(pRGBBuf + i * pVideoLogo->stride + j * 4 + 3) = 0xff; /*alpha*/
					break;

				default:
					fprintf(stderr, "file(%s), line(%d), no such format!\n", __FILE__, __LINE__);
					break;
				}
			} else if ((Bpp == 2) || (Bpp == 4)) /*..............*/
			{
				memcpy(pRGBBuf + i * pVideoLogo->stride + j * Bpp,
					pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);
			}
		}
	}

	free(pOrigBMPBuf);
	pOrigBMPBuf = NULL;

	fclose(pFile);
	return 0;
}

int load_bitmap_canvas(const char *filename, OSD_LOGO_T *pVideoLogo, OSD_COLOR_FMT_E enFmt) {
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

	if (!(pFile = fopen(filename, "rb"))) {
		fprintf(stderr, "Open file faild:%s!\n", filename);
		return -1;
	}

	Bpp = bmpInfo.bmiHeader.biBitCount / 8;
	w = (unsigned short)bmpInfo.bmiHeader.biWidth;
	h = (unsigned short)((bmpInfo.bmiHeader.biHeight > 0) ? bmpInfo.bmiHeader.biHeight
														  : (-bmpInfo.bmiHeader.biHeight));

	stride = w * Bpp;
	if (stride % 4)
		stride = (stride & 0xfffc) + 4;

	/* RGB8888 or RGB1555 */
	pOrigBMPBuf = (unsigned char *)malloc(h * stride);
	if (!pOrigBMPBuf) {
		fprintf(stderr, "not enough memory to malloc!\n");
		fclose(pFile);
		return -1;
	}

	pRGBBuf = pVideoLogo->pRGBBuffer;

	if (stride > pVideoLogo->stride) {
		fprintf(stderr,
			"Bitmap's stride(%d) is bigger than canvas's stide(%d). Load "
			"bitmap error!\n",
			stride, pVideoLogo->stride);
		fclose(pFile);
		free(pOrigBMPBuf);
		return -1;
	}

	if (h > pVideoLogo->height) {
		fprintf(stderr,
			"Bitmap's height(%d) is bigger than canvas's height(%d). Load "
			"bitmap error!\n",
			h, pVideoLogo->height);
		fclose(pFile);
		free(pOrigBMPBuf);
		return -1;
	}

	if (w > pVideoLogo->width) {
		fprintf(stderr,
			"Bitmap's width(%d) is bigger than canvas's width(%d). Load bitmap "
			"error!\n",
			w, pVideoLogo->width);
		fclose(pFile);
		free(pOrigBMPBuf);
		return -1;
	}

	if (fseek(pFile, bmpFileHeader.bfOffBits, 0)) {
		fprintf(stderr, "fseek error!\n");
		fclose(pFile);
		free(pOrigBMPBuf);
		return -1;
	}

	if (fread(pOrigBMPBuf, 1, (unsigned int)(h * stride), pFile) != (unsigned int)(h * stride)) {
		fprintf(stderr, "fread (%d*%d)error!line:%d\n", h, stride, __LINE__);
		perror("fread:");
	}

	for (unsigned short i = 0; i < h; i++) {
		for (unsigned short j = 0; j < w; j++) {
			if (Bpp == 3) /*.....*/
			{
				switch (enFmt) {
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
					// fprintf(stderr, "Func: %s, line:%d, Bpp: %d, bmp stride:
					// %d, Canvas stride: %d, h:%d, w:%d.\n",
					//     __FUNCTION__, __LINE__, Bpp, stride,
					//     pVideoLogo->stride, i, j);
					*pDst = convert_2bpp(r, g, b, s_OSDCompInfo[enFmt]);

					break;

				case OSD_COLOR_FMT_RGB888:
				case OSD_COLOR_FMT_RGB8888:
					memcpy(pRGBBuf + i * pVideoLogo->stride + j * 4,
						pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);
					*(pRGBBuf + i * pVideoLogo->stride + j * 4 + 3) = 0xff; /*alpha*/
					break;

				default:
					fprintf(stderr, "file(%s), line(%d), no such format!\n", __FILE__, __LINE__);
					break;
				}
			} else if ((Bpp == 2) || (Bpp == 4)) /*..............*/
			{
				memcpy(pRGBBuf + i * pVideoLogo->stride + j * Bpp,
					pOrigBMPBuf + ((h - 1) - i) * stride + j * Bpp, Bpp);
			}
		}
	}

	free(pOrigBMPBuf);
	pOrigBMPBuf = NULL;

	fclose(pFile);
	return 0;
}

char *extract_extension(char *filename) {
	char *pret = NULL;

	if (!filename) {
		fprintf(stderr, "filename can't be null!");
		return NULL;
	}

	unsigned int fnLen = strlen(filename);
	while (fnLen) {
		pret = filename + fnLen;
		if (*pret == '.')
			return (pret + 1);
		fnLen--;
	}

	return pret;
}

int load_image(const char *filename, OSD_LOGO_T *pVideoLogo) {
	char *ext = extract_extension((char *)filename);

	if (ext && !strcmp(ext, "bmp")) {
		if (load_bitmap(filename, pVideoLogo)) {
			fprintf(stderr, "load_bitmap error!\n");
			return -1;
		}
	} else {
		fprintf(stderr, "not supported image file!\n");
		return -1;
	}

	return 0;
}

int load_imageex(const char *filename, OSD_LOGO_T *pVideoLogo, OSD_COLOR_FMT_E enFmt) {
	char *ext = extract_extension((char *)filename);

	if (ext && !strcmp(ext, "bmp")) {
		if (load_bitmapex(filename, pVideoLogo, enFmt)) {
			fprintf(stderr, "load_bitmap error!\n");
			return -1;
		}
	} else {
		fprintf(stderr, "not supported image file!\n");
		return -1;
	}

	return 0;
}

int load_canvasex(const char *filename, OSD_LOGO_T *pVideoLogo, OSD_COLOR_FMT_E enFmt) {
	char *ext = extract_extension((char *)filename);

	if (ext && !strcmp(ext, "bmp")) {
		if (load_bitmap_canvas(filename, pVideoLogo, enFmt)) {
			fprintf(stderr, "load_bitmap error!\n");
			return -1;
		}
	} else {
		fprintf(stderr, "not supported image file!\n");
		return -1;
	}

	return 0;
}

int LoadBitMap2Surface(
	const char *pszFileName, const OSD_SURFACE_S *pstSurface, unsigned char *pu8Virt) {
	OSD_LOGO_T stLogo;

	stLogo.stride = pstSurface->u16Stride;
	stLogo.pRGBBuffer = pu8Virt;

	return load_image(pszFileName, &stLogo);
}

int CreateSurfaceByBitMap(
	const char *pszFileName, OSD_SURFACE_S *pstSurface, unsigned char *pu8Virt) {
	OSD_LOGO_T stLogo;

	stLogo.pRGBBuffer = pu8Virt;

	if (load_imageex(pszFileName, &stLogo, pstSurface->enColorFmt) < 0) {
		fprintf(stderr, "load bmp error!\n");
		return -1;
	}

	pstSurface->u16Height = stLogo.height;
	pstSurface->u16Width = stLogo.width;
	pstSurface->u16Stride = stLogo.stride;

	return 0;
}

int CreateSurfaceByCanvas(const char *pszFileName, OSD_SURFACE_S *pstSurface,
	unsigned char *pu8Virt, unsigned int u32Width, unsigned int u32Height, unsigned int u32Stride) {
	OSD_LOGO_T stLogo;

	stLogo.pRGBBuffer = pu8Virt;
	stLogo.width = u32Width;
	stLogo.height = u32Height;
	stLogo.stride = u32Stride;

	if (load_canvasex(pszFileName, &stLogo, pstSurface->enColorFmt) < 0) {
		fprintf(stderr, "load bmp error!\n");
		return -1;
	}

	pstSurface->u16Height = u32Height;
	pstSurface->u16Width = u32Width;
	pstSurface->u16Stride = u32Stride;

	return 0;
}

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

// Function to calculate the squared difference between two colors in ARGB1555
// format
uint32_t colorDistance8(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2) {
	return (r1 - r2) * (r1 - r2) + (g1 - g2) * (g1 - g2) + (b1 - b2) * (b1 - b2);
}

uint8_t findClosestPaletteIndex(uint16_t color, uint16_t *palette) {
	uint32_t minDistance = 65535;
	uint8_t bestIndex = 0;

	for (uint8_t i = 0; i < 16 /*PALETTE_SIZE*/; ++i) {
		uint32_t distance = colorDistance(color, palette[i]);
		if (distance < minDistance) {
			minDistance = distance;
			bestIndex = i;
		}
	}

	return bestIndex;
}

// Function to find the closest color in the palette
uint8_t findClosestPaletteIndexBW(uint16_t color, MI_RGN_PaletteTable_t *paletteTable) {
	uint32_t minDistance = 65535;
	uint8_t bestIndex = 2;

	uint8_t a = (color >> 15) & 0x01; // 1-bit alpha
	if (a == 0) {
		bestIndex = 15; // transparent
		return bestIndex;
	}

	// Extract RGB components from ARGB1555 color
	uint8_t r = ((color >> 10) & 0x1F) << 3; // Convert 5-bit to 8-bit
	uint8_t g = ((color >> 5) & 0x1F) << 3;	 // Convert 5-bit to 8-bit
	uint8_t b = (color & 0x1F) << 3;		 // Convert 5-bit to 8-bit

	for (uint8_t i = 7; i < 18; ++i) { // only 5 searched W, Bl, 2xGrays , transp
		if (i == 9)
			i = 13;
		MI_RGN_PaletteElement_t *element = &paletteTable->astElement[i];
		uint32_t distance =
			colorDistance8(r, g, b, element->u8Red, element->u8Green, element->u8Blue);
		if (distance < minDistance) {
			minDistance = distance;
			bestIndex = i;
		}
	}

	return bestIndex;
}

// Function to find the closest color in the palette
uint8_t findClosestPaletteIndex8(uint16_t color, MI_RGN_PaletteTable_t *paletteTable) {
	uint32_t minDistance = 65535;
	uint8_t bestIndex = 0;

	uint8_t a = (color >> 15) & 0x01; // 1-bit alpha
	if (a == 0) {
		bestIndex = 15; // transparent
		return bestIndex;
	}

	// Extract RGB components from ARGB1555 color
	uint8_t r = ((color >> 10) & 0x1F) << 3; // Convert 5-bit to 8-bit
	uint8_t g = ((color >> 5) & 0x1F) << 3;	 // Convert 5-bit to 8-bit
	uint8_t b = (color & 0x1F) << 3;		 // Convert 5-bit to 8-bit

	for (uint8_t i = 1; i < 18; ++i) { // only 16 searched

		MI_RGN_PaletteElement_t *element = &paletteTable->astElement[i];
		uint32_t distance =
			colorDistance8(r, g, b, element->u8Red, element->u8Green, element->u8Blue);
		if (distance < minDistance) {
			minDistance = distance;
			bestIndex = i;
		}
	}

	return bestIndex;
}

uint8_t findClosestPaletteIndexRGBA(uint8_t *color, MI_RGN_PaletteTable_t *paletteTable) {
	uint32_t minDistance = 65535;
	uint8_t bestIndex = 0;

	// Extract RGB components from ARGB1555 color
	// Extract RGBA values
	uint8_t r = *(color + 0);
	uint8_t g = *(color + 1);
	uint8_t b = *(color + 2);
	uint8_t a = *(color + 3);
	if (a < 64)	   // transparency treshold, was 128
		return 15; // transparent
	else if (r > 77)
		bestIndex = 0;
	else
		bestIndex = 0;

	if (a > 2 && a < 200)
		a++;

	for (uint8_t i = 1; i < 18; ++i) { // only 16 searched
		MI_RGN_PaletteElement_t *element = &paletteTable->astElement[i];
		uint32_t distance =
			colorDistance8(r, g, b, element->u8Red, element->u8Green, element->u8Blue);
		if (distance < minDistance) {
			minDistance = distance;
			bestIndex = i;
		}
	}

	return bestIndex;
}

void convertBitmap1555ToI8(uint16_t *srcBitmap, uint32_t width, uint32_t height,
	uint8_t *destBitmap, MI_RGN_PaletteTable_t *paletteTable) {
	uint32_t numPixels = width * height;

	for (uint32_t i = 0; i < numPixels; ++i) {
		// Find the closest palette index for each ARGB1555 pixel
		uint8_t paletteIndex = findClosestPaletteIndex8(srcBitmap[i], paletteTable);

		if (paletteIndex == 0)	// Sigmastar reserver
			paletteIndex = 8;	// black
		if (paletteIndex == 17) // Transparent color
			paletteIndex = 15;
		// Store the palette index in the I8 bitmap
		destBitmap[i] = paletteIndex;
	}
}

uint16_t ConvertARGB8888ToARGB1555(MI_RGN_PaletteElement_t color) {
    uint16_t a = (color.u8Alpha >= 128) ? 1 : 0;  // 1-bit alpha
    uint16_t r = (color.u8Red >> 3) & 0x1F;
    uint16_t g = (color.u8Green >> 3) & 0x1F;
    uint16_t b = (color.u8Blue >> 3) & 0x1F;

    return (a << 15) | (r << 10) | (g << 5) | b;
}

uint16_t GetARGB1555From_RGN_Palette(int index) {
    if (index < 0 || index >= 256)
        return 0;  // Return black or error fallback

    MI_RGN_PaletteElement_t color = g_stPaletteTable.astElement[index];

    uint16_t a = (color.u8Alpha >= 128) ? 1 : 0;  // 1-bit alpha
    uint16_t r = (color.u8Red >> 3) & 0x1F;
    uint16_t g = (color.u8Green >> 3) & 0x1F;
    uint16_t b = (color.u8Blue >> 3) & 0x1F;

    return (a << 15) | (r << 10) | (g << 5) | b;
}

// static MI_RGN_PaletteTable_t g_stPaletteTable ;//= {{{0, 0, 0, 0}}};
MI_RGN_PaletteTable_t g_stPaletteTable = {{// index0 ~ index15
	{255, 0, 0, 0},						   // reserved

	{0xFF, 0xFF, 0x00, 0x00}, // 0x7C00 -> Red
	{0xFF, 0x00, 0xFF, 0x00}, // 0x03E0 -> Green
	{0xFF, 0x00, 0x00, 0xFF}, // 0x001F -> Blue
	{0xFF, 0xF8, 0xF8, 0x00}, // 0x7FE0 -> Yellow
	{0xFF, 0xF8, 0x00, 0xF8}, // 0x7C1F -> Magenta
	{0xFF, 0x00, 0xF8, 0xF8}, // 0x03FF -> Cyan
	{0xFF, 0xFF, 0xFF, 0xFF}, // 0x7FFF -> White
	{0xFF, 0x00, 0x00, 0x00}, // 0x0000 -> Black  index 8
	{0x6F, 0x00, 0x00, 0x00}, // Semi transparent
	{0xFF, 0xA2, 0x08, 0x08}, // Dark Red
	{0xFF, 0x63, 0x18, 0xC6}, //
	{0xFF, 0xAD, 0x52, 0xD6}, // Orchid
	{0xFF, 0xCC, 0xCC, 0xCC}, // 0x739C -> Gray (Light) {0xFF, 0xCC, 0xCC, 0xCC}
	{0xFF, 0x77, 0x77, 0x77}, // 0x18C6 -> Gray (Dark)
	{0x00, 0, 0, 0},		  // transparent  index 15, 0x0A
	{0x00, 0, 0, 0},		  // 0x7BDE -> transparent
	// index17 ~ index31
	{0xFF, 0xF0, 0xF0, 0xF0}, // this is the predefined TRANSARANT Color index17
	{0, 0, 255, 60}, {0, 128, 0, 90}, {255, 0, 0, 120}, {0, 255, 255, 150}, {255, 255, 0, 180},
	{0, 255, 0, 210}, {255, 0, 255, 240}, {192, 192, 192, 255}, {128, 128, 128, 10}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index32 ~ index47
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index48 ~ index63
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index64 ~ index79
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index80 ~ index95
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index96 ~ index111
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index112 ~ index127
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index128 ~ index143
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index144 ~ index159
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index160 ~ index175
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index176 ~ index191
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index192 ~ index207
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index208 ~ index223
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index224 ~ index239
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// (index236 :192,160,224 defalut colorkey)
	{192, 160, 224, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	// index240 ~ index255
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {192, 160, 224, 255}}};

void Convert1555ToRGBA(
	unsigned short *bitmap1555, unsigned char *rgbaData, unsigned int width, unsigned int height) {
	for (unsigned int i = 0; i < width * height; ++i) {
		unsigned short pixel1555 = bitmap1555[i];

		// Extract components
		unsigned char alpha =
			(pixel1555 & 0x8000) ? 255 : 0;				// 1-bit alpha, 0x8000 is the alpha bit mask
		unsigned char red = (pixel1555 & 0x7C00) >> 10; // 5-bit red, shift right to align
		unsigned char green = (pixel1555 & 0x03E0) >> 5; // 5-bit green
		unsigned char blue = (pixel1555 & 0x001F);		 // 5-bit blue

		// Scale 5-bit colors to 8-bit
		red = (red << 3) | (red >> 2);		 // Scale from 5-bit to 8-bit
		green = (green << 3) | (green >> 2); // Scale from 5-bit to 8-bit
		blue = (blue << 3) | (blue >> 2);	 // Scale from 5-bit to 8-bit

		// Combine into RGBA
		rgbaData[i * 4 + 0] = red;
		rgbaData[i * 4 + 1] = green;
		rgbaData[i * 4 + 2] = blue;
		rgbaData[i * 4 + 3] = alpha;
	}
}

/*
void copyRectARGB1555Slow(
	uint16_t* srcBitmap, uint32_t srcWidth, uint32_t srcHeight,
	uint16_t* destBitmap, uint32_t destWidth, uint32_t destHeight,
	uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height,
	uint32_t destX, uint32_t destY)
{
	// Bounds checking
	if (srcX + width > srcWidth || srcY + height > srcHeight ||
		destX + width > destWidth || destY + height > destHeight){
		// Handle error: the rectangle is out of bounds
		printf("Error copyRectARGB1555 to %d : %d\r\n", destX, destY);
		return;
	}

	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			// Calculate the source and destination indices
			uint32_t srcIndex = (srcY + y) * srcWidth + (srcX + x);
			uint32_t destIndex = (destY + y) * destWidth + (destX + x);

			//if (srcBitmap[srcIndex]==TRANSPARENT_COLOR)
			 //   srcBitmap[srcIndex]=0x7FFF;
			// Copy the pixel
			destBitmap[destIndex] = srcBitmap[srcIndex];
		}
	}
}
*/
#include <stdint.h>
#include <stdio.h>
#include <string.h> // for memcpy

void copyRectARGB1555(uint16_t *srcBitmap, uint32_t srcWidth, uint32_t srcHeight,
	uint16_t *destBitmap, uint32_t destWidth, uint32_t destHeight, uint32_t srcX, uint32_t srcY,
	uint32_t width, uint32_t height, uint32_t destX, uint32_t destY) {
	// Bounds checking
	if (srcX + width > srcWidth || srcY + height > srcHeight || destX + width > destWidth ||
		destY + height > destHeight) {
		// Handle error: the rectangle is out of bounds
		printf("Error copyRectARGB1555 to %d : %d\r\n", destX, destY);
		return;
	}

	// Calculate the width of the rectangle in bytes
	uint32_t rowSizeInBytes = width * sizeof(uint16_t);
	rowSizeInBytes = getRowStride(width, 16);

	for (uint32_t y = 0; y < height; ++y) {
		// Calculate the source and destination pointers for the current row
		uint16_t *srcPtr = srcBitmap + (srcY + y) * srcWidth + srcX;
		uint16_t *destPtr = destBitmap + (destY + y) * destWidth + destX;

		// Use memcpy to copy the entire row
		memcpy(destPtr, srcPtr, rowSizeInBytes);
	}
}

void copyRectRGBA8888(uint32_t *srcBitmap, uint32_t srcWidth, uint32_t srcHeight,
	uint32_t *destBitmap, uint32_t destWidth, uint32_t destHeight, uint32_t srcX, uint32_t srcY,
	uint32_t width, uint32_t height, uint32_t destX, uint32_t destY) {
	// Bounds checking
	if (srcX + width > srcWidth || srcY + height > srcHeight || destX + width > destWidth ||
		destY + height > destHeight) {
		// Handle error: the rectangle is out of bounds
		printf("Error copyRectRGBA8888 to %d : %d\n", destX, destY);
		return;
	}

	// Calculate the width of the rectangle in bytes (4 bytes per pixel for
	// RGBA8888)
	uint32_t rowSizeInBytes = width * sizeof(uint32_t);
	rowSizeInBytes = getRowStride(width, 32); // 32 bits per pixel for RGBA8888

	for (uint32_t y = 0; y < height; ++y) {
		// Calculate the source and destination pointers for the current row
		uint32_t *srcPtr = srcBitmap + (srcY + y) * srcWidth + srcX;
		uint32_t *destPtr = destBitmap + (destY + y) * destWidth + destX;

		// Use memcpy to copy the entire row
		memcpy(destPtr, srcPtr, rowSizeInBytes);
	}
}

/*
void copyRectI8Slow(
	uint8_t* srcBitmap, uint32_t srcWidth, uint32_t srcHeight,
	uint8_t* destBitmap, uint32_t destWidth, uint32_t destHeight,
	uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height,
	uint32_t destX, uint32_t destY)
{
	// Bounds checking
	if (srcX + width > srcWidth || srcY + height > srcHeight ||
		destX + width > destWidth || destY + height > destHeight){
		// Handle error: the rectangle is out of bounds
		printf("Error copyRectARGB1555 to %d : %d\r\n", destX, destY);
		return;
	}

	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			// Calculate the source and destination indices
			uint32_t srcIndex = (srcY + y) * srcWidth + (srcX + x);
			uint32_t destIndex = (destY + y) * destWidth + (destX + x);

			//if (srcBitmap[srcIndex]==TRANSPARENT_COLOR)
			 //   srcBitmap[srcIndex]=0x7FFF;
			// Copy the pixel
			destBitmap[destIndex] = srcBitmap[srcIndex];
		}
	}
}
*/

void copyRectI8(uint8_t *srcBitmap, uint32_t srcWidth, uint32_t srcHeight, uint8_t *destBitmap,
	uint32_t destWidth, uint32_t destHeight, uint32_t srcX, uint32_t srcY, uint32_t width,
	uint32_t height, uint32_t destX, uint32_t destY) {
	// Bounds checking
	if (srcX + width > srcWidth || srcY + height > srcHeight || destX + width > destWidth ||
		destY + height > destHeight) {
		// Handle error: the rectangle is out of bounds
		printf("Error copyRectI8 to %d : %d\r\n", destX, destY);
		return;
	}

	// Calculate the starting indices once
	uint32_t srcStartIndex = srcY * srcWidth + srcX;
	uint32_t destStartIndex = destY * destWidth + destX;

	// Copy each row in one operation using memcpy
	for (uint32_t y = 0; y < height; ++y) {
		memcpy(&destBitmap[destStartIndex + y * destWidth],
			&srcBitmap[srcStartIndex + y * srcWidth], width);
	}
}

#include <stdint.h>
#include <stdio.h>
#include <string.h> // For memcpy

void copyRectI4(uint8_t *srcBitmap, uint32_t srcWidth, uint32_t srcHeight, uint8_t *destBitmap,
	uint32_t destWidth, uint32_t destHeight, uint32_t srcX, uint32_t srcY, uint32_t width,
	uint32_t height, uint32_t destX, uint32_t destY) {
	// Bounds checking
	if (srcX + width > srcWidth || srcY + height > srcHeight || destX + width > destWidth ||
		destY + height > destHeight) {
		// Handle error: the rectangle is out of bounds
		printf("Error copyRectI4 from %d:%d  to %d:%d  srcWidth:%d, srcHeight:%d, "
			   "destWidth:%d, destHeight:%d \r\n",
			srcX, srcY, destX, destY, srcWidth, srcHeight, destWidth, destHeight);
		return;
	}

	// Determine byte and pixel offsets
	uint32_t srcByteOffset = srcX / 2;
	uint32_t destByteOffset = destX / 2;
	uint32_t srcPixelOffset = srcX % 2;
	uint32_t destPixelOffset = destX % 2;

	// Fast path: both srcX and destX align on byte boundaries
	if (srcPixelOffset == 0 && destPixelOffset == 0 && width % 2 == 0) {
		uint32_t byteWidth = width / 2; // Number of bytes to copy per row
		for (uint32_t y = 0; y < height; ++y) {
			uint8_t *srcRow = srcBitmap + (srcY + y) * (srcWidth / 2) + srcByteOffset;
			uint8_t *destRow = destBitmap + (destY + y) * (destWidth / 2) + destByteOffset;
			memcpy(destRow, srcRow, byteWidth);
		}
	} else {
		// Handle cases where srcX or destX are not byte-aligned
		for (uint32_t y = 0; y < height; ++y) {
			for (uint32_t x = 0; x < width; ++x) {
				uint32_t srcIndex = (srcY + y) * (srcWidth / 2) + (srcByteOffset + (x / 2));
				uint32_t destIndex = (destY + y) * (destWidth / 2) + (destByteOffset + (x / 2));

				uint8_t srcPixel;
				if ((srcPixelOffset + x) % 2 == 0) {
					srcPixel = (srcBitmap[srcIndex] & 0xF0) >> 4; // High nibble
				} else {
					srcPixel = srcBitmap[srcIndex] & 0x0F; // Low nibble
				}

				if ((destPixelOffset + x) % 2 == 0) {
					// Write to high nibble
					destBitmap[destIndex] = (destBitmap[destIndex] & 0x0F) | (srcPixel << 4);
				} else {
					// Write to low nibble
					destBitmap[destIndex] = (destBitmap[destIndex] & 0xF0) | srcPixel;
				}
			}
		}
	}
}

void ConvertI8ToRGBA(uint8_t *bitmapI8, uint8_t *rgbaData, uint32_t width, uint32_t height,
	MI_RGN_PaletteElement_t *palette) {
	for (uint32_t i = 0; i < width * height; ++i) {
		uint8_t index = bitmapI8[i];

		// Get RGBA color from the palette
		MI_RGN_PaletteElement_t color = palette[index];

		// Assign to RGBA data
		rgbaData[i * 4 + 0] = color.u8Red;
		rgbaData[i * 4 + 1] = color.u8Green;
		rgbaData[i * 4 + 2] = color.u8Blue;
		rgbaData[i * 4 + 3] = color.u8Alpha;
	}
}

void ConvertI4ToRGBA(uint8_t *bitmapI4, uint8_t *rgbaData, uint32_t width, uint32_t height,
	MI_RGN_PaletteElement_t *palette) {
	uint32_t pixelIndex = 0;
	int RowLength = getRowStride(width, 4); // the length of one row, it is rounded up to 4 bytes
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; x += 2) {
			// Each byte in the I4 bitmap contains two pixels
			// uint8_t byte = bitmapI4[(y * (width + 1) / 2) + (x / 2)];

			uint8_t byte = bitmapI4[y * RowLength + (x / 2)];

			// Extract the first (high) pixel
			uint8_t index1 = (byte & 0xF0) >> 4;
			MI_RGN_PaletteElement_t color1 = palette[index1];

     //Cairo expects image surfaces to be in premultiplied ARGB32 format on little-endian systems, 
	 //which in memory layout becomes: memory layout per pixel:  [Blue, Green, Red, Alpha]  (BGRA)
			rgbaData[pixelIndex * 4 + 2] = color1.u8Red;
			rgbaData[pixelIndex * 4 + 1] = color1.u8Green;
			rgbaData[pixelIndex * 4 + 0] = color1.u8Blue;
			rgbaData[pixelIndex * 4 + 3] = color1.u8Alpha;
			pixelIndex++;

			if (x + 1 < width) {
				// Extract the second (low) pixel
				uint8_t index2 = byte & 0x0F;
				MI_RGN_PaletteElement_t color2 = palette[index2];
				rgbaData[pixelIndex * 4 + 2] = color2.u8Red;
				rgbaData[pixelIndex * 4 + 1] = color2.u8Green;
				rgbaData[pixelIndex * 4 + 0] = color2.u8Blue;
				rgbaData[pixelIndex * 4 + 3] = color2.u8Alpha;
				pixelIndex++;
			}
		}
	}
}

void ConvertI4ToRGBA2(uint8_t *bitmapI4, uint8_t *rgbaData, uint32_t width, uint32_t height,
	MI_RGN_PaletteElement_t *palette) {
	uint32_t pixelIndex = 0;
	int RowLength = getRowStride(width, 4); // The length of one row, rounded up to 4 bytes

	for (uint32_t y = 0; y < height; ++y) {
		// Calculate the starting position of the current row
		uint32_t rowStart = y * RowLength;

		for (uint32_t x = 0; x < width; x += 2) {
			// Each byte in the I4 bitmap contains two pixels
			uint8_t byte = bitmapI4[rowStart + (x / 2)];

			// Extract the first (high) pixel
			uint8_t index1 = (byte & 0xF0) >> 4;
			MI_RGN_PaletteElement_t color1 = palette[index1];
			rgbaData[pixelIndex * 4 + 0] = color1.u8Red;
			rgbaData[pixelIndex * 4 + 1] = color1.u8Green;
			rgbaData[pixelIndex * 4 + 2] = color1.u8Blue;
			rgbaData[pixelIndex * 4 + 3] = color1.u8Alpha;
			pixelIndex++;

			if (x + 1 < width) {
				// Extract the second (low) pixel
				uint8_t index2 = byte & 0x0F;
				MI_RGN_PaletteElement_t color2 = palette[index2];
				rgbaData[pixelIndex * 4 + 0] = color2.u8Red;
				rgbaData[pixelIndex * 4 + 1] = color2.u8Green;
				rgbaData[pixelIndex * 4 + 2] = color2.u8Blue;
				rgbaData[pixelIndex * 4 + 3] = color2.u8Alpha;
				pixelIndex++;
			}
		}
	}
}

static uint32_t black_cntr = 0;

void convertBitmap1555ToI4x86(uint16_t *srcBitmap, uint32_t width, uint32_t height,
	uint8_t *destBitmap, MI_RGN_PaletteTable_t *paletteTable) {
	// Calculate the number of bytes required per line without padding
	uint32_t bytesPerLine = getRowStride(width, 4); // (width + 1) / 2;  // +1 to handle odd width

	for (uint32_t y = 0; y < height; ++y) {
		// Add a 4-bit offset for each row
		// No Fucking idea why this is different for x86 and Sigmastar
		// Maybe the routines that copy the BMP later are incorrect, no time to
		// investigate
		uint32_t destOffset = y * bytesPerLine; //+ (y / 2);  // Adjusted to add
												// 4-bit offset for each row

		for (uint32_t x = 0; x < width; ++x) {
			uint32_t srcIndex = y * width + x; // Calculate the source index
			uint8_t paletteIndex = findClosestPaletteIndex8(srcBitmap[srcIndex], paletteTable);

			// Clamp paletteIndex within the valid range for I4 (0 to 15)

			if (paletteIndex == 0)	// Sigmastar reserver
				paletteIndex = 8;	// black
			if (paletteIndex == 17) // Transparent color
				paletteIndex = 15;

			// Calculate the destination byte and bit position
			uint32_t destByteIndex = destOffset + x / 2;
			if (x % 2 == 0) {
				// Even index: Store in the upper 4 bits
				destBitmap[destByteIndex] = (paletteIndex << 4);
			} else {
				// Odd index: Store in the lower 4 bits
				destBitmap[destByteIndex] |= paletteIndex;
			}
		}
	}
}

typedef unsigned char MI_U8;

int getRowStride(int width, int BitsPerPixel) {
	int rowLength = width * BitsPerPixel;
	int stride = (rowLength + 32 - ((rowLength - 1) % 32)) >> 3;

#ifdef __SIGMASTAR__
	// This is fundamential difference for sigmastar, since the BMP we get for
	// the font is aligned with a different stride?! stride = (stride + 7) &
	// ~7;//Round up to 8
#endif
	return stride;
}

void convertBitmap1555ToI4(uint16_t *srcBitmap, uint32_t width, uint32_t height,
	uint8_t *destBitmap, int singleColor, int colourBackground) {
	MI_RGN_PaletteTable_t *paletteTable = &g_stPaletteTable;
	// Calculate the number of bytes required per line without padding

	//if (singleColor == -1) // The color that we assume as transparent
	//	singleColor = 15;

	unsigned char u8Value = 0;
	uint32_t u32Stride = (width + 1) / 2;

	int SourceStride = getRowStride(width, 16);

	int u32Stride2 = getRowStride(width, 4); //  rowLength + 32 - ((rowLength-1) % 32)) >> 3;

	// printf("I4 %d:%d Stride1:%d  Stride2:%d \n",width,height,u32Stride ,
	// u32Stride2 );

	u32Stride = u32Stride2;

	for (uint32_t u32Y = 0; u32Y < height; ++u32Y) {

		for (int32_t u32X = 0; u32X < width; ++u32X) {
			uint32_t srcIndex =
				u32Y * width + u32X; // Calculate the source index, but what if it is odd ?
									 //  srcIndex = u32Y*SourceStride/2 + u32X;
			uint8_t paletteIndex = 15;
			if (singleColor >= 0 && singleColor < 15)
				paletteIndex = findClosestPaletteIndexBW(srcBitmap[srcIndex], paletteTable);
			else
				paletteIndex = findClosestPaletteIndex8(srcBitmap[srcIndex], paletteTable);

			if (paletteIndex == 0) // Sigmastar reserved, just in case
				paletteIndex = 8;  // black

			// convert to black and white
			if (singleColor>=0 && paletteIndex != 15 && paletteIndex >= 0) {
				paletteIndex = singleColor;
			}
			if (colourBackground >= 0 && paletteIndex == 15)
				paletteIndex = colourBackground;

				// No Fucking idea why this is different for x86 and Sigmastar, BUT
				// !!! SigmaStar I4 format needs it reversit 4bit pairs.   0x0A,
				// 0x0B needs to be 0XBA
#ifdef __SIGMASTAR__
			if (u32X % 2) { // this is the secret of distorted image !!!
#else
			if (u32X % 2 == 0) {
#endif
				u8Value = (*((MI_U8 *)destBitmap + (u32Stride * u32Y) + u32X / 2) & 0x0F) |
						  ((paletteIndex & 0x0f) << 4);
				*((MI_U8 *)destBitmap + (u32Stride * u32Y) + u32X / 2) = u8Value;
			} else {
				u8Value = (*((MI_U8 *)destBitmap + (u32Stride * u32Y) + u32X / 2) & 0xF0) |
						  (paletteIndex & 0x0f);
				*((MI_U8 *)destBitmap + (u32Stride * u32Y) + u32X / 2) = u8Value;
			}
		}
	}
}

void setPixelI4(
	uint8_t *bmpData, uint32_t width, uint32_t x, uint32_t y, uint8_t color, uint32_t rowStride) {

	// Calculate the byte index for the pixel
	uint32_t byteIndex = y * rowStride + (x / 2);

	// Determine if it's the high nibble or low nibble
#ifdef __SIGMASTAR__
	if (x % 2 == 1) {
#else
	if (x % 2 == 0) {
#endif
		// High nibble (first pixel in the byte)
		bmpData[byteIndex] = (bmpData[byteIndex] & 0x0F) | (color << 4);
	} else {
		// Low nibble (second pixel in the byte)
		bmpData[byteIndex] = (bmpData[byteIndex] & 0xF0) | (color & 0x0F);
	}
}

// Assuming MI_RGN_PaletteTable_t and findClosestPaletteIndex8 are defined
// elsewhere
void convertRGBAToI4(uint8_t *srcBitmap, uint32_t width, uint32_t height, uint8_t *destBitmap,
	MI_RGN_PaletteTable_t *paletteTable) {
	unsigned char u8Value = 0;
	int u32Stride = getRowStride(width, 4);

	printf("I4 %d:%d Stride:%d\n", width, height, u32Stride);

	for (uint32_t u32Y = 0; u32Y < height; ++u32Y) {
		for (int32_t u32X = 0; u32X < width; ++u32X) {
			uint32_t srcIndex =
				(u32Y * width + u32X) * 4; // Calculate the source index (4 bytes per pixel)

			// Extract RGBA values
			uint8_t r = srcBitmap[srcIndex + 0];
			uint8_t g = srcBitmap[srcIndex + 1];
			uint8_t b = srcBitmap[srcIndex + 2];
			uint8_t a = srcBitmap[srcIndex + 3];

			if (a != 0 && r > 0)
				u8Value = 0;

			// Find the closest palette index for the RGBA color
			// uint8_t paletteIndex = findClosestPaletteIndex8(r, g, b, a,
			// paletteTable);
			uint8_t paletteIndex = findClosestPaletteIndexRGBA(&srcBitmap[srcIndex], paletteTable);

			// Handle specific palette indices as per SigmaStar requirements
			//           if (paletteIndex == 0) // Sigmastar reserved
			//                paletteIndex = 8;  // Black
			if (paletteIndex == 17) // Transparent color
				paletteIndex = 15;

			// Handle SigmaStar-specific bit manipulation
			/*
	#ifdef __SIGMASTAR__
			if (u32X % 2) {   // SigmaStar I4 format needs reversed 4-bit pairs
	#else
			if (u32X % 2 == 0) { // Other platforms (non-SigmaStar)
	#endif
				u8Value = (*(destBitmap + (u32Stride * u32Y) + u32X / 2) & 0x0F)
	|
	((paletteIndex & 0x0F) << 4);
				*(destBitmap + (u32Stride * u32Y) + u32X / 2) = u8Value;
			} else {
				u8Value = (*(destBitmap + (u32Stride * u32Y) + u32X / 2) & 0xF0)
	| (paletteIndex & 0x0F);
				*(destBitmap + (u32Stride * u32Y) + u32X / 2) = u8Value;
			}
			*/
			setPixelI4(destBitmap, width, u32X, u32Y, paletteIndex, u32Stride);
		}
	}
}

uint16_t Transform_OVERLAY_WIDTH;
uint16_t Transform_OVERLAY_HEIGHT;
float Transform_Roll;
float Transform_Pitch;

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

void ApplyTransform(int posX0, int posY0, uint32_t *posX_R, uint32_t *posY_R) {
	uint32_t width = Transform_OVERLAY_WIDTH;
	uint32_t height = Transform_OVERLAY_HEIGHT;
	// Apply Transform
	int OffsY = sin((Transform_Pitch) * (M_PI / 180.0)) * 400;
	Point img_center = {
		Transform_OVERLAY_WIDTH / 2, Transform_OVERLAY_HEIGHT / 2}; // Center of the image
	// Define the four corners of the rectangle before rotation
	Point A = {posX0, posY0 - OffsY};
	// Rotate each corner around the center
	Point rotated_A;
	rotate_point(A, img_center, Transform_Roll, &rotated_A);
	*posX_R = rotated_A.x;
	*posY_R = rotated_A.y;
}

void drawLine(
	uint8_t *bmpData, int posX0, int posY0, int posX1, int posY1, uint8_t color, int thickness) {

	uint32_t width = Transform_OVERLAY_WIDTH;
	uint32_t height = Transform_OVERLAY_HEIGHT;
	// Apply Transform
	int OffsY = sin((Transform_Pitch) * (M_PI / 180.0)) * 400;
	Point img_center = {
		Transform_OVERLAY_WIDTH / 2, Transform_OVERLAY_HEIGHT / 2}; // Center of the image

	// Define the four corners of the rectangle before rotation
	Point A = {posX0, posY0 - OffsY};
	Point B = {posX1, posY1 - OffsY};

	// Rotate each corner around the center
	Point rotated_A, rotated_B;
	rotate_point(A, img_center, Transform_Roll, &rotated_A);
	rotate_point(B, img_center, Transform_Roll, &rotated_B);

	drawLineI4(bmpData, width, height, rotated_A.x, rotated_A.y, rotated_B.x, rotated_B.y, color,
		thickness); // Right side

	return;
}

void drawThickLineI4(uint8_t *bmpData, uint32_t width, uint32_t height, int x0, int y0, int x1,
	int y1, uint8_t color, int thickness) {
	for (int i = -thickness / 2; i <= thickness / 2; i++) {
		for (int j = -thickness / 2; j <= thickness / 2; j++) {
			drawLineI4(bmpData, width, height, x0 + i, y0 + j, x1 + i, y1 + j, color, 1);
		}
	}
}

// Helper function to calculate bounding box
void getBoundingBox(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, int *minX,
	int *minY, int *maxX, int *maxY) {
	*minX = x0 < x1 ? (x0 < x2 ? (x0 < x3 ? x0 : x3) : (x2 < x3 ? x2 : x3))
					: (x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3));
	*minY = y0 < y1 ? (y0 < y2 ? (y0 < y3 ? y0 : y3) : (y2 < y3 ? y2 : y3))
					: (y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3));
	*maxX = x0 > x1 ? (x0 > x2 ? (x0 > x3 ? x0 : x3) : (x2 > x3 ? x2 : x3))
					: (x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3));
	*maxY = y0 > y1 ? (y0 > y2 ? (y0 > y3 ? y0 : y3) : (y2 > y3 ? y2 : y3))
					: (y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3));
}

// Function to check if a point is inside the region using the winding number
// algorithm
int isPointInPolygon(int x, int y, int *vx, int *vy) {
	int windingNumber = 0;

	for (int i = 0; i < 4; i++) {
		int x0 = vx[i];
		int y0 = vy[i];
		int x1 = vx[(i + 1) % 4];
		int y1 = vy[(i + 1) % 4];

		if (y0 <= y) {
			if (y1 > y && (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0) > 0) {
				windingNumber++;
			}
		} else {
			if (y1 <= y && (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0) < 0) {
				windingNumber--;
			}
		}
	}

	return windingNumber != 0;
}

// Function to fill the region in the BMP
void fillRegionI4(uint8_t *bmpData, uint32_t width, uint32_t height, int x0, int y0, int x1, int y1,
	int x2, int y2, int x3, int y3, uint8_t color) {
	int minX, minY, maxX, maxY;
	getBoundingBox(x0, y0, x1, y1, x2, y2, x3, y3, &minX, &minY, &maxX, &maxY);

	int vx[4] = {x0, x1, x2, x3};
	int vy[4] = {y0, y1, y2, y3};

	uint16_t rowStride = getRowStride(width, 4);

	// Iterate through each pixel in the bounding box
	for (int y = minY; y <= maxY; y++) {
		for (int x = minX; x <= maxX; x++) {
			if (isPointInPolygon(x, y, vx, vy)) {
				if (x >= 0 && x < width && y >= 0 && y < height) {
					uint32_t byteIndex = y * rowStride + (x / 2);
					if (x % 2 == 0) {
						bmpData[byteIndex] = (bmpData[byteIndex] & 0x0F) | (color << 4);
					} else {
						bmpData[byteIndex] = (bmpData[byteIndex] & 0xF0) | (color & 0x0F);
					}
				}
			}
		}
	}
}

void drawRectangleI4(uint8_t *bmpData, int posX, int posY, int rectWidth, int rectHeight,
	uint8_t color, int thickness) {

	uint32_t width = Transform_OVERLAY_WIDTH;
	uint32_t height = Transform_OVERLAY_HEIGHT;

	// Apply Transform
	int OffsY = sin((Transform_Pitch) * (M_PI / 180.0)) * 400;
	Point img_center = {
		Transform_OVERLAY_WIDTH / 2, Transform_OVERLAY_HEIGHT / 2}; // Center of the image

	// Define the four corners of the rectangle before rotation
	Point A = {posX, posY - OffsY};
	Point B = {posX + rectWidth, posY - OffsY};
	Point C = {posX + rectWidth, posY + rectHeight - OffsY};
	Point D = {posX, posY + rectHeight - OffsY};

	// Rotate each corner around the center
	Point rotated_A, rotated_B, rotated_C, rotated_D;
	rotate_point(A, img_center, Transform_Roll, &rotated_A);
	rotate_point(B, img_center, Transform_Roll, &rotated_B);
	rotate_point(C, img_center, Transform_Roll, &rotated_C);
	rotate_point(D, img_center, Transform_Roll, &rotated_D);

	// Old way
	if (rectHeight == 1) {
		if (color == COLOR_WHITE)
			drawLineI4AA(bmpData, width, height, rotated_A.x, rotated_A.y, rotated_B.x,
				rotated_B.y); // Top side
		else
			drawLineI4(bmpData, width, height, rotated_A.x, rotated_A.y, rotated_B.x, rotated_B.y,
				color,
				1); // Top side
		return;
	}
	if (rectWidth == 1) {
		if (color == COLOR_WHITE)
			drawLineI4AA(bmpData, width, height, rotated_B.x, rotated_B.y, rotated_C.x,
				rotated_C.y); // Right side
		else
			drawLineI4(bmpData, width, height, rotated_B.x, rotated_B.y, rotated_C.x, rotated_C.y,
				color,
				1); // Right side
		return;
	}

	// Draw the four sides of the rectangle
	if (thickness == 1) {
		if (color == COLOR_WHITE) {
			drawLineI4AA(bmpData, width, height, rotated_A.x, rotated_A.y, rotated_B.x,
				rotated_B.y); // Top side
			drawLineI4AA(bmpData, width, height, rotated_B.x, rotated_B.y, rotated_C.x,
				rotated_C.y); // Right side
			drawLineI4AA(bmpData, width, height, rotated_C.x, rotated_C.y, rotated_D.x,
				rotated_D.y); // Bottom side
			drawLineI4AA(bmpData, width, height, rotated_D.x, rotated_D.y, rotated_A.x,
				rotated_A.y); // Left side
		} else {
			drawLineI4(bmpData, width, height, rotated_A.x, rotated_A.y, rotated_B.x, rotated_B.y,
				color,
				1); // Top side
			drawLineI4(bmpData, width, height, rotated_B.x, rotated_B.y, rotated_C.x, rotated_C.y,
				color,
				1); // Right side
			drawLineI4(bmpData, width, height, rotated_C.x, rotated_C.y, rotated_D.x, rotated_D.y,
				color,
				1); // Bottom side
			drawLineI4(bmpData, width, height, rotated_D.x, rotated_D.y, rotated_A.x, rotated_A.y,
				color,
				1); // Left side
		}
	} else if (thickness >= 99) {
		fillRegionI4(bmpData, width, height, rotated_A.x, rotated_A.y, rotated_B.x, rotated_B.y,
			rotated_C.x, rotated_C.y, rotated_D.x, rotated_D.y, color);
	} else {
		drawThickLineI4(bmpData, width, height, rotated_A.x, rotated_A.y, rotated_B.x, rotated_B.y,
			color,
			thickness); // Top side
		drawThickLineI4(bmpData, width, height, rotated_B.x, rotated_B.y, rotated_C.x, rotated_C.y,
			color,
			thickness); // Right side
		drawThickLineI4(bmpData, width, height, rotated_C.x, rotated_C.y, rotated_D.x, rotated_D.y,
			color,
			thickness); // Bottom side
		drawThickLineI4(bmpData, width, height, rotated_D.x, rotated_D.y, rotated_A.x, rotated_A.y,
			color,
			thickness); // Left side
	}
}

void drawLineI4Ex(
	uint8_t *bmpData, uint32_t width, uint32_t height, Point A, Point B, uint8_t color) {

	// Apply Transform

	int OffsY = sin((Transform_Pitch) * (M_PI / 180.0)) * 400;
	Point img_center = {
		Transform_OVERLAY_WIDTH / 2, Transform_OVERLAY_HEIGHT / 2}; // Center of the image (example)
	Point original_point = A;										// Example point
	Point original_point2 = B;										// Example point
	// original_point.y-=OffsY;
	// original_point2.y-=OffsY;
	Point rotated_pointA, rotated_pointB;

	int x_rotated, y_rotated;

	rotate_point(original_point, img_center, Transform_Roll, &rotated_pointA);
	rotate_point(original_point2, img_center, Transform_Roll, &rotated_pointB);

	drawLineI4(bmpData, width, height, rotated_pointA.x, rotated_pointA.y, rotated_pointB.x,
		rotated_pointB.y, color, 1);
}

//--------------------UGLY -----------------
uint8_t *_bmpData;
uint32_t _width;
uint32_t _height;
int _rowstride;
int _color;

void plot_pixel(int x, int y) {
	if (x < 0 || x > _width)
		return;

	// Set the pixel at (x0, y0)
	if (x >= 0 && x < _width && y >= 0 && y < _height) {
		uint32_t byteIndex = y * _rowstride + (x / 2);
		// Determine if it's the high nibble or low nibble
		if (x % 2 == 0) {
			_bmpData[byteIndex] = (_bmpData[byteIndex] & 0x0F) |
								  (_color << 4); // High nibble (first pixel in the byte)
		} else {
			_bmpData[byteIndex] = (_bmpData[byteIndex] & 0xF0) | (_color & 0x0F);
		}
	}
}

void plot_pen_pixel(int x, int y, int pen_width) {
	switch (pen_width) {
	case 1:
		plot_pixel(x, y);
		break;

	case 2: {
		plot_pixel(x, y);
		plot_pixel(x + 1, y);
		plot_pixel(x + 1, y + 1);
		plot_pixel(x, y + 1);
	} break;

	case 3: {
		plot_pixel(x, y - 1);
		plot_pixel(x - 1, y - 1);
		plot_pixel(x + 1, y - 1);

		plot_pixel(x, y);
		plot_pixel(x - 1, y);
		plot_pixel(x + 1, y);

		plot_pixel(x, y + 1);
		plot_pixel(x - 1, y + 1);
		plot_pixel(x + 1, y + 1);
	} break;

	default:
		plot_pixel(x, y);
		break;
	}
}

void line_segment(int x1, int y1, int x2, int y2, int width) {
	int steep = 0;
	int sx = ((x2 - x1) > 0) ? 1 : -1;
	int sy = ((y2 - y1) > 0) ? 1 : -1;
	int dx = abs(x2 - x1);
	int dy = abs(y2 - y1);

	if (dy > dx) {
		int temp = x1;
		x1 = y1;
		y1 = temp;
		temp = dx;
		dx = dy;
		dy = temp;
		temp = sx;
		sx = sy;
		sy = temp;
		steep = 1;
	}

	int e = 2 * dy - dx;

	for (int i = 0; i < dx; ++i) {
		if (steep)
			plot_pen_pixel(y1, x1, width);
		else
			plot_pen_pixel(x1, y1, width);

		while (e >= 0) {
			y1 += sy;
			e -= (dx << 1);
		}

		x1 += sx;
		e += (dy << 1);
	}

	// plot_pen_pixel(x2,y2, width);
}

void drawLineI4(uint8_t *bmpData, uint32_t width, uint32_t height, int x0, int y0, int x1, int y1,
	uint8_t color, int thickness) {
	int dx = abs(x1 - x0);
	int dy = abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = (dx > dy ? dx : -dy) / 2;
	int e2;
	uint16_t rowStride = getRowStride(width, 4);
	if (x0 < 0 || x0 > width)
		return;
	if (x1 < 0 || x1 > width || y0 < 0 || y0 > height || y1 < 0 || y1 > height)
		return;

	_bmpData = bmpData;
	_width = width;
	_height = height;
	_rowstride = rowStride;
	_color = color;
	line_segment(x0, y0, x1, y1, thickness);
	return;
	//    if (color==COLOR_WHITE)
	//        return drawLineI4AA( bmpData, width, height, x0, y0,  x1,  y1);
	while (1) {
		// Set the pixel at (x0, y0)
		if (x0 >= 0 && x0 < width && y0 >= 0 && y0 < height) {
			// setPixelI4(bmpData, width, x0, y0, color);
			//  Calculate the byte index for the pixel
			uint32_t byteIndex = y0 * rowStride + (x0 / 2);

			// Determine if it's the high nibble or low nibble
			if (x0 % 2 == 0) {
				// High nibble (first pixel in the byte)
				bmpData[byteIndex] = (bmpData[byteIndex] & 0x0F) | (color << 4);
			} else {
				// Low nibble (second pixel in the byte)
				bmpData[byteIndex] = (bmpData[byteIndex] & 0xF0) | (color & 0x0F);
			}
		}

		// If we've reached the end point, break out of the loop
		if (x0 == x1 && y0 == y1)
			break;

		e2 = err;
		if (e2 > -dx) {
			err -= dy;
			x0 += sx;
		}
		if (e2 < dy) {
			err += dx;
			y0 += sy;
		}
	}
}

// Function to plot a pixel with a specified grayscale level
void setPixelI4AA(uint8_t *bmpData, uint32_t width, uint32_t height, int x, int y, uint8_t color) {
	uint16_t rowStride = getRowStride(width, 4);
	if (x >= 0 && x < width && y >= 0 && y < height) {
		uint32_t byteIndex = y * rowStride + (x / 2);
		if (x % 2 == 0) {
			bmpData[byteIndex] = (bmpData[byteIndex] & 0x0F) | (color << 4);
		} else {
			bmpData[byteIndex] = (bmpData[byteIndex] & 0xF0) | (color & 0x0F);
		}
	}
}

// Function to draw an anti-aliased line
void drawLineI4AA(
	uint8_t *bmpData, uint32_t width, uint32_t height, int x0, int y0, int x1, int y1) {
	int dx = abs(x1 - x0);
	int dy = abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;

	float ed = (dx + dy == 0) ? 1.0f : sqrtf((float)dx * dx + (float)dy * dy);

	while (1) {
		// Calculate the intensity of the color based on the distance to the
		// actual line
		float err2 = abs(err - dx + dy) / ed;
		uint8_t color = COLOR_WHITE;

		if (err2 > 0.9f) {

			color = COLOR_GRAY_Dark;
		} else if (err2 > 0.7f) {
			// color = COLOR_GRAY_Darker;
			color = COLOR_GRAY_Dark;
		} else if (err2 > 0.5f) {
			color = COLOR_GRAY_Dark;
		} else if (err2 > 0.3f) {
			color = COLOR_GRAY_Dark;
		} else if (err2 > 0.1f) {
			color = COLOR_GRAY_Light;
		} else {
			color = COLOR_WHITE;
		}

		// Set the pixel at the current position with the calculated color
		setPixelI4AA(bmpData, width, height, x0, y0, color);

		// If we've reached the end point, break out of the loop
		if (x0 == x1 && y0 == y1)
			break;

		int e2 = err;
		if (e2 * 2 > -dy) {
			err -= dy;
			x0 += sx;
		}
		if (e2 * 2 < dx) {
			err += dx;
			y0 += sy;
		}
	}
}

void convertRGBAToARGB1555(
	uint8_t *srcBitmap, uint32_t width, uint32_t height, uint16_t *destBitmap) {
	int u32Stride = getRowStride(width, 16); // ARGB1555 uses 2 bytes per pixel

	for (uint32_t u32Y = 0; u32Y < height; ++u32Y) {
		for (int32_t u32X = 0; u32X < width; ++u32X) {
			uint32_t srcIndex =
				(u32Y * width + u32X) * 4; // Calculate the source index (4 bytes per pixel)

			// Extract RGBA values
			uint8_t r = srcBitmap[srcIndex + 0];
			uint8_t g = srcBitmap[srcIndex + 1];
			uint8_t b = srcBitmap[srcIndex + 2];
			uint8_t a = srcBitmap[srcIndex + 3];

			// Convert to ARGB1555 format
			uint16_t pixel = 0;

			// Set alpha bit (1 bit)
			if (a > 199) {		 // Assuming any alpha value above 127 is considered
								 // opaque Needs  to be higher
				pixel |= 0x8000; // Set the alpha bit (1 << 15)
			}

			// Set red (5 bits), green (5 bits), and blue (5 bits)
			pixel |= (r >> 3) << 10; // Red: take the top 5 bits
			pixel |= (g >> 3) << 5;	 // Green: take the top 5 bits
			pixel |= (b >> 3);		 // Blue: take the top 5 bits

			// Write the pixel to the destination bitmap
			uint32_t destIndex = u32Y * (u32Stride / 2) + u32X; // 2 bytes per pixel in ARGB1555
			destBitmap[destIndex] = pixel;
		}
	}
}

void convertRGBAToARGB(uint8_t *srcBitmap, uint32_t width, uint32_t height, uint32_t *destBitmap) {
	// Iterate over each pixel in the image
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			uint32_t srcIndex =
				(y * width + x) * 4; // Calculate the source index (4 bytes per pixel)

			// Extract RGBA values
			uint8_t r = srcBitmap[srcIndex + 0];
			uint8_t g = srcBitmap[srcIndex + 1];
			uint8_t b = srcBitmap[srcIndex + 2];
			uint8_t a = srcBitmap[srcIndex + 3];
			if (a < 222)
				a = 0;

			// Convert to ARGB format (store alpha first)
			uint32_t argbPixel = (a << 24) | (r << 16) | (g << 8) | b;

			// Write the pixel to the destination bitmap
			uint32_t destIndex = y * width + x; // Direct index for 32-bit ARGB
			destBitmap[destIndex] = argbPixel;
		}
	}
}
