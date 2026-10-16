#include "region.h"


#if defined(__INFINITY6C__)
//#include "star/i6c_hal.h"   // always include prototypes
//#include "star/i6c_hal.c"
#endif


// https://wx.comake.online/doc/ds82ff82j7jsd9-SSD220/customer/development/mi/en/exclude/mi_rgn.html

#ifdef __SIGMASTAR__
int PIXEL_FORMAT_DEFAULT = E_MI_RGN_PIXEL_FORMAT_I4; // 0 for PIXEL_FORMAT_1555 , 4 for
													 // E_MI_RGN_PIXEL_FORMAT_I8
#else
int PIXEL_FORMAT_DEFAULT = 3; // 0 for PIXEL_FORMAT_1555 , 4 for E_MI_RGN_PIXEL_FORMAT_I8
#endif

extern bool verbose;
const double inv16 = 1.0 / 16.0;

#ifdef __HI3536__
// Hi3536DV100 is an NVR: there is no encoder channel to attach an RGN overlay to, and the
// decoded stream leaves through VO/HDMI. The OSD therefore goes on the hifb graphics layer
// above the video, as silicon_research's osd does next to vdec (msposd#18).
#include <errno.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "hifb.h"

#define HIFB_DEVICE "/dev/fb0"

static int fb_fd = -1;
static unsigned char *fb_mem;
static size_t fb_len;
static struct fb_var_screeninfo fb_var;
static unsigned int fb_stride;
static int fb_x, fb_y;

// hifb keeps whatever size it was last given and does not follow VO, so a
// stale 1280x720 layer would sit on a 1024x768 screen. vdec's own --osd sets
// the layer to the VO size; take that size from video layer 0 the same way.
static int vo_display_size(unsigned int *width, unsigned int *height) {
	char line[256];
	int found = 0, header = 0;
	FILE *f = fopen("/proc/umap/vo", "r");
	if (!f)
		return -1;
	while (!found && fgets(line, sizeof(line), f)) {
		if (strstr(line, "LayerId") && strstr(line, "DispW")) {
			header = 1;
			continue;
		}
		unsigned int layer, imgw, imgh, dispw, disph;
		char en[4], clust[4], fmt[8];
		if (header && sscanf(line, "%u %3s %3s %7s %u %u %u %u", &layer, en, clust, fmt,
						  &imgw, &imgh, &dispw, &disph) == 8 &&
			layer == 0 && dispw && disph) {
			*width = dispw;
			*height = disph;
			found = 1;
		}
		header = 0;
	}
	fclose(f);
	return found ? 0 : -1;
}

static int hifb_open(void) {
	struct fb_fix_screeninfo fix;
	unsigned int vo_w, vo_h;
	HIFB_ALPHA_S alpha;
	HIFB_COLORKEY_S colorkey;
	HI_BOOL show = HI_TRUE;

	if (fb_fd >= 0)
		return 0;

	fb_fd = open(HIFB_DEVICE, O_RDWR);
	if (fb_fd < 0) {
		fprintf(stderr, "[%s:%d]open %s failed: %s\n", __func__, __LINE__, HIFB_DEVICE,
			strerror(errno));
		return -1;
	}

	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0)
		goto fail;

	if (!vo_display_size(&vo_w, &vo_h)) {
		fb_var.xres = vo_w;
		fb_var.yres = vo_h;
	}

	// ARGB1555 is the format msposd composes in, so a frame is a plain row copy
	fb_var.xres_virtual = fb_var.xres;
	fb_var.yres_virtual = fb_var.yres;
	fb_var.xoffset = 0;
	fb_var.yoffset = 0;
	fb_var.bits_per_pixel = 16;
	fb_var.transp = (struct fb_bitfield){15, 1, 0};
	fb_var.red = (struct fb_bitfield){10, 5, 0};
	fb_var.green = (struct fb_bitfield){5, 5, 0};
	fb_var.blue = (struct fb_bitfield){0, 5, 0};
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var) < 0 ||
		ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0 || fb_var.bits_per_pixel != 16 ||
		ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0)
		goto fail;

	fb_stride = fix.line_length;
	fb_len = (size_t)fb_stride * fb_var.yres;
	fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		fb_mem = NULL;
		goto fail;
	}

	// The 1555 alpha bit picks u8Alpha0 (clear) or u8Alpha1 (solid) per pixel
	memset(&alpha, 0, sizeof(alpha));
	alpha.bAlphaEnable = HI_TRUE;
	alpha.bAlphaChannel = HI_FALSE;
	alpha.u8Alpha0 = 0;
	alpha.u8Alpha1 = 255;
	alpha.u8GlobalAlpha = 255;

	memset(&colorkey, 0, sizeof(colorkey));
	colorkey.bKeyEnable = HI_FALSE;

	memset(fb_mem, 0, fb_len);
	if (ioctl(fb_fd, FBIOPUT_ALPHA_HIFB, &alpha) < 0 ||
		ioctl(fb_fd, FBIOPUT_COLORKEY_HIFB, &colorkey) < 0 ||
		ioctl(fb_fd, FBIOPUT_SHOW_HIFB, &show) < 0)
		goto fail;

	if (verbose)
		printf("%s: %ux%u, %u bytes per line\n", HIFB_DEVICE, fb_var.xres, fb_var.yres,
			fb_stride);
	return 0;

fail:
	fprintf(stderr, "[%s:%d]%s setup failed: %s\n", __func__, __LINE__, HIFB_DEVICE,
		strerror(errno));
	if (fb_mem) {
		munmap(fb_mem, fb_len);
		fb_mem = NULL;
	}
	close(fb_fd);
	fb_fd = -1;
	return -1;
}

int hifb_get_resolution(int *width, int *height) {
	if (hifb_open())
		return -1;
	*width = fb_var.xres;
	*height = fb_var.yres;
	return 0;
}

static int hifb_set_bitmap(BITMAP *bitmap) {
	// The layer is set up as ARGB1555, so only a 16-bit bitmap can be copied in
	if (!fb_mem || !bitmap->pData || bitmap->enPixelFormat != PIXEL_FORMAT_1555)
		return -1;

	unsigned int w = bitmap->u32Width;
	unsigned int h = bitmap->u32Height;
	unsigned int src_stride = w * 2;
	unsigned char *src = (unsigned char *)bitmap->pData;

	if (fb_x + w <= fb_var.xres && fb_y + h <= fb_var.yres) {
		for (unsigned int i = 0; i < h; i++)
			memcpy(fb_mem + (size_t)(fb_y + i) * fb_stride + fb_x * 2,
				src + (size_t)i * src_stride, w * 2);
		return 0;
	}

	// The glyph grid is sized for 720p or 1080p, but vdec also drives 1024x768 and the
	// like: shrink it to fit rather than lose the right-hand columns.
	unsigned int scale = (fb_var.xres << 16) / w;
	if (((fb_var.yres << 16) / h) < scale)
		scale = (fb_var.yres << 16) / h;
	unsigned int dw = (w * scale) >> 16, dh = (h * scale) >> 16;
	unsigned int ox = (fb_var.xres - dw) / 2, oy = 0;
	// fb_y places the unscaled grid at the top, centre or bottom (--matrix); keep that
	// alignment by mapping it onto the room the shrunk grid leaves
	if (fb_y > 0 && fb_var.yres > h)
		oy = (unsigned long long)fb_y * (fb_var.yres - dh) / (fb_var.yres - h);
	if (oy + dh > fb_var.yres)
		oy = fb_var.yres - dh;
	unsigned int step = (unsigned int)((1ULL << 32) / scale);

	for (unsigned int y = 0; y < dh; y++) {
		uint16_t *s = (uint16_t *)(src + (size_t)((y * step) >> 16) * src_stride);
		uint16_t *d = (uint16_t *)(fb_mem + (size_t)(oy + y) * fb_stride) + ox;
		for (unsigned int x = 0, sx = 0; x < dw; x++, sx += step)
			d[x] = s[sx >> 16];
	}
	return 0;
}

