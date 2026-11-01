#include <stdio.h>
#include <ogcsys.h>
#include <ogc/usbstorage.h>

#include "usbstorage2.h"

s32 USBStorage2_Init(void)
{
    if (!__io_usbstorage.startup())
        return -1;
    if (!__io_usbstorage.isInserted()) {
        __io_usbstorage.shutdown();
        return -1;
    }
    return 0;
}

s32 USBStorage2_GetCapacity(u32 *_sector_size)
{
    u32 ss = __io_usbstorage_sector_size;
    if (!ss) ss = 512;
    if (_sector_size) *_sector_size = ss;
    return 1000000;
}

s32 USBStorage2_ReadSectors(u32 sector, u32 numSectors, void *buffer)
{
    if (!__io_usbstorage.readSectors(sector, numSectors, buffer))
        return -1;
    return 0;
}

s32 USBStorage2_WriteSectors(u32 sector, u32 numSectors, const void *buffer)
{
    if (!__io_usbstorage.writeSectors(sector, numSectors, (void *)buffer))
        return -1;
    return 0;
}

s32 USBStorage2_Deinit(void)
{
    __io_usbstorage.shutdown();
    return 0;
}
