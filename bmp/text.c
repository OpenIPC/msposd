#include "text.h"

const double inv255 = 1.0 / 255.0;
extern int PIXEL_FORMAT_DEFAULT;

static void copyimage(SFT_Image *dest, const SFT_Image *source, int x0, int y0, int color) {
	unsigned short maskr = (color & 0x7C00) >> 10;
	unsigned short maskg = (color & 0x3E0) >> 5;
	unsigned short maskb = color & 0x1F;

	unsigned short *d = dest->pixels;
	unsigned char *s = source->pixels;

	// int rowStride=getRowStride(dest->width,16);
	d += x0 + y0 * dest->width; // rowStride

	for (int y = 0; y < source->height; y++) {
		for (int x = 0; x < source->width; x++) {
			/* we do not need gray scale when we work with only 16 colors
			double t = s[x] * inv255;
			unsigned short r = (1.0 - t) * ((d[x] & 0x7C00) >> 10) + t * maskr;
			unsigned short g = (1.0 - t) * ((d[x] & 0x3E0) >> 5) + t * maskg;
			unsigned short b = (1.0 - t) * (d[x] & 0x1F) + t * maskb;
			d[x] = ((t > 0.0) << 15) | (r << 10) | (g << 5) | b;
			*/
			unsigned char pixel = s[x];
			// If the pixel is not zero (assuming non-zero means black)
			if (pixel > 96)	   // 128 makes them too thin... 64 is best
				d[x] = color | 0x8000 ; //d[x] = 0x8000; // Set pixel to opaque black (ARGB1555: 1000 0000 0000 0000)
			else
				d[x] =  color & 0x7FFF; //0x0000; // Set pixel to transparent (ARGB1555: 0000 0000 0000 0000)
		}
		d += dest->width; // rowStride
		s += source->width;
	}
}

static void loadfont(SFT *sft, const char *path, double size, SFT_LMetrics *lmtx) {
	SFT_Font *font = sft_loadfile(path);
	if (font == NULL)
		fatal("sft_loadfile failed");
	sft->font = font;
	sft->xScale = size;
	sft->yScale = size;
	sft->xOffset = 0.0;
	sft->yOffset = 0.0;
	sft->flags = SFT_DOWNWARD_Y;
	if (sft_lmetrics(sft, lmtx) < 0)
		fatal("sft_lmetrics failed");
}

static void loadglyph(
	const SFT *sft, SFT_UChar codepoint, SFT_Glyph *glyph, SFT_GMetrics *metrics) {
	if (sft_lookup(sft, codepoint, glyph) < 0)
		fatal("sft_lookup failed");
	if (sft_gmetrics(sft, *glyph, metrics) < 0)
		fatal("sft_gmetrics failed");
}

int getRowStride(int width, int BitsPerPixel);

static void newimage(SFT_Image *image, int width, int height, int color) {
	// size_t size = (size_t)(width * height * 2);
	size_t size = (size_t)(height * getRowStride(width, 16));
	void *pixels = malloc(size);
	image->pixels = pixels;
	image->width = width;
	image->height = height;
	if (color == 0)
		memset(pixels, 0, size);
	else
		for (int i = 0; i < size / 2; i++)
			((unsigned short *)pixels)[i] = color;
}

static inline void calcdim(double *margin, double *height, double *width, const char *text) {
	double lwidth = 0;
	*margin = 0;
	*height = lmtx.ascender - lmtx.descender + lmtx.lineGap;
	*width = 0;

#ifdef SUPP_UTF32
	unsigned cps[strlen(text) + 1];
	int n = utf8_to_utf32(text, cps, strlen(text) + 1);
#else
#define cps text
#define n strlen(text)
#endif

	for (int k = 0; k < n; k++) {
		if (cps[k] == '\\' && cps[k + 1] == 'n') {
			k++;
			*width = MAX(*width, lwidth);
			*height += lmtx.ascender - lmtx.descender + lmtx.lineGap;
			lwidth = 0;
			continue;
		}
		SFT_UChar cp = (SFT_UChar)cps[k];
		SFT_Glyph gid;
		SFT_GMetrics mtx;
		loadglyph(&sft, cp, &gid, &mtx);
		if (lwidth == 0 && mtx.leftSideBearing < 0 && *margin < -mtx.leftSideBearing)
			*margin -= mtx.leftSideBearing;
		lwidth += MAX(mtx.advanceWidth, mtx.minWidth);
	}
	*height += -lmtx.descender + lmtx.lineGap + 2 * *margin;
	*width = MAX(*width, lwidth) + 2 * *margin;
}

RECT measure_text(const char *font, double size, const char *text) {
	loadfont(&sft, font, size, &lmtx);

	double margin, height, width;
	calcdim(&margin, &height, &width, text);
	// Some platforms operate with a coarse pixel size of 2x2
	// and rounding up is required for a sufficient canvas size
	RECT rect = {.height = ceil(height), .width = ceil(width)};
	rect.height += rect.height & 1;
	rect.width += rect.width & 1;

	sft_freefont(sft.font);
	return rect;
}

BITMAP raster_text(const char *font, double size, const char *text, uint16_t color) {
	loadfont(&sft, font, size, &lmtx);

	double margin, height, width;

	calcdim(&margin, &height, &width, text);

	// WIDTH Needs to be multiple of 8 ?!?!
	int MaxWidth = (int)(width + 7) & ~7; // multiple of 8
	// int MinWidth=(int)width & ~7;
	// width = abs(MaxWidth-width)>(width - MinWidth) ? MinWidth:MaxWidth;
	newimage(&canvas, MaxWidth, height, 0);

#ifdef SUPP_UTF32
	unsigned cps[strlen(text) + 1];
	int n = utf8_to_utf32(text, cps, strlen(text) + 1);
#else
#define cps text
#define n strlen(text)
#endif

	double x = margin;
	double y = margin + lmtx.ascender + lmtx.lineGap;
	SFT_Glyph ogid = 0;
	for (int k = 0; k < n; k++) {
		if (cps[k] == '\\' && cps[k + 1] == 'n') {
			k++;
			x = margin;
			y += lmtx.ascender - lmtx.descender + lmtx.lineGap;
			ogid = 0;
			continue;
		}
		SFT_Image image;
		SFT_UChar cp = (SFT_UChar)cps[k];
		SFT_Glyph gid;
		SFT_GMetrics mtx;
		SFT_Kerning kerning;
		loadglyph(&sft, cp, &gid, &mtx);
		newimage(&image, mtx.minWidth, mtx.minHeight, 0x7FFF);
		sft_render(&sft, gid, image);
		sft_kerning(&sft, ogid, gid, &kerning);
		x += kerning.xShift;
		copyimage(&canvas, &image, x + mtx.leftSideBearing, y + mtx.yOffset, color/*0xFFFF*/);
		x += mtx.advanceWidth;
		free(image.pixels);
		ogid = gid;
	}

	bitmap.u32Width = canvas.width;
	bitmap.u32Height = canvas.height;
	bitmap.pData = canvas.pixels;
	bitmap.enPixelFormat = PIXEL_FORMAT_1555;

	sft_freefont(sft.font);
	return bitmap;
}