static void hifb_close(void) {
	if (fb_mem) {
		memset(fb_mem, 0, fb_len);
		munmap(fb_mem, fb_len);
		fb_mem = NULL;
	}
	if (fb_fd >= 0) {
		close(fb_fd);
		fb_fd = -1;
	}
}
#endif



int InitRGN_SigmaStar(){
#if __SIGMASTAR__		
#if __INFINITY6C__		

	MI_SYS_Init(0);
	//int s32Ret = i6c_region_init(&g_stPaletteTable);//Old way using Hardware Abstraction, not needed	
	
	MI_RGN_InitParam_t p = {0};//Needed for MI_RGN_InitDev some day
    p.pstPaletteTable = &g_stPaletteTable ;//

	//int s32Ret = MI_RGN_InitDev( 0, &p);//New version, needs to be added in header files
	int s32Ret = MI_RGN_Init(DEV &g_stPaletteTable);
	
	if (verbose)
		printf("MI_RGN_Init_6c results: %d   \n", s32Ret);
	if (s32Ret)
		fprintf(stderr, "[%s:%d]RGN_Init_6c failed with %#x!\n", __func__, __LINE__, s32Ret);
	
#else	
	int s32Ret = MI_RGN_Init(DEV &g_stPaletteTable);	
	if (verbose)
		printf("MI_RGN_Init results: %d\n", s32Ret);
	if (s32Ret)
		fprintf(stderr, "[%s:%d]RGN_Init failed with %#x!\n", __func__, __LINE__, s32Ret);
#endif	
#endif	
}

