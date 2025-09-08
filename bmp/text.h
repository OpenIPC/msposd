#ifndef TEXT_H_
#define TEXT_H_

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

#include "lib/schrift.h"

#include "common.h"

static SFT sft;
static char last_font_name[256] = "";
static double last_font_size=0;
static SFT_Image canvas;
static SFT_LMetrics lmtx;
static BITMAP bitmap;

RECT measure_text(const char *font, double size, const char *text);
BITMAP raster_text(const char *font, double size, const char *text, uint16_t color);

int FreeCachedFont();

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif