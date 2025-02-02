/* Copyright (c) 2018-2019 Sigmastar Technology Corp.
 All rights reserved.

  Unless otherwise stipulated in writing, any and all information contained
 herein regardless in any format shall remain the sole proprietary of
 Sigmastar Technology Corp. and be kept in strict confidence
 (��Sigmastar Confidential Information��) by the recipient.
 Any unauthorized act including without limitation unauthorized disclosure,
 copying, use, reproduction, sale, distribution, modification, disassembling,
 reverse engineering and compiling of the contents of Sigmastar Confidential
 Information is unlawful and strictly prohibited. Sigmastar hereby reserves the
 rights to any and all damages, losses, costs and expenses resulting therefrom.
*/
#ifndef _MI_RGN_H_
#define _MI_RGN_H_

#include "mi_common_datatype.h"
#include "mi_rgn_datatype.h"

#if __INFINITY6C__
MI_S32 MI_SYS_Init(MI_U16 u16SocId);
#define DEV 0,
#define SOC MI_U16 u16SocId,
#define DEV2 0
#define SOC2 MI_U16 u16SocId
#else
#define DEV
#define SOC
#define DEV2
#define SOC2
#endif

MI_S32 MI_RGN_Init(SOC MI_RGN_PaletteTable_t *pstPaletteTable);
MI_S32 MI_RGN_DeInit(SOC2);
MI_S32 MI_RGN_Create(SOC MI_RGN_HANDLE hHandle, MI_RGN_Attr_t *pstRegion);
MI_S32 MI_RGN_Destroy(SOC MI_RGN_HANDLE hHandle);
MI_S32 MI_RGN_GetAttr(SOC MI_RGN_HANDLE hHandle, MI_RGN_Attr_t *pstRegion);
MI_S32 MI_RGN_SetBitMap(SOC MI_RGN_HANDLE hHandle, MI_RGN_Bitmap_t *pstBitmap);
MI_S32 MI_RGN_AttachToChn(SOC MI_RGN_HANDLE hHandle, MI_RGN_ChnPort_t* pstChnPort, MI_RGN_ChnPortParam_t *pstChnAttr);
MI_S32 MI_RGN_DetachFromChn(SOC MI_RGN_HANDLE hHandle, MI_RGN_ChnPort_t *pstChnPort);
MI_S32 MI_RGN_GetDisplayAttr(SOC MI_RGN_HANDLE hHandle, MI_RGN_ChnPort_t *pstChnPort, MI_RGN_ChnPortParam_t *pstChnPortAttr);
MI_S32 MI_RGN_GetCanvasInfo(SOC MI_RGN_HANDLE hHandle, MI_RGN_CanvasInfo_t* pstCanvasInfo);
MI_S32 MI_RGN_UpdateCanvas(SOC MI_RGN_HANDLE hHandle);

#endif
