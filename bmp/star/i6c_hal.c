#if defined(__INFINITY6C__)

#include "i6c_hal.h"

i6c_rgn_impl  i6c_rgn;
i6c_sys_impl  i6c_sys;

char _i6c_venc_dev[2] = { 0, 8 };
char _i6c_venc_port = 0;

void i6c_hal_deinit(void)
{
    i6c_system_deinit();

    i6c_rgn_unload(&i6c_rgn);
    i6c_sys_unload(&i6c_sys);
}

int i6c_hal_init(void)
{
    int ret;

    if (ret = i6c_sys_load(&i6c_sys))
        return ret;
    if (ret = i6c_rgn_load(&i6c_rgn))
        return ret;

    if (ret = i6c_system_init())
        return ret;

    return EXIT_SUCCESS;
}

int i6c_region_create(char handle, hal_rect rect, short opacity)
{
    int ret = EXIT_SUCCESS;

    i6c_sys_bind dest = { .module = I6C_SYS_MOD_VENC, .port =_i6c_venc_port };
    //i6c_sys_bind dest = { .module = I6C_SYS_MOD_VPE, .port =_i6c_venc_port };
     
    i6c_rgn_cnf region, regionCurr;
    i6c_rgn_chn attrib, attribCurr;

    region.type = I6C_RGN_TYPE_OSD;
    region.pixFmt = PIXEL_FORMAT_DEFAULT;//I6C_RGN_PIXFMT_ARGB1555;
    region.size.width = rect.width;
    region.size.height = rect.height;

    if (i6c_rgn.fnGetRegionConfig(0, handle, &regionCurr)) {
        HAL_INFO("i6c_rgn", "Creating region %d...\n", handle);
        if (ret = i6c_rgn.fnCreateRegion(0, handle, &region))
            return ret;
    } else if (regionCurr.type != region.type ||
        regionCurr.size.height != region.size.height || 
        regionCurr.size.width != region.size.width) {
        HAL_INFO("i6c_rgn", "Parameters are different, recreating "
            "region %d...\n", handle);
        for (dest.channel = 0; dest.channel < 2; dest.channel++) {
            dest.device = _i6c_venc_dev[0];
            i6c_rgn.fnDetachChannel(0, handle, &dest);
            dest.device = _i6c_venc_dev[1];
            i6c_rgn.fnDetachChannel(0, handle, &dest);
        }
        i6c_rgn.fnDestroyRegion(0, handle);
        if (ret = i6c_rgn.fnCreateRegion(0, handle, &region))
            return ret;
    }

    if (i6c_rgn.fnGetChannelConfig(0, handle, &dest, &attribCurr))
        HAL_INFO("i6c_rgn", "Attaching region %d...\n", handle);
    else if (attribCurr.point.x != rect.x || attribCurr.point.x != rect.y ||
        attribCurr.osd.bgFgAlpha[1] != opacity) {
        HAL_INFO("i6c_rgn", "Parameters are different, reattaching "
            "region %d...\n", handle);
        for (dest.channel = 0; dest.channel < 2; dest.channel++) {
            dest.device = _i6c_venc_dev[0];
            i6c_rgn.fnDetachChannel(0, handle, &dest);
            dest.device = _i6c_venc_dev[1];
            i6c_rgn.fnDetachChannel(0, handle, &dest);
        }
    }

    memset(&attrib, 0, sizeof(attrib));
    attrib.show = 1;
    attrib.point.x = rect.x;
    attrib.point.y = rect.y;
    attrib.osd.layer = 0;
    attrib.osd.constAlphaOn = 0;
    attrib.osd.bgFgAlpha[0] = 0;
    attrib.osd.bgFgAlpha[1] = opacity;

    for (dest.channel = 0; dest.channel < 2; dest.channel++) {
        dest.device = _i6c_venc_dev[0];
        i6c_rgn.fnAttachChannel(0, handle, &dest, &attrib);
        dest.device = _i6c_venc_dev[1];
        i6c_rgn.fnAttachChannel(0, handle, &dest, &attrib);
    }

    return EXIT_SUCCESS;
}

void i6c_region_deinit(void)
{
    i6c_rgn.fnDeinit(0);
}

void i6c_region_destroy(char handle)
{
    i6c_sys_bind dest = { .module = I6C_SYS_MOD_VENC, .port =_i6c_venc_port };

    for (dest.channel = 0; dest.channel < 2; dest.channel++) {
        dest.device = _i6c_venc_dev[0];
        i6c_rgn.fnDetachChannel(0, handle, &dest);
        dest.device = _i6c_venc_dev[1];
        i6c_rgn.fnDetachChannel(0, handle, &dest);
    }
    i6c_rgn.fnDestroyRegion(0, handle);
}

void i6c_region_init(i6c_rgn_pal *palette)
{
    //i6c_rgn_pal palette = {{{0, 0, 0, 0}}};
    i6c_rgn.fnInit(0, palette);
}

int i6c_region_setbitmap(int handle, hal_bitmap *bitmap)
{
    i6c_rgn_bmp nativeBmp = { .data = bitmap->data, .pixFmt = I6C_RGN_PIXFMT_ARGB1555,
        .size.height = bitmap->dim.height, .size.width = bitmap->dim.width };

    return i6c_rgn.fnSetBitmap(0, handle, &nativeBmp);
}

void i6c_system_deinit(void)
{
    i6c_sys.fnExit(0);
}

int i6c_system_init(void)
{
    int ret;

    if (ret = i6c_sys.fnInit(0))
        return ret;

    printf("App built with headers v%s\n", I6C_SYS_API);

    {
        i6c_sys_ver version;
        if (ret = i6c_sys.fnGetVersion(0, &version))
            return ret;
        puts(version.version);
    }

    return EXIT_SUCCESS;
}

#endif