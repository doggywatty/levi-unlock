#ifndef _USBSTORAGE2_H_
#define _USBSTORAGE2_H_

#include <gctypes.h>

s32 USBStorage2_Init(void);
s32 USBStorage2_GetCapacity(u32 *_sector_size);
s32 USBStorage2_ReadSectors(u32 sector, u32 numSectors, void *buffer);
s32 USBStorage2_WriteSectors(u32 sector, u32 numSectors, const void *buffer);
s32 USBStorage2_Deinit(void);

#endif