int create_region(int *handle, int x, int y, int width, int height) {
	int s32Ret = -1;
#ifdef __HI3536__
	if (hifb_open())
		return -1;
	fb_x = x < 0 ? 0 : x;
	fb_y = y < 0 ? 0 : y;
	return 0;
#endif
#if !defined(_x86) && !defined(__ROCKCHIP__) && !defined(__HI3536__)
#ifdef __SIGMASTAR__
	MI_RGN_ChnPort_t stChn;

	MI_RGN_Attr_t stRegionCurrent;
	MI_RGN_Attr_t stRegion;

	MI_RGN_ChnPortParam_t stChnAttr;
	MI_RGN_ChnPortParam_t stChnAttrCurrent;

#if __INFINITY6C__	
	stChn.s32DevId = 0;
	stChn.s32ChnId = 0;
	//Need to test which one performs best!
	//stChn.eModId = E_MI_RGN_MODID_VPE ;   
	//stChn.eModId =  2;//I6C_SYS_MOD_VENC =2  
	stChn.eModId =  34;//I6C_SYS_MOD_SCL =34   
	//stChn.eModId =  36;//I6C_SYS_MOD_DSP = 36
	
#else
	stChn.eModId = E_MI_RGN_MODID_VPE;
#endif	
	stChn.s32DevId = 0;
	stChn.s32ChnId = 0;
	stChn.s32OutputPortId = 0;

	stRegion.eType = E_MI_RGN_TYPE_OSD;
	stRegion.stOsdInitParam.stSize.u32Height = height;
	stRegion.stOsdInitParam.stSize.u32Width = width;
	//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	stRegion.stOsdInitParam.ePixelFmt =
		PIXEL_FORMAT_DEFAULT; // PIXEL_FORMAT_1555;// E_MI_RGN_PIXEL_FORMAT_I4;
							  // //;PIXEL_FORMAT_1555;//E_MI_RGN_PIXEL_FORMAT_I4;//PIXEL_FORMAT_1555

	s32Ret = MI_RGN_GetAttr(DEV *handle, &stRegionCurrent);
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
	if (s32Ret) {
		if (verbose)
			fprintf(stderr, "[%s:%d]RGN_GetAttr failed with %#x , creating region %d...\n",
				__func__, __LINE__, s32Ret, *handle);
#ifdef __SIGMASTAR__
		s32Ret = MI_RGN_Create(DEV *handle, &stRegion);
#else
		s32Ret = HI_MPI_RGN_Create(*handle, &stRegion);
#endif
		if (s32Ret) {
			fprintf(stderr, "[%s:%d]RGN_Create failed with %#x!\n", __func__, __LINE__, s32Ret);
			return -1;
		}
	} else {
#ifdef __SIGMASTAR__
		if (stRegionCurrent.stOsdInitParam.stSize.u32Height !=
				stRegion.stOsdInitParam.stSize.u32Height ||
			stRegionCurrent.stOsdInitParam.stSize.u32Width !=
				stRegion.stOsdInitParam.stSize.u32Width)
#else
		if (stRegionCurrent.unAttr.stOverlay.stSize.u32Height !=
				stRegion.unAttr.stOverlay.stSize.u32Height ||
			stRegionCurrent.unAttr.stOverlay.stSize.u32Width !=
				stRegion.unAttr.stOverlay.stSize.u32Width)
#endif
		{
			fprintf(stderr, "[%s:%d] Region parameters are different, recreating ... \n", __func__,
				__LINE__);
#ifdef __SIGMASTAR__
			MI_RGN_DetachFromChn(DEV *handle, &stChn);
			MI_RGN_Destroy(DEV *handle);
			s32Ret = MI_RGN_Create(DEV *handle, &stRegion);
#else
			HI_MPI_RGN_DetachFromChn(*handle, &stChn);
			HI_MPI_RGN_Destroy(*handle);
			s32Ret = HI_MPI_RGN_Create(*handle, &stRegion);
#endif
			if (s32Ret) {
				fprintf(stderr, "[%s:%d]RGN_Create failed with %#x!\n", __func__, __LINE__, s32Ret);
				return -1;
			}
		}
	}

#ifdef __SIGMASTAR__
	s32Ret = MI_RGN_GetDisplayAttr(DEV *handle, &stChn, &stChnAttrCurrent);
#else
	s32Ret = HI_MPI_RGN_GetDisplayAttr(*handle, &stChn, &stChnAttrCurrent);
#endif
	if (s32Ret)
		if (verbose)
			fprintf(stderr, "[%s:%d]RGN_GetDisplayAttr failed with %#x %d, attaching...\n",
				__func__, __LINE__, s32Ret, *handle);
#ifdef __SIGMASTAR__
		else if (stChnAttrCurrent.stPoint.u32X != x || stChnAttrCurrent.stPoint.u32Y != y)
#else
		else if (stChnAttrCurrent.unChnAttr.stOverlayChn.stPoint.s32X != x ||
				 stChnAttrCurrent.unChnAttr.stOverlayChn.stPoint.s32Y != y)
#endif
		{
			if (verbose)
				fprintf(stderr,
					"[%s:%d] Position has changed, detaching handle %d from "
					"channel %d...\n",
					__func__, __LINE__, *handle, &stChn.s32ChnId);
#ifdef __SIGMASTAR__
			MI_RGN_DetachFromChn(DEV *handle, &stChn);
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
	stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8BgAlpha = 0; // 0;
	stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8FgAlpha = 255;

	/*
	stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.eAlphaMode =
	E_MI_RGN_CONSTANT_ALPHA;
	stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8BgAlpha
	= 64;
	stChnAttr.unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara.stArgb1555Alpha.u8FgAlpha
	= 127;
	*/

#if __INFINITY6C__
	stChn.s32ChnId = 0;
	stChn.s32OutputPortId = 0;
#else
	stChn.s32OutputPortId = 0;
#endif
	s32Ret = MI_RGN_AttachToChn(DEV *handle, &stChn, &stChnAttr);
#if __INFINITY6C__
	stChn.s32ChnId = 1;
	stChn.s32OutputPortId = 1;
#else
	stChn.s32OutputPortId = 1;
#endif
	s32Ret = MI_RGN_AttachToChn(DEV *handle, &stChn, &stChnAttr);
#else
	memset(&stChnAttr, 0, sizeof(RGN_CHN_ATTR_S));
	stChnAttr.bShow = 1;
	stChnAttr.enType = OVERLAY_RGN;
	stChnAttr.unChnAttr.stOverlayChn.u32BgAlpha = 0;
	stChnAttr.unChnAttr.stOverlayChn.u32FgAlpha = 128;
	stChnAttr.unChnAttr.stOverlayChn.stQpInfo.bQpDisable = 0;
	stChnAttr.unChnAttr.stOverlayChn.stQpInfo.bAbsQp = 0;
	stChnAttr.unChnAttr.stOverlayChn.stQpInfo.s32Qp = 0;
#ifndef __HI3536__
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

static int skipcol = 1;

int prepare_bitmap(
	const char *filename, BITMAP *bitmap, int bFil, unsigned int u16FilColor, int enPixelFmt) {
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

	if (parse_bitmap(filename, &bmpFileHeader, &bmpInfo) < 0) {
		fprintf(stderr, "GetBmpInfo err!\n");
		return -1;
	}

	switch (enPixelFmt) {
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

	if (!(bitmap->pData =
				malloc(s32BytesPerPix * bmpInfo.bmiHeader.biWidth * bmpInfo.bmiHeader.biHeight))) {
		fputs("malloc osd memory err!\n", stderr);
		return -1;
	}

	CreateSurfaceByBitMap(filename, &Surface, (unsigned char *)(bitmap->pData));

	bitmap->u32Width = Surface.u16Width;
	bitmap->u32Height = Surface.u16Height;
	bitmap->enPixelFormat = enPixelFmt;

	int i, j, k;
	unsigned char *pu8Temp;

	if (skipcol > 700) // test performance
		skipcol = 1;
	skipcol++;

#ifndef __16CV300__
	if (enPixelFmt == PIXEL_FORMAT_2BPP) {
		s32Width = DIV_UP(bmpInfo.bmiHeader.biWidth, 4);
		pu8Data = malloc(s32Width * bmpInfo.bmiHeader.biHeight);
		if (!pu8Data) {
			fputs("malloc osd memory err!\n", stderr);
			return -1;
		}
	}

#endif
	long ccc = 0;
	if (enPixelFmt != PIXEL_FORMAT_2BPP) {
		unsigned short *pu16Temp;
		pu16Temp = (unsigned short *)bitmap->pData;
		if (bFil > 0) {

			for (i = 0; i < bitmap->u32Height; i++) {
				for (j = 0; j < bitmap->u32Width; j++) { // 64478 = 0xFBDE R:245,G:245,B:245
					if (bFil == 2 && i == 0 && j == 0)	 // take transparent color from first pixel
						u16FilColor = *pu16Temp;

					if (u16FilColor == *pu16Temp) {
						*pu16Temp &= 0x7FFF;
					}
					pu16Temp++;
				}
			}
		}
		// printf("enPixelFmt:%d\n",ccc);
	} else {
		unsigned short *pu16Temp;

		pu16Temp = (unsigned short *)bitmap->pData;
		pu8Temp = (unsigned char *)pu8Data;

		for (i = 0; i < bitmap->u32Height; i++) {
			for (j = 0; j < bitmap->u32Width / 4; j++) {
				Value = 0;

				for (k = j; k < j + 4; k++) {
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

int set_bitmap(int handle, BITMAP *bitmap) {
	int s32Ret = 0;
#ifdef __HI3536__
	return hifb_set_bitmap(bitmap);
#endif
#if !defined(_x86) && !defined(__ROCKCHIP__) && !defined(__HI3536__)
#ifdef __SIGMASTAR__
	s32Ret = MI_RGN_SetBitMap(DEV handle, (MI_RGN_Bitmap_t *)(bitmap));
#elif __GOKE__
	s32Ret = HI_MPI_RGN_SetBitMap(handle, (BITMAP_S *)(bitmap));
#endif
	if (s32Ret) {
#ifdef __SIGMASTAR__
		if (s32Ret == E_MI_ERR_ILLEGAL_PARAM)
			fprintf(stderr, "RGN_SetBitMap failed E_MI_ERR_ILLEGAL_PARAM %#x!\n", s32Ret);
#endif
		fprintf(stderr, "RGN_SetBitMap failed with %#x  %d!\n", s32Ret, (s32Ret & 0xFFF));
		return -1;
	}
#endif
	return s32Ret;
}

/// @brief Returns a pointer to BMP directly in the video overlay region, so no
/// need to create a buffer and copy it later!
/// @param handle
/// @return
void *get_directBMP(int handle) {
#ifdef __SIGMASTAR__
	MI_RGN_CanvasInfo_t stCanvasInfo;
	memset(&stCanvasInfo, 0, sizeof(stCanvasInfo));
	int s32Ret = GetCanvas(handle, &stCanvasInfo);

	if (s32Ret)
	   printf("GetCanvas Res: %X OSD Handle:%d  CanvasStride: %d , Canvas size : %d:%d\r\n",
	   s32Ret, handle, stCanvasInfo.u32Stride, stCanvasInfo.stSize.u32Width,
	 stCanvasInfo.stSize.u32Height);
	return (void *)(stCanvasInfo.virtAddr);
#endif
	return NULL;
}

unsigned long set_bitmapEx(int handle, BITMAP *bitmap, int BitsPerPixel) {
	int s32Ret = 0;
#ifdef __SIGMASTAR__
	int byteWidth = getRowStride(bitmap->u32Width, BitsPerPixel);
	MI_RGN_CanvasInfo_t stCanvasInfo;
	memset(&stCanvasInfo, 0, sizeof(stCanvasInfo));
	s32Ret = GetCanvas(handle, &stCanvasInfo);
	printf("OSD Handle:%d GetCanvas:%d  CanvasStride: %d , BMP_Stride:%d "
		   "BMP_Size: %d:%d , Canvas size : %d:%d\r\n",
		handle, s32Ret, stCanvasInfo.u32Stride, byteWidth, bitmap->u32Width, bitmap->u32Height,
		stCanvasInfo.stSize.u32Width, stCanvasInfo.stSize.u32Height);

	for (int i = 0; i < bitmap->u32Height; i++)
		memcpy((void *)(stCanvasInfo.virtAddr + i * (stCanvasInfo.u32Stride)),
			bitmap->pData + i * byteWidth, byteWidth);

	// memcpy((void *)(stCanvasInfo.virtAddr) , bitmap->pData ,
	// bitmap->u32Height
	// * byteWidth);

	s32Ret = MI_RGN_UpdateCanvas(DEV handle);

	if (verbose)
		printf("MI_RGN_UpdateCanvas completed byteWidth:%d!\n", byteWidth);

	// this will break, the pointer to memory is no longer valid!
	// memset((void *)(stCanvasInfo.virtAddr),
	// PIXEL_FORMAT_DEFAULT==PIXEL_FORMAT_I4 ? 0xFF : 0x00 , bitmap->u32Height *
	// getRowStride(bitmap->u32Width , BitsPerPixel));

	return stCanvasInfo.virtAddr;
#else
	return s32Ret;
#endif
}

int unload_region(int *handle) {
	int s32Ret = 0;
#ifdef __HI3536__
	hifb_close();
#elif defined(__SIGMASTAR__)
	MI_RGN_ChnPort_t stChn;
#if __INFINITY6C__
	stChn.eModId = E_MI_MODULE_ID_VENC;
#else
	stChn.eModId = E_MI_RGN_MODID_VPE;
#endif
	stChn.s32DevId = 0;
	stChn.s32ChnId = 0;
	stChn.s32OutputPortId = 0;

	MI_RGN_DetachFromChn(DEV *handle, &stChn);
	s32Ret = MI_RGN_Destroy(DEV *handle);
	if (s32Ret)
		fprintf(stderr, "[%s:%d]RGN_Destroy failed with %#x %d!\n", __func__, __LINE__, s32Ret,
			*handle);
#elif __GOKE__
	MPP_CHN_S stChn;
	stChn.s32DevId = 0;
	stChn.s32ChnId = 0;

	stChn.enModId = HI_ID_VENC;
	HI_MPI_RGN_DetachFromChn(*handle, &stChn);
	s32Ret = HI_MPI_RGN_Destroy(*handle);
	if (s32Ret)
		fprintf(stderr, "[%s:%d]RGN_Destroy failed with %#x %d!\n", __func__, __LINE__, s32Ret,
			*handle);
#endif
	return s32Ret;
}
#ifdef __SIGMASTAR__

int GetCanvas(int handle, MI_RGN_CanvasInfo_t *stCanvasInfo) {	
	int s32Result = MI_RGN_GetCanvasInfo(DEV handle, stCanvasInfo);	
	if (s32Result != MI_RGN_OK)
		return s32Result;

	// if (verbose)
	//     printf("MI_RGN_GetCanvas stride:%d  stSize:%d:%d  ePixelFmt:%d !\n",
	// stCanvasInfo->u32Stride, stCanvasInfo->stSize.u32Width ,
	// stCanvasInfo->stSize.u32Height , stCanvasInfo->ePixelFmt);
	return 0;
}

uint32_t ST_OSD_DrawPoint(
	MI_U16 *pDst, MI_U32 u32Stride, uint32_t u32X, uint32_t u32Y, MI_U32 u32Color) {

	MI_U8 u8Value = 0;

	// ST_DBG("pDst:%p, u32Stride:%d, point(%d,%d)\n", pDst, u32Stride, u32X,
	// u32Y);

	if (/*g_stRgnInfo[hHandle].ePixelFmt == E_MI_RGN_PIXEL_FORMAT_I4*/ true) {

		if (u32X % 2) {
			u8Value = (*((MI_U8 *)pDst + (u32Stride * u32Y) + u32X / 2) & 0x0F) |
					  ((u32Color & 0x0f) << 4);
			*((MI_U8 *)pDst + (u32Stride * u32Y) + u32X / 2) = u8Value;
		} else {
			u8Value = (*((MI_U8 *)pDst + (u32Stride * u32Y) + u32X / 2) & 0xF0) | (u32Color & 0x0f);
			*((MI_U8 *)pDst + (u32Stride * u32Y) + u32X / 2) = u8Value;
		}
	}

	return MI_RGN_OK;
}

void DrawBitmap1555ToI4(uint16_t *srcBitmap, uint32_t width, uint32_t height, uint8_t *destBitmap,
	MI_RGN_PaletteTable_t *paletteTable, MI_U16 *pDst, MI_U32 u32Stride) {

	for (uint32_t y = 0; y < height; ++y) {

		for (uint32_t x = 0; x < width; ++x) {
			uint32_t srcIndex = y * width + x;
			uint8_t paletteIndex = findClosestPaletteIndex8(srcBitmap[srcIndex], paletteTable);

			if (paletteIndex == 0)	// Sigmastar reserver
				paletteIndex = 8;	// black
			if (paletteIndex == 17) // Transparent color
				paletteIndex = 15;

			ST_OSD_DrawPoint(pDst, u32Stride, x, y, paletteIndex);
		}
	}
}

#endif
